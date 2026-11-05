#include "scriptsource.h"
#include "filesource.h"
#include "fuzzy.h"
#include "launcher.h"
#include "library.h"
#include "outputview.h"

#include <chrome.h>

#include <KIO/OpenUrlJob>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QStandardPaths>

#include <algorithm>

namespace
{

constexpr int CommandTimeout = 30000;   // ms before a custom command is stopped
constexpr int ExtensionTimeout = 3000;  // ms for an extension to answer a search
constexpr int RunTimeout = 60000;       // ms for a command-style extension
constexpr int MaxListed = 200;

QString shellQuote(const QString &text)
{
    QString quoted = text;
    quoted.replace(u'\'', QLatin1String("'\\''"));
    return u'\'' + quoted + u'\'';
}

// The action a JSON object describes ({"copy": …}, {"open": …}, …), for a
// result titled `title`. Sets *filePath for a local file, so Space previews it.
std::optional<Action> actionFrom(const QJsonObject &o, Launcher *l, const ScriptSource::Extension &extension, const QString &title,
                                 QString *filePath)
{
    const QString label = o.value(QStringLiteral("title")).toString();
    const QKeySequence keys = QKeySequence::fromString(o.value(QStringLiteral("shortcut")).toString(), QKeySequence::PortableText);
    if (o.contains(QStringLiteral("open"))) {
        const QUrl url = QUrl::fromUserInput(o.value(QStringLiteral("open")).toString(), QDir::homePath(), QUrl::AssumeLocalFile);
        if (url.isLocalFile() && filePath && filePath->isEmpty())
            *filePath = url.toLocalFile();
        return Action{label.isEmpty() ? QObject::tr("Open") : label, QStringLiteral("document-open"), keys,
                      [url] { (new KIO::OpenUrlJob(url))->start(); }};
    }
    if (o.contains(QStringLiteral("paste")) && l->canPaste()) {
        const QString text = o.value(QStringLiteral("paste")).toString();
        return Action{label.isEmpty() ? QObject::tr("Paste") : label, QStringLiteral("edit-paste"), keys, [l, text] { l->pasteText(text); }};
    }
    if (o.contains(QStringLiteral("copy")) || o.contains(QStringLiteral("paste"))) {
        const QString text = o.value(o.contains(QStringLiteral("copy")) ? QStringLiteral("copy") : QStringLiteral("paste")).toString();
        return Action{label.isEmpty() ? QObject::tr("Copy to Clipboard") : label, QStringLiteral("edit-copy"), keys, [l, text] { l->copyText(text); }};
    }
    if (o.contains(QStringLiteral("run"))) {
        QStringList argv;
        for (const QJsonValue &a : o.value(QStringLiteral("run")).toArray())
            argv << a.toString();
        if (argv.isEmpty())
            return std::nullopt;
        return Action{label.isEmpty() ? QObject::tr("Run") : label, QStringLiteral("system-run"), keys,
                      [argv] { QProcess::startDetached(argv.first(), argv.mid(1)); }};
    }
    if (o.contains(QStringLiteral("push"))) {
        // A nested list: `EXT open ID TEXT`.
        const QString id = o.value(QStringLiteral("push")).toString();
        Action push{label.isEmpty() ? QObject::tr("Open") : label, QStringLiteral("go-next"), keys, [l, extension, id, title] {
            l->pushView(new ExtensionView(l, extension, {QStringLiteral("open"), id}, {}, title));
        }, true};
        return push;
    }
    return std::nullopt;
}

} // namespace

ScriptSource::ScriptSource(Launcher *launcher, Library *library)
    : Source(launcher)
    , m_library(library)
{
    QDir().mkpath(extensionsFolder());
    m_watcher.addPath(extensionsFolder());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &ScriptSource::scanExtensions);
    scanExtensions();
    m_delay.setSingleShot(true);
    m_delay.setInterval(150);
    connect(&m_delay, &QTimer::timeout, this, &ScriptSource::searchExtension);
}

QString ScriptSource::extensionsFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lumen/extensions");
}

QIcon ScriptSource::iconFrom(const QString &spec, const QString &fallback, const QString &baseDir)
{
    const QIcon fallbackIcon = QIcon::fromTheme(fallback);
    if (spec.isEmpty() || spec.startsWith(QLatin1String("http://")) || spec.startsWith(QLatin1String("https://")))
        return fallbackIcon; // no downloading
    QString path = spec.startsWith(u'~') ? QDir::homePath() + spec.mid(1) : spec;
    if (!baseDir.isEmpty() && QFileInfo(path).isRelative())
        path = QDir(baseDir).filePath(path);
    if (QFileInfo(path).isFile())
        return QIcon(path);
    // An emoji (or any short non-name text) is drawn as the icon.
    if (spec.size() <= 8 && !spec.at(0).isLetterOrNumber()) {
        QPixmap pixmap(64, 64);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        QFont font = p.font();
        font.setPixelSize(46);
        p.setFont(font);
        p.drawText(pixmap.rect(), Qt::AlignCenter, spec);
        p.end();
        return QIcon(pixmap);
    }
    return QIcon::fromTheme(spec, fallbackIcon);
}

Argument ScriptSource::argumentFrom(const QJsonObject &o, int index)
{
    Argument a;
    a.name = o.value(QStringLiteral("name")).toString(QStringLiteral("argument%1").arg(index + 1));
    a.placeholder = o.value(QStringLiteral("placeholder")).toString();
    a.optional = o.value(QStringLiteral("optional")).toBool();
    a.defaultValue = o.value(QStringLiteral("default")).toString();
    const QString type = o.value(QStringLiteral("type")).toString();
    a.secret = type == QLatin1String("password") || o.value(QStringLiteral("secret")).toBool() || o.value(QStringLiteral("secure")).toBool();
    // Choices: ["a", "b"] or [{"title": "A", "value": "a"}] (Raycast's "data").
    const QJsonArray options = o.contains(QStringLiteral("options")) ? o.value(QStringLiteral("options")).toArray() : o.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &v : options) {
        if (v.isObject()) {
            a.options << v.toObject().value(QStringLiteral("title")).toString();
            a.values << v.toObject().value(QStringLiteral("value")).toString(a.options.last());
        } else {
            a.options << v.toString();
            a.values << v.toString();
        }
    }
    return a;
}

QStringList ScriptSource::argumentFlags(const QList<Argument> &arguments, const QStringList &values)
{
    QStringList flags;
    for (int i = 0; i < arguments.size(); ++i)
        flags << QStringLiteral("--arg") << arguments.at(i).name + u'=' + values.value(i);
    return flags;
}

QList<Item> ScriptSource::resultsFrom(const QJsonArray &results, Launcher *l, const Extension &extension)
{
    QList<Item> items;
    for (const QJsonValue &v : results) {
        const QJsonObject r = v.toObject();
        Item item;
        item.title = r.value(QStringLiteral("title")).toString();
        if (item.title.isEmpty())
            continue;
        item.subtitle = r.value(QStringLiteral("subtitle")).toString();
        item.id = QStringLiteral("ext-result:") + extension.name + u':' + r.value(QStringLiteral("id")).toString(item.title);
        item.kind = r.value(QStringLiteral("accessory")).toString(extension.name);
        item.icon = iconFrom(r.value(QStringLiteral("icon")).toString(), QStringLiteral("plugins"), QFileInfo(extension.path).path());
        item.score = 90 - items.size() * 0.01; // keep the extension's order
        if (r.contains(QStringLiteral("detail"))) {
            item.previewText = r.value(QStringLiteral("detail")).toString();
            item.markdown = true;
        }
        // The result's own keys are its first action; "actions" adds more.
        if (const auto first = actionFrom(r, l, extension, item.title, &item.filePath))
            item.actions << *first;
        for (const QJsonValue &a : r.value(QStringLiteral("actions")).toArray())
            if (const auto action = actionFrom(a.toObject(), l, extension, item.title, &item.filePath))
                item.actions << *action;
        if (!item.filePath.isEmpty())
            item.actions << FileSource::fileActions(l, item.filePath).mid(1); // Show in Folder, Copy Path, …
        items << item;
        if (items.size() == MaxListed)
            break;
    }
    return items;
}

void ScriptSource::scanExtensions()
{
    m_extensions.clear();
    const QFileInfoList files = QDir(extensionsFolder()).entryInfoList(QDir::Files | QDir::Executable, QDir::Name);
    for (const QFileInfo &file : files) {
        QProcess proc;
        proc.start(file.absoluteFilePath(), {QStringLiteral("info")});
        if (!proc.waitForFinished(1000) || proc.exitCode() != 0) {
            proc.kill();
            continue;
        }
        const QJsonObject info = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
        Extension e;
        e.path = file.absoluteFilePath();
        e.name = info.value(QStringLiteral("name")).toString(file.baseName());
        e.keyword = info.value(QStringLiteral("keyword")).toString();
        e.description = info.value(QStringLiteral("description")).toString();
        e.icon = info.value(QStringLiteral("icon")).toString();
        e.placeholder = info.value(QStringLiteral("placeholder")).toString();
        e.runs = info.value(QStringLiteral("mode")).toString() == QLatin1String("run");
        e.preview = info.value(QStringLiteral("preview")).toBool();
        const QJsonArray arguments = info.value(QStringLiteral("arguments")).toArray();
        for (int i = 0; i < arguments.size(); ++i)
            e.arguments << argumentFrom(arguments.at(i).toObject(), i);
        if (!e.keyword.isEmpty() || !e.name.isEmpty())
            m_extensions << e;
    }
}

// ------------------------------------------------------------ commands

void ScriptSource::runCommand(const QString &name, const QString &shellLine, int output)
{
    auto *proc = new QProcess(this);
    Launcher *l = launcher();
    const auto mode = Library::Command::Output(output);
    connect(proc, &QProcess::finished, this, [this, proc, l, name, mode](int code) {
        proc->deleteLater();
        const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        if (code != 0) {
            const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed().section(u'\n', 0, 0);
            l->notify(err.isEmpty() ? tr("%1 failed").arg(name) : err);
            return;
        }
        switch (mode) {
        case Library::Command::Silent:
            break;
        case Library::Command::Notify:
            l->notify(out.isEmpty() ? tr("%1: done").arg(name) : out.section(u'\n', 0, 0));
            break;
        case Library::Command::Copy:
            l->copyText(out);
            break;
        case Library::Command::Paste:
            l->pasteText(out);
            break;
        case Library::Command::List: {
            // Each line becomes a result: files open, links open, text copies.
            m_listed.clear();
            m_listedFor = m_query;
            const QStringList lines = out.split(u'\n', Qt::SkipEmptyParts);
            for (const QString &line : lines.mid(0, MaxListed)) {
                Item item;
                item.id = QStringLiteral("cmd-line:") + line;
                item.title = line.simplified();
                item.kind = name;
                item.score = 90 - m_listed.size() * 0.01; // keep the program's order
                const QString path = line.trimmed();
                if (path.startsWith(u'/') && QFileInfo::exists(path)) {
                    item.filePath = path;
                    item.icon = QIcon::fromTheme(QStringLiteral("document-open"));
                    item.actions = FileSource::fileActions(l, path);
                } else if (path.startsWith(QLatin1String("http://")) || path.startsWith(QLatin1String("https://"))) {
                    item.icon = QIcon::fromTheme(QStringLiteral("internet-web-browser"));
                    item.actions << Action{tr("Open"), {}, {}, [path] { (new KIO::OpenUrlJob(QUrl(path)))->start(); }};
                } else {
                    item.icon = QIcon::fromTheme(QStringLiteral("utilities-terminal"));
                    item.actions << Action{tr("Copy to Clipboard"), {}, {}, [l, line] { l->copyText(line.trimmed()); }};
                }
                m_listed << item;
            }
            publish();
            break;
        }
        }
    });
    QTimer::singleShot(CommandTimeout, proc, [proc] { proc->kill(); });
    proc->start(QStringLiteral("sh"), {QStringLiteral("-c"), shellLine});
}

QList<Item> ScriptSource::commandItems(const QString &text) const
{
    QList<Item> items;
    Launcher *l = launcher();
    auto self = const_cast<ScriptSource *>(this);
    const QString keyword = text.section(u' ', 0, 0);
    const QString argument = text.section(u' ', 1).trimmed();
    for (const Library::Command &c : m_library->commands()) {
        Item item;
        item.id = QStringLiteral("cmd:") + c.name;
        item.kind = c.keyword.isEmpty() ? tr("Command") : tr("Command · %1").arg(c.keyword);
        item.icon = QIcon::fromTheme(QStringLiteral("utilities-terminal"));
        const bool listed = c.output == Library::Command::List;
        const int output = int(c.output);
        const QList<Argument> arguments = c.arguments();
        const QString name = c.name, command = c.command;
        // Asks for the arguments (the first pre-filled), then runs.
        auto ask = [&](const QStringList &initial) {
            Item shown = item;
            shown.title = c.name;
            shown.subtitle = c.command;
            Action action{arguments.size() == 1 ? tr("Enter %1").arg(arguments.first().label()) : tr("Enter Arguments"), {}, {},
                          [l, self, shown, arguments, initial, name, command, output] {
                              l->askArguments(shown, arguments, initial, [l, self, arguments, name, command, output](const QStringList &values) {
                                  self->runCommand(name, Arguments::fill(command, Arguments::byName(arguments, values), shellQuote), output);
                                  if (Library::Command::Output(output) != Library::Command::List)
                                      l->hideLauncher();
                              });
                          },
                          true};
            action.asksArguments = true;
            return action;
        };
        if (!arguments.isEmpty() && !c.keyword.isEmpty() && keyword.compare(c.keyword, Qt::CaseInsensitive) == 0) {
            item.title = argument.isEmpty() ? c.name : tr("%1: %2").arg(c.name, argument);
            item.subtitle = argument.isEmpty() ? tr("Type the argument") : c.command;
            item.score = 120;
            if (arguments.size() > 1) {
                item.actions << ask({argument});
            } else if (!argument.isEmpty()) {
                const QString line = Arguments::fill(c.command, {{arguments.first().name, argument}}, shellQuote);
                item.actions << Action{tr("Run"), {}, {}, [self, name, line, output] { self->runCommand(name, line, output); }, listed};
            }
            items << item;
            continue;
        }
        qreal score = std::max(fuzzyScore(text, c.name), fuzzyScore(text, c.keyword));
        if (score <= 0)
            continue;
        item.title = c.name;
        item.subtitle = c.command;
        item.score = score;
        if (arguments.size() == 1 && !c.keyword.isEmpty()) {
            const QString prefix = c.keyword + u' ';
            item.actions << Action{tr("Enter Argument"), {}, {}, [l, prefix] { l->setQuery(prefix); }, true};
        } else if (!arguments.isEmpty()) {
            item.actions << ask({});
        } else {
            item.actions << Action{tr("Run"), {}, {}, [self, name, command, output] { self->runCommand(name, command, output); }, listed};
        }
        items << item;
    }
    return items;
}

// ---------------------------------------------------------- extensions

void ScriptSource::openExtension(const Extension &extension, const QString &text)
{
    Launcher *l = launcher();
    Item shown;
    shown.title = extension.name;
    shown.subtitle = extension.description;
    shown.icon = iconFrom(extension.icon, QStringLiteral("plugins"), QFileInfo(extension.path).path());
    l->askArguments(shown, extension.arguments, {}, [this, l, extension, text](const QStringList &values) {
        const QStringList flags = argumentFlags(extension.arguments, values);
        if (extension.runs)
            runExtension(extension, flags);
        else
            l->pushView(new ExtensionView(l, extension, {QStringLiteral("search")}, flags, extension.name), text);
    });
}

void ScriptSource::runExtension(const Extension &extension, const QStringList &flags)
{
    Launcher *l = launcher();
    l->notify(tr("Running %1…").arg(extension.name));
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [l, proc, extension](int code) {
        proc->deleteLater();
        const QByteArray out = proc->readAllStandardOutput();
        if (code != 0) {
            const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed().section(u'\n', -1);
            l->notify(err.isEmpty() ? tr("%1 failed").arg(extension.name) : err);
            return;
        }
        // {"message"|"copy"|"paste"|"open"|"detail"|"text": …}, or plain text.
        const QJsonObject o = QJsonDocument::fromJson(out).object();
        const QString text = o.isEmpty() ? OutputView::plain(QString::fromUtf8(out)).trimmed() : QString();
        if (o.contains(QStringLiteral("detail")) || o.contains(QStringLiteral("text")) || text.contains(u'\n')) {
            auto *view = new OutputView(l, extension.name, extension.icon);
            if (o.contains(QStringLiteral("detail")))
                view->setText(o.value(QStringLiteral("detail")).toString(), true);
            else
                view->setText(o.isEmpty() ? text : o.value(QStringLiteral("text")).toString());
            l->pushView(view);
            return;
        }
        if (const auto action = actionFrom(o, l, extension, extension.name, nullptr)) {
            if (!action->keepOpen)
                l->hideLauncher();
            action->run();
            return;
        }
        l->hideLauncher();
        const QString message = o.value(QStringLiteral("message")).toString(text);
        l->notify(message.isEmpty() ? tr("%1: done").arg(extension.name) : message);
    });
    QTimer::singleShot(RunTimeout, proc, [proc] { proc->kill(); });
    proc->start(extension.path, QStringList{QStringLiteral("run")} + flags);
}

QList<Item> ScriptSource::extensionItems(const QString &text) const
{
    QList<Item> items;
    auto self = const_cast<ScriptSource *>(this);
    const QString keyword = text.section(u' ', 0, 0);
    for (const Extension &e : m_extensions) {
        const bool inline_ = !e.runs && e.arguments.isEmpty() && !e.keyword.isEmpty();
        if (inline_ && keyword.compare(e.keyword, Qt::CaseInsensitive) == 0 && text.contains(u' '))
            continue; // its results are shown instead
        const qreal score = std::max(fuzzyScore(text, e.name), e.keyword.isEmpty() ? 0 : fuzzyScore(text, e.keyword));
        if (score <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("ext:") + e.name;
        item.title = e.name;
        item.subtitle = e.description;
        item.kind = e.keyword.isEmpty() ? tr("Extension") : tr("Extension · %1").arg(e.keyword);
        item.icon = iconFrom(e.icon, QStringLiteral("plugins"), QFileInfo(e.path).path());
        item.score = score;
        Action open{e.runs ? tr("Run") : tr("Open"), {}, {}, [self, e] { self->openExtension(e, {}); }, true};
        open.asksArguments = !e.arguments.isEmpty();
        item.actions << open;
        items << item;
    }
    return items;
}

void ScriptSource::searchExtension()
{
    const QString keyword = m_query.section(u' ', 0, 0);
    const auto it = std::find_if(m_extensions.cbegin(), m_extensions.cend(), [&](const Extension &e) {
        return !e.runs && e.arguments.isEmpty() && e.keyword.compare(keyword, Qt::CaseInsensitive) == 0;
    });
    if (it == m_extensions.cend())
        return;
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
    }
    m_proc = new QProcess(this);
    const QString forQuery = m_query;
    const Extension extension = *it;
    connect(m_proc, &QProcess::finished, this, [this, forQuery, extension] {
        const QJsonArray results = QJsonDocument::fromJson(m_proc->readAllStandardOutput()).array();
        m_proc->deleteLater();
        m_proc = nullptr;
        if (forQuery != m_query)
            return;
        m_listed = resultsFrom(results, launcher(), extension);
        m_listedFor = forQuery;
        publish();
    });
    QTimer::singleShot(ExtensionTimeout, m_proc, [proc = m_proc] { proc->kill(); });
    m_proc->start(it->path, {QStringLiteral("search"), m_query.section(u' ', 1).trimmed()});
}

// --------------------------------------------------------------- query

void ScriptSource::query(const QString &text)
{
    m_query = text.trimmed();
    if (m_query.isEmpty()) {
        Q_EMIT resultsReady({});
        return;
    }
    const QString keyword = m_query.section(u' ', 0, 0);
    if (text.contains(u' ') && std::any_of(m_extensions.cbegin(), m_extensions.cend(),
                                           [&](const Extension &e) { return e.keyword.compare(keyword, Qt::CaseInsensitive) == 0; }))
        m_delay.start();
    publish();
}

void ScriptSource::publish()
{
    QList<Item> items = commandItems(m_query) + extensionItems(m_query);
    if (m_listedFor == m_query)
        items << m_listed;
    Q_EMIT resultsReady(items);
}

QList<Item> ScriptSource::fallbackItems(const QString &text) const
{
    QList<Item> items;
    Launcher *l = launcher();
    auto self = const_cast<ScriptSource *>(this);
    for (const Library::Command &c : m_library->commands()) {
        const QList<Argument> arguments = c.arguments();
        if (arguments.isEmpty())
            continue;
        Item item;
        item.id = QStringLiteral("cmd:") + c.name;
        item.title = tr("%1: “%2”").arg(c.name, text);
        item.icon = QIcon::fromTheme(QStringLiteral("utilities-terminal"));
        const QString name = c.name, command = c.command;
        const int output = int(c.output);
        if (arguments.size() == 1) {
            const QString line = Arguments::fill(command, {{arguments.first().name, text}}, shellQuote);
            item.actions << Action{tr("Run"), {}, {}, [self, name, line, output] { self->runCommand(name, line, output); },
                                   c.output == Library::Command::List};
        } else {
            Item shown = item;
            shown.title = c.name;
            Action ask{tr("Enter Arguments"), {}, {}, [l, self, shown, arguments, text, name, command, output] {
                l->askArguments(shown, arguments, {text}, [l, self, arguments, name, command, output](const QStringList &values) {
                    self->runCommand(name, Arguments::fill(command, Arguments::byName(arguments, values), shellQuote), output);
                    l->hideLauncher();
                });
            }, true};
            ask.asksArguments = true;
            item.actions << ask;
        }
        items << item;
    }
    for (const Extension &e : m_extensions) {
        if (e.runs || !e.arguments.isEmpty())
            continue;
        Item item;
        item.id = QStringLiteral("ext:") + e.name;
        item.title = tr("Search %1 for “%2”").arg(e.name, text);
        item.icon = iconFrom(e.icon, QStringLiteral("plugins"), QFileInfo(e.path).path());
        item.actions << Action{tr("Search"), {}, {}, [l, e, text] {
            l->pushView(new ExtensionView(l, e, {QStringLiteral("search")}, {}, e.name), text);
        }, true};
        items << item;
    }
    return items;
}

// ------------------------------------------------------- extension view

ExtensionView::ExtensionView(Launcher *launcher, const ScriptSource::Extension &extension, const QStringList &command, const QStringList &flags,
                             const QString &title)
    : Source(launcher)
    , m_extension(extension)
    , m_command(command)
    , m_flags(flags)
    , m_title(title)
{
    m_delay.setSingleShot(true);
    m_delay.setInterval(120);
    connect(&m_delay, &QTimer::timeout, this, &ExtensionView::search);
}

ExtensionView::~ExtensionView()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
    }
}

QString ExtensionView::viewPlaceholder() const
{
    return m_extension.placeholder.isEmpty() ? tr("Search %1…").arg(m_title) : m_extension.placeholder;
}

void ExtensionView::query(const QString &text)
{
    m_query = text.trimmed();
    m_delay.start(m_proc ? 120 : 0); // the first list at once
}

void ExtensionView::search()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
    }
    m_proc = new QProcess(this);
    const QString forQuery = m_query;
    connect(m_proc, &QProcess::finished, this, [this, forQuery] {
        const QJsonArray results = QJsonDocument::fromJson(m_proc->readAllStandardOutput()).array();
        m_proc->deleteLater();
        m_proc = nullptr;
        if (forQuery == m_query)
            Q_EMIT resultsReady(ScriptSource::resultsFrom(results, launcher(), m_extension));
    });
    QTimer::singleShot(ExtensionTimeout, m_proc, [proc = m_proc] { proc->kill(); });
    m_proc->start(m_extension.path, m_command + QStringList{m_query} + m_flags);
}
