#include "filesource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KIO/OpenFileManagerWindowJob>
#include <KIO/OpenUrlJob>

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QMimeDatabase>
#include <QLocale>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include <algorithm>

namespace
{

constexpr int CandidateLimit = 3000; // paths read from fd or plocate
constexpr int ResultLimit = 60;      // rows shown

QString tildePath(const QString &path)
{
    const QString home = QDir::homePath();
    return path.startsWith(home) ? u'~' + path.mid(home.size()) : path;
}

bool hasHiddenPart(const QString &path)
{
    return path.contains(QLatin1String("/."));
}

} // namespace

FileSource::FileSource(Launcher *launcher)
    : Source(launcher)
{
    m_delay.setSingleShot(true);
    m_delay.setInterval(120); // wait for a pause in typing
    connect(&m_delay, &QTimer::timeout, this, &FileSource::search);
}

QList<Action> FileSource::fileActions(Launcher *launcher, const QString &path)
{
    const QUrl url = QUrl::fromLocalFile(path);
    return {
        {tr("Open"), {}, {}, [url] { (new KIO::OpenUrlJob(url))->start(); }},
        {tr("Show in Folder"), QStringLiteral("document-open-folder"), QKeySequence(Qt::CTRL | Qt::Key_Return),
         [url] { KIO::highlightInFileManager({url}); }},
        {tr("Quick Look"), QStringLiteral("document-preview"), {},
         [launcher, path] { launcher->quickLook(path); }, true},
        {tr("Copy Path"), QStringLiteral("edit-copy-path"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
         [launcher, path] { launcher->copyText(path); }},
        {tr("Copy File"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F),
         [launcher, url] { launcher->copyUrls({url}); }},
    };
}

void FileSource::query(const QString &text)
{
    m_query = text.trimmed();
    if (m_query.isEmpty()) {
        m_delay.stop();
        QList<Item> items = recentFiles();
        if (items.isEmpty()) {
            Item hint;
            hint.title = tr("Type a file or folder name");
            hint.subtitle = tr("Searches your home folder; start with / to search the whole system");
            hint.icon = QIcon::fromTheme(QStringLiteral("system-search"));
            items << hint;
        }
        Q_EMIT resultsReady(items);
        return;
    }
    m_delay.start();
}

// Recently used files from the shared freedesktop list (written by Dolphin,
// KDE and GTK apps), newest first.
QList<Item> FileSource::recentFiles() const
{
    QFile f(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/recently-used.xbel"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    struct Recent {
        QString path;
        QDateTime when;
    };
    QList<Recent> recent;
    QXmlStreamReader xml(&f);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() != QLatin1String("bookmark"))
            continue;
        const QUrl url(xml.attributes().value(QLatin1String("href")).toString());
        const QDateTime when = QDateTime::fromString(xml.attributes().value(QLatin1String("modified")).toString(), Qt::ISODateWithMs);
        if (url.isLocalFile())
            recent << Recent{url.toLocalFile(), when};
    }
    std::sort(recent.begin(), recent.end(), [](const Recent &a, const Recent &b) { return a.when > b.when; });

    QMimeDatabase mimes;
    QList<Item> items;
    for (const Recent &r : std::as_const(recent)) {
        const QFileInfo info(r.path);
        if (!info.exists())
            continue;
        Item item;
        item.id = QStringLiteral("file:") + r.path;
        item.title = info.fileName();
        item.subtitle = tildePath(info.path()) + QStringLiteral(" · ") + QLocale().toString(r.when.toLocalTime(), QLocale::ShortFormat);
        item.kind = items.isEmpty() ? tr("Recent") : QString();
        const QMimeType mime = info.isDir() ? mimes.mimeTypeForName(QStringLiteral("inode/directory"))
                                            : mimes.mimeTypeForFile(r.path, QMimeDatabase::MatchExtension);
        item.icon = QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName()));
        item.filePath = r.path;
        item.score = 1000 - items.size();
        item.actions = fileActions(launcher(), r.path);
        items << item;
        if (items.size() == 40)
            break;
    }
    return items;
}

void FileSource::search()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
    }
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, &FileSource::finished);
    const QStringList words = m_query.split(u' ', Qt::SkipEmptyParts);
    static const QString fd = QStandardPaths::findExecutable(QStringLiteral("fd"));
    if (!fd.isEmpty() && !m_query.startsWith(u'/')) {
        // fd matches the last word against names; finished() checks the
        // other words against the whole path.
        QStringList args{QStringLiteral("--ignore-case"), QStringLiteral("--fixed-strings"), QStringLiteral("--absolute-path"),
                         QStringLiteral("--max-results"), QString::number(CandidateLimit)};
        for (const char *skip : {"node_modules", "__pycache__", "venv", ".venv"})
            args << QStringLiteral("--exclude") << QLatin1String(skip);
        if (m_query.startsWith(u'.'))
            args << QStringLiteral("--hidden");
        args << QStringLiteral("--") << words.last() << QDir::homePath();
        m_proc->start(fd, args);
        return;
    }
    // plocate: every word must appear in the path.
    QStringList args{QStringLiteral("--ignore-case"), QStringLiteral("--limit"), QString::number(CandidateLimit), QStringLiteral("--")};
    args << words;
    m_proc->start(QStringLiteral("plocate"), args);
}

void FileSource::finished()
{
    const QStringList paths = QString::fromUtf8(m_proc->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts);
    m_proc->deleteLater();
    m_proc = nullptr;

    const QString home = QDir::homePath() + u'/';
    const bool wantHidden = m_query.startsWith(u'.');
    const QString nameQuery = m_query.section(u' ', -1); // the last word usually names the file

    struct Candidate {
        QString path;
        qreal score;
    };
    QList<Candidate> candidates;
    const QStringList words = m_query.split(u' ', Qt::SkipEmptyParts);
    for (QString path : paths) {
        if (path.endsWith(u'/')) // fd marks folders with a trailing slash
            path.chop(1);
        if (!wantHidden && hasHiddenPart(path))
            continue;
        if (!std::all_of(words.cbegin(), words.cend(), [&](const QString &w) { return w.startsWith(u'/') || path.contains(w, Qt::CaseInsensitive); }))
            continue;
        const QString name = path.section(u'/', -1);
        qreal score = fuzzyScore(m_query, name);
        if (score <= 0)
            score = fuzzyScore(nameQuery, name) * 0.6; // words matched folders, last word the name
        if (score <= 0)
            score = 5; // matched only through folder names
        if (path.startsWith(home))
            score += 15;
        score -= path.count(u'/') * 0.5; // shallower first
        candidates << Candidate{path, score};
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });

    QMimeDatabase mimes;
    QList<Item> items;
    for (const Candidate &c : candidates) {
        const QFileInfo info(c.path);
        if (!info.exists()) // the database is up to a day old
            continue;
        Item item;
        item.id = QStringLiteral("file:") + c.path;
        item.title = info.fileName();
        item.subtitle = tildePath(info.path());
        item.kind = info.isDir() ? tr("Folder") : QString();
        const QMimeType mime = info.isDir() ? mimes.mimeTypeForName(QStringLiteral("inode/directory"))
                                            : mimes.mimeTypeForFile(c.path, QMimeDatabase::MatchExtension);
        item.icon = QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName()));
        item.filePath = c.path;
        item.score = c.score;
        item.actions = fileActions(launcher(), c.path);
        items << item;
        if (items.size() == ResultLimit)
            break;
    }
    if (items.isEmpty()) {
        Item none;
        none.title = tr("No files named “%1”").arg(m_query);
        none.subtitle = m_query.startsWith(u'/') ? tr("The system file database is refreshed once a day")
                                                 : tr("Searched your home folder; start with / to search the whole system");
        none.icon = QIcon::fromTheme(QStringLiteral("edit-find"));
        items << none;
    }
    Q_EMIT resultsReady(items);
}
