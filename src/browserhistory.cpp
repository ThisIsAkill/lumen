#include "browserhistory.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KIO/OpenUrlJob>
#include <KSharedConfig>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>

namespace
{

constexpr int MaxPerDatabase = 4000; // most relevant visits read from each browser
constexpr int MaxShown = 60;
constexpr int RefreshSeconds = 180;  // a newer copy at most this often

QDateTime lastLoaded;

// Reads `sql` from a copy of an SQLite file (and its write-ahead log, which
// holds the latest visits while the browser runs).
template<typename Row>
QList<BrowserHistory::Visit> readCopy(const QString &file, const QString &sql, Row row)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {};
    const QString copy = dir.filePath(QStringLiteral("history.sqlite"));
    if (!QFile::copy(file, copy))
        return {};
    QFile::copy(file + QStringLiteral("-wal"), copy + QStringLiteral("-wal"));
    QList<BrowserHistory::Visit> out;
    const QString connection = QUuid::createUuid().toString();
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(copy);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql)) {
                while (q.next()) {
                    BrowserHistory::Visit v = row(q);
                    if (!v.url.isEmpty())
                        out << v;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return out;
}

// Firefox-style profiles listed in profiles.ini under `root`.
QStringList firefoxProfiles(const QString &root)
{
    const QString ini = QDir(root).filePath(QStringLiteral("profiles.ini"));
    if (!QFile::exists(ini))
        return {};
    QStringList profiles;
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(ini, KConfig::SimpleConfig);
    for (const QString &group : config->groupList()) {
        if (!group.startsWith(QLatin1String("Profile")))
            continue;
        const KConfigGroup g = config->group(group);
        const QString path = g.readEntry("Path");
        profiles << (g.readEntry("IsRelative", 1) ? QDir(root).filePath(path) : path);
    }
    return profiles;
}

} // namespace

BrowserHistory::BrowserHistory(Launcher *launcher, bool view)
    : Source(launcher)
    , m_view(view)
{
    connect(&m_loading, &QFutureWatcher<QList<Visit>>::finished, this, [this] {
        visits() = m_loading.result();
        lastLoaded = QDateTime::currentDateTime();
        if (!m_query.isEmpty() || m_view)
            publish();
    });
}

QList<BrowserHistory::Visit> &BrowserHistory::visits()
{
    static QList<Visit> list;
    return list;
}

QList<BrowserHistory::Database> BrowserHistory::databases()
{
    QList<Database> out;
    const QString home = QDir::homePath();
    const QString config = home + QStringLiteral("/.config");
    // Firefox and its relatives: a profiles.ini, and places.sqlite per profile.
    const QList<std::tuple<QString, QString, QStringList>> firefoxes{
        {QStringLiteral("Firefox"), QStringLiteral("firefox"), {config + QStringLiteral("/mozilla/firefox"), home + QStringLiteral("/.mozilla/firefox")}},
        {QStringLiteral("LibreWolf"), QStringLiteral("librewolf"), {home + QStringLiteral("/.librewolf"), config + QStringLiteral("/librewolf/librewolf")}},
        {QStringLiteral("Zen"), QStringLiteral("zen-browser"), {home + QStringLiteral("/.zen"), config + QStringLiteral("/zen")}},
        {QStringLiteral("Floorp"), QStringLiteral("floorp"), {home + QStringLiteral("/.floorp")}},
        {QStringLiteral("Waterfox"), QStringLiteral("waterfox"), {home + QStringLiteral("/.waterfox")}},
    };
    for (const auto &[name, icon, roots] : firefoxes)
        for (const QString &root : roots)
            for (const QString &profile : firefoxProfiles(root))
                if (const QString file = QDir(profile).filePath(QStringLiteral("places.sqlite")); QFile::exists(file))
                    out << Database{name, icon, file, true};
    // Chromium-based: "Default" and "Profile N" folders with a History file.
    const QList<std::tuple<QString, QString, QString>> chromiums{
        {QStringLiteral("Brave"), QStringLiteral("brave-browser"), QStringLiteral("BraveSoftware/Brave-Browser")},
        {QStringLiteral("Chrome"), QStringLiteral("google-chrome"), QStringLiteral("google-chrome")},
        {QStringLiteral("Chromium"), QStringLiteral("chromium"), QStringLiteral("chromium")},
        {QStringLiteral("Vivaldi"), QStringLiteral("vivaldi"), QStringLiteral("vivaldi")},
        {QStringLiteral("Edge"), QStringLiteral("microsoft-edge"), QStringLiteral("microsoft-edge")},
        {QStringLiteral("Opera"), QStringLiteral("opera"), QStringLiteral("opera")},
    };
    for (const auto &[name, icon, folder] : chromiums) {
        const QDir dir(config + u'/' + folder);
        for (const QString &profile : dir.entryList({QStringLiteral("Default"), QStringLiteral("Profile *")}, QDir::Dirs))
            if (const QString file = dir.filePath(profile + QStringLiteral("/History")); QFile::exists(file))
                out << Database{name, icon, file, false};
    }
    return out;
}

QList<BrowserHistory::Visit> BrowserHistory::readFirefox(const QString &placesFile, const QString &browser)
{
    // Times are microseconds since 1970; frecency is Firefox's own ranking.
    return readCopy(placesFile,
                    QStringLiteral("SELECT url, title, visit_count, last_visit_date FROM moz_places WHERE hidden = 0 AND last_visit_date IS NOT NULL "
                                   "AND url LIKE 'http%' ORDER BY frecency DESC LIMIT %1")
                        .arg(MaxPerDatabase),
                    [browser](const QSqlQuery &q) {
                        return Visit{q.value(0).toString(), q.value(1).toString(), browser, {},
                                     QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong() / 1000), q.value(2).toInt()};
                    });
}

QList<BrowserHistory::Visit> BrowserHistory::readChromium(const QString &historyFile, const QString &browser)
{
    // Times are microseconds since 1601-01-01 (Windows' epoch).
    constexpr qint64 FromWindowsEpoch = 11644473600000LL; // ms
    return readCopy(historyFile,
                    QStringLiteral("SELECT url, title, visit_count, last_visit_time FROM urls WHERE hidden = 0 AND url LIKE 'http%' "
                                   "ORDER BY last_visit_time DESC LIMIT %1")
                        .arg(MaxPerDatabase),
                    [browser](const QSqlQuery &q) {
                        return Visit{q.value(0).toString(), q.value(1).toString(), browser, {},
                                     QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong() / 1000 - FromWindowsEpoch), q.value(2).toInt()};
                    });
}

QList<BrowserHistory::Visit> BrowserHistory::loadAll()
{
    QHash<QString, Visit> byUrl;
    for (const Database &db : databases()) {
        QList<Visit> visits = db.firefox ? readFirefox(db.file, db.browser) : readChromium(db.file, db.browser);
        for (Visit &v : visits) {
            v.icon = db.icon;
            const auto it = byUrl.constFind(v.url);
            if (it == byUrl.constEnd() || it->last < v.last)
                byUrl.insert(v.url, v);
        }
    }
    QList<Visit> all = byUrl.values();
    std::sort(all.begin(), all.end(), [](const Visit &a, const Visit &b) { return a.last > b.last; });
    return all;
}

void BrowserHistory::sessionStarted()
{
    if (!m_view) // one of the two loads for both
        refresh();
}

void BrowserHistory::refresh()
{
    if (m_loading.isRunning() || (lastLoaded.isValid() && lastLoaded.secsTo(QDateTime::currentDateTime()) < RefreshSeconds))
        return;
    m_loading.setFuture(QtConcurrent::run(&BrowserHistory::loadAll));
}

void BrowserHistory::query(const QString &text)
{
    m_query = text.trimmed();
    if (m_view)
        refresh();
    publish();
}

void BrowserHistory::publish()
{
    QList<Item> items;
    // The main search only offers history for a few typed letters.
    if (!m_view && m_query.size() < 2) {
        Q_EMIT resultsReady(items);
        return;
    }
    Launcher *l = launcher();
    const QList<Visit> &all = visits();
    for (int i = 0; i < all.size(); ++i) {
        const Visit &v = all.at(i);
        const QUrl url(v.url);
        const QString host = url.host().remove(QRegularExpression(QStringLiteral("^www\\.")));
        qreal score;
        if (m_query.isEmpty()) {
            score = 1000 - i * 0.01; // newest first
        } else {
            score = std::max(fuzzyScore(m_query, v.title), fuzzyScore(m_query, host) * 0.95);
            if (score <= 0)
                continue;
            // A little for pages visited often or lately; below apps and commands in the main search.
            score += std::min(v.visits, 50) * 0.04 - std::min<qint64>(v.last.daysTo(QDateTime::currentDateTime()), 60) * 0.03;
            if (!m_view)
                score = std::min(score * 0.8, 85.0);
        }
        Item item;
        item.id = QStringLiteral("history:") + v.url;
        item.title = v.title.isEmpty() ? host : v.title;
        item.subtitle = host + url.path().left(60);
        item.kind = m_view ? v.browser : tr("History · %1").arg(v.browser);
        item.icon = QIcon::fromTheme(v.icon, QIcon::fromTheme(QStringLiteral("internet-web-browser")));
        item.score = score;
        const QString link = v.url, title = item.title;
        item.actions << Action{tr("Open in Browser"), {}, {}, [link] { (new KIO::OpenUrlJob(QUrl(link)))->start(); }};
        item.actions << Action{tr("Copy Link"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), [l, link] { l->copyText(link); }};
        item.actions << Action{tr("Copy as Markdown Link"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M),
                               [l, link, title] { l->copyText(QStringLiteral("[%1](%2)").arg(title, link)); }};
        items << item;
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    if (items.size() > (m_view ? MaxShown * 3 : 6))
        items.resize(m_view ? MaxShown * 3 : 6);
    if (m_view && items.isEmpty() && all.isEmpty() && !m_loading.isRunning()) {
        Item none;
        none.title = tr("No browser history found");
        none.subtitle = tr("Firefox, Chrome, Brave, Vivaldi, Edge and Opera are read from their profile folders");
        none.icon = QIcon::fromTheme(QStringLiteral("internet-web-browser"));
        items << none;
    }
    Q_EMIT resultsReady(items);
}
