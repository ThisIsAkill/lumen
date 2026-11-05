#include "library.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>

namespace
{

QList<Library::QuickLink> defaultLinks()
{
    return {
        {QStringLiteral("Google"), QStringLiteral("g"), QStringLiteral("https://www.google.com/search?q={query}")},
        {QStringLiteral("YouTube"), QStringLiteral("yt"), QStringLiteral("https://www.youtube.com/results?search_query={query}")},
        {QStringLiteral("GitHub"), QStringLiteral("gh"), QStringLiteral("https://github.com/search?q={query}")},
        {QStringLiteral("Wikipedia"), QStringLiteral("w"), QStringLiteral("https://en.wikipedia.org/w/index.php?search={query}")},
        {QStringLiteral("ArchWiki"), QStringLiteral("aw"), QStringLiteral("https://wiki.archlinux.org/index.php?search={query}")},
        {QStringLiteral("Arch User Repository"), QStringLiteral("aur"), QStringLiteral("https://aur.archlinux.org/packages?K={query}")},
        {QStringLiteral("Downloads"), QStringLiteral("dl"), QStringLiteral("~/Downloads")},
    };
}

QList<Library::Snippet> defaultSnippets()
{
    return {
        {QStringLiteral("Current Date"), QStringLiteral("date"), QStringLiteral("{date}")},
        {QStringLiteral("Shrug"), QStringLiteral("shrug"), QStringLiteral("¯\\_(ツ)_/¯")},
    };
}

} // namespace

Library::Library(QObject *parent)
    : QObject(parent)
    , m_folder(configuredFolder())
    , m_file(m_folder + QStringLiteral("/library.json"))
{
    load();
    // Saving replaces the file, which ends a watch on it; watching the folder too
    // catches that and new files.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &Library::reloadIfChanged);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &Library::reloadIfChanged);
    QDir().mkpath(m_folder);
    m_watcher.addPath(m_folder);
    if (QFile::exists(m_file))
        m_watcher.addPath(m_file);
}

QString Library::defaultFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lumen");
}

QString Library::configuredFolder()
{
    const QString folder = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Sync")).readEntry("Folder");
    return folder.isEmpty() ? defaultFolder() : folder;
}

void Library::setFolder(const QString &requested)
{
    const QString folder = requested.isEmpty() ? defaultFolder() : QDir::cleanPath(requested);
    if (folder == m_folder)
        return;
    QDir().mkpath(folder + QStringLiteral("/icons"));
    // Bring along notes and favourite icons that are not there yet.
    for (const QString &name : QDir(m_folder + QStringLiteral("/icons")).entryList(QDir::Files))
        QFile::copy(m_folder + QStringLiteral("/icons/") + name, folder + QStringLiteral("/icons/") + name);
    QFile::copy(m_folder + QStringLiteral("/notes.md"), folder + QStringLiteral("/notes.md"));

    m_watcher.removePaths(m_watcher.files() + m_watcher.directories());
    const QString oldFile = m_file;
    m_folder = folder;
    m_file = folder + QStringLiteral("/library.json");
    if (QFile::exists(m_file))
        load(); // another computer's library is already there: use it
    else
        save();
    m_watcher.addPath(m_folder);
    m_watcher.addPath(m_file);

    KConfigGroup sync = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Sync"));
    sync.writeEntry("Folder", folder == defaultFolder() ? QString() : folder);
    sync.sync();
    Q_EMIT changed();
}

void Library::reloadIfChanged()
{
    if (!m_watcher.files().contains(m_file) && QFile::exists(m_file))
        m_watcher.addPath(m_file);
    QFile f(m_file);
    if (!f.open(QIODevice::ReadOnly) || f.readAll() == m_saved)
        return; // gone, or our own save
    load();
    Q_EMIT changed();
}

void Library::load()
{
    m_snippets.clear();
    m_links.clear();
    m_commands.clear();
    m_favourites.clear();
    m_aliases.clear();
    m_fallbacks = {QStringLiteral("lumen:files-for")};
    m_arrangements.clear();
    QFile f(m_file);
    if (!f.open(QIODevice::ReadOnly)) {
        m_snippets = defaultSnippets();
        m_links = defaultLinks();
        m_commands = {
            {QStringLiteral("Uptime"), {}, QStringLiteral("uptime -p"), Command::Notify},
            {QStringLiteral("Disk Space"), {}, QStringLiteral("df -h --output=target,avail,pcent -x tmpfs -x devtmpfs -x efivarfs | tail -n +2"), Command::List},
        };
        return;
    }
    m_saved = f.readAll();
    const QJsonObject root = QJsonDocument::fromJson(m_saved).object();
    for (const QJsonValue &v : root.value(QStringLiteral("snippets")).toArray()) {
        const QJsonObject o = v.toObject();
        m_snippets << Snippet{o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("keyword")).toString(),
                              o.value(QStringLiteral("text")).toString()};
    }
    for (const QJsonValue &v : root.value(QStringLiteral("favourites")).toArray()) {
        const QJsonObject o = v.toObject();
        m_favourites << Favourite{o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("title")).toString(),
                                  o.value(QStringLiteral("subtitle")).toString(), o.value(QStringLiteral("kind")).toString(),
                                  o.value(QStringLiteral("icon")).toString()};
    }
    for (const QJsonValue &v : root.value(QStringLiteral("aliases")).toArray()) {
        const QJsonObject o = v.toObject();
        m_aliases << Alias{o.value(QStringLiteral("alias")).toString(), o.value(QStringLiteral("id")).toString(),
                           o.value(QStringLiteral("title")).toString()};
    }
    for (const QJsonValue &v : root.value(QStringLiteral("commands")).toArray()) {
        const QJsonObject o = v.toObject();
        m_commands << Command{o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("keyword")).toString(),
                              o.value(QStringLiteral("command")).toString(), outputFromName(o.value(QStringLiteral("output")).toString())};
    }
    for (const QJsonValue &v : root.value(QStringLiteral("windowLayouts")).toArray())
        m_arrangements << WindowArrangement{v.toObject().value(QStringLiteral("name")).toString(), v.toObject().value(QStringLiteral("windows")).toArray()};
    if (root.contains(QStringLiteral("fallbacks"))) {
        m_fallbacks.clear();
        for (const QJsonValue &v : root.value(QStringLiteral("fallbacks")).toArray())
            m_fallbacks << v.toString();
    }
    for (const QJsonValue &v : root.value(QStringLiteral("quickLinks")).toArray()) {
        const QJsonObject o = v.toObject();
        m_links << QuickLink{o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("keyword")).toString(),
                             o.value(QStringLiteral("target")).toString()};
    }
}

void Library::save()
{
    QJsonArray snippets, links, commands, favourites, aliases, arrangements;
    for (const WindowArrangement &a : m_arrangements)
        arrangements << QJsonObject{{QStringLiteral("name"), a.name}, {QStringLiteral("windows"), a.windows}};
    for (const Favourite &f : m_favourites)
        favourites << QJsonObject{{QStringLiteral("id"), f.id}, {QStringLiteral("title"), f.title}, {QStringLiteral("subtitle"), f.subtitle},
                                  {QStringLiteral("kind"), f.kind}, {QStringLiteral("icon"), f.iconFile}};
    for (const Alias &a : m_aliases)
        aliases << QJsonObject{{QStringLiteral("alias"), a.alias}, {QStringLiteral("id"), a.id}, {QStringLiteral("title"), a.title}};
    for (const Command &c : m_commands)
        commands << QJsonObject{{QStringLiteral("name"), c.name}, {QStringLiteral("keyword"), c.keyword},
                                {QStringLiteral("command"), c.command}, {QStringLiteral("output"), outputName(c.output)}};
    for (const Snippet &s : m_snippets)
        snippets << QJsonObject{{QStringLiteral("name"), s.name}, {QStringLiteral("keyword"), s.keyword}, {QStringLiteral("text"), s.text}};
    for (const QuickLink &l : m_links)
        links << QJsonObject{{QStringLiteral("name"), l.name}, {QStringLiteral("keyword"), l.keyword}, {QStringLiteral("target"), l.target}};
    QDir().mkpath(QFileInfo(m_file).path());
    QSaveFile f(m_file);
    if (f.open(QIODevice::WriteOnly)) {
        m_saved = QJsonDocument(QJsonObject{{QStringLiteral("snippets"), snippets}, {QStringLiteral("quickLinks"), links},
                                            {QStringLiteral("commands"), commands}, {QStringLiteral("favourites"), favourites},
                                            {QStringLiteral("aliases"), aliases}, {QStringLiteral("fallbacks"), QJsonArray::fromStringList(m_fallbacks)},
                                            {QStringLiteral("windowLayouts"), arrangements}})
                      .toJson();
        f.write(m_saved);
        f.commit();
    }
}

void Library::setSnippets(const QList<Snippet> &snippets)
{
    m_snippets = snippets;
    save();
    Q_EMIT changed();
}

void Library::setFavourites(const QList<Favourite> &favourites)
{
    m_favourites = favourites;
    save();
    Q_EMIT changed();
}

void Library::setAliases(const QList<Alias> &aliases)
{
    m_aliases = aliases;
    save();
    Q_EMIT changed();
}

void Library::setArrangements(const QList<WindowArrangement> &arrangements)
{
    m_arrangements = arrangements;
    save();
    Q_EMIT changed();
}

void Library::setFallbacks(const QStringList &ids)
{
    m_fallbacks = ids;
    save();
    Q_EMIT changed();
}

void Library::setCommands(const QList<Command> &commands)
{
    m_commands = commands;
    save();
    Q_EMIT changed();
}

QString Library::outputName(Command::Output output)
{
    switch (output) {
    case Command::Silent:
        return QStringLiteral("silent");
    case Command::Copy:
        return QStringLiteral("copy");
    case Command::Paste:
        return QStringLiteral("paste");
    case Command::List:
        return QStringLiteral("list");
    case Command::Notify:
        break;
    }
    return QStringLiteral("notify");
}

Library::Command::Output Library::outputFromName(const QString &name)
{
    for (auto o : {Command::Silent, Command::Notify, Command::Copy, Command::Paste, Command::List})
        if (outputName(o) == name)
            return o;
    return Command::Notify;
}

void Library::setQuickLinks(const QList<QuickLink> &links)
{
    m_links = links;
    save();
    Q_EMIT changed();
}

namespace
{

// "+3d", "-2h", "+1w-1d": moves `time` by each step.
QDateTime shifted(QDateTime time, const QString &offset)
{
    static const QRegularExpression step(QStringLiteral(R"(([+-])\s*(\d+)\s*([smhdwMy]))"));
    for (auto it = step.globalMatch(offset); it.hasNext();) {
        const auto m = it.next();
        const int n = m.captured(2).toInt() * (m.captured(1) == QLatin1String("-") ? -1 : 1);
        switch (m.captured(3).at(0).unicode()) {
        case 's':
            time = time.addSecs(n);
            break;
        case 'm':
            time = time.addSecs(n * 60);
            break;
        case 'h':
            time = time.addSecs(n * 3600);
            break;
        case 'd':
            time = time.addDays(n);
            break;
        case 'w':
            time = time.addDays(n * 7);
            break;
        case 'M':
            time = time.addMonths(n);
            break;
        default:
            time = time.addYears(n);
            break;
        }
    }
    return time;
}

} // namespace

Library::Expanded Library::expandSnippet(const QString &text, const QHash<QString, QString> &values, const std::function<QString(int)> &clipboard,
                                         const QDateTime &now)
{
    // Arguments first: their values are the user's text, never placeholders.
    const QChar cursorMark(0xE000); // a private-use character marks {cursor} until the end
    QString filled = Arguments::fill(text, values);
    static const QRegularExpression placeholder(QStringLiteral(R"(\{(date|time|datetime|day|clipboard|uuid|cursor)((?:\s+[^{}]*)?)\})"));
    static const QRegularExpression attribute(QStringLiteral(R"re(([A-Za-z]+)\s*=\s*(?:"([^"]*)"|'([^']*)'|(\S+)))re"));
    const QLocale locale;
    QString out;
    qsizetype last = 0;
    for (auto it = placeholder.globalMatch(filled); it.hasNext();) {
        const auto m = it.next();
        out += filled.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        // Attributes, and a bare offset ("{date +3d}").
        QHash<QString, QString> attrs;
        QString rest = m.captured(2);
        for (auto a = attribute.globalMatch(rest); a.hasNext();) {
            const auto am = a.next();
            attrs.insert(am.captured(1).toLower(), am.hasCaptured(2) ? am.captured(2) : am.hasCaptured(3) ? am.captured(3) : am.captured(4));
        }
        rest.remove(attribute);
        const QString offset = attrs.value(QStringLiteral("offset"), rest.trimmed());
        const QString format = attrs.value(QStringLiteral("format"));
        const QString kind = m.captured(1);
        const QDateTime when = shifted(now, offset);
        if (kind == QLatin1String("date"))
            out += format.isEmpty() ? locale.toString(when.date(), QLocale::ShortFormat) : locale.toString(when, format);
        else if (kind == QLatin1String("time"))
            out += format.isEmpty() ? locale.toString(when.time(), QLocale::ShortFormat) : locale.toString(when, format);
        else if (kind == QLatin1String("datetime"))
            out += format.isEmpty() ? locale.toString(when, QLocale::ShortFormat) : locale.toString(when, format);
        else if (kind == QLatin1String("day"))
            out += locale.dayName(when.date().dayOfWeek());
        else if (kind == QLatin1String("uuid"))
            out += QUuid::createUuid().toString(QUuid::WithoutBraces);
        else if (kind == QLatin1String("cursor"))
            out += cursorMark;
        else // clipboard
            out += clipboard ? clipboard(offset.toInt()) : offset.toInt() == 0 ? QGuiApplication::clipboard()->text() : QString();
    }
    out += filled.mid(last);

    Expanded result;
    const qsizetype mark = out.indexOf(cursorMark);
    if (mark >= 0) {
        result.cursorBack = int(out.mid(mark + 1).remove(cursorMark).toUcs4().size());
        out.remove(cursorMark);
    }
    result.text = out;
    return result;
}

QUrl Library::resolve(const QuickLink &link, const QHash<QString, QString> &values)
{
    QString target = link.target.trimmed();
    if (target.startsWith(u'~'))
        target = QDir::homePath() + target.mid(1);
    if (target.startsWith(u'/'))
        return QUrl::fromLocalFile(Arguments::fill(target, values));
    target = Arguments::fill(target, values, [](const QString &v) { return QString::fromUtf8(QUrl::toPercentEncoding(v)); });
    return QUrl::fromUserInput(target);
}

QUrl Library::resolve(const QuickLink &link, const QString &query)
{
    const QList<Argument> arguments = link.arguments();
    return resolve(link, arguments.isEmpty() ? QHash<QString, QString>() : QHash<QString, QString>{{arguments.first().name, query}});
}
