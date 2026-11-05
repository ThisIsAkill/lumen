#include "scriptcommands.h"
#include "fuzzy.h"
#include "launcher.h"
#include "outputview.h"
#include "scriptsource.h"

#include <chrome.h>

#include <KConfigGroup>
#include <KIO/OpenUrlJob>
#include <KSharedConfig>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace
{

constexpr int RunTimeout = 5 * 60 * 1000;   // ms before a silent or compact script is stopped
constexpr int InlineTimeout = 30 * 1000;    // ms for an inline script to print its line
constexpr int MinRefresh = 10;              // seconds, as in Raycast
constexpr qint64 MaxScriptSize = 512 * 1024; // bytes read when looking for the metadata

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("ScriptCommands"));
}

QString firstLine(const QString &output)
{
    for (const QString &line : output.split(u'\n'))
        if (!line.trimmed().isEmpty())
            return line.trimmed();
    return {};
}

QString lastLine(const QString &output)
{
    const QStringList lines = output.split(u'\n', Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it)
        if (!it->trimmed().isEmpty())
            return it->trimmed();
    return {};
}

} // namespace

// ------------------------------------------------------------- parsing

int ScriptCommand::parseDuration(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*(\d+)\s*([smhd])\s*$)"));
    const auto m = pattern.match(text);
    if (!m.hasMatch())
        return 0;
    const int n = m.captured(1).toInt();
    switch (m.captured(2).at(0).unicode()) {
    case 's':
        return n;
    case 'm':
        return n * 60;
    case 'h':
        return n * 3600;
    default:
        return n * 86400;
    }
}

std::optional<ScriptCommand> ScriptCommand::parse(const QString &path, const QString &content)
{
    // "# @raycast.title Say Hello", after any comment marker (#, //, --, ;, ').
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*(?:#|//|--|;|'|%)\s*@raycast\.([A-Za-z0-9]+)[ \t]*(.*?)\s*$)"),
                                            QRegularExpression::MultilineOption);
    ScriptCommand c;
    c.path = path;
    bool versioned = false;
    QHash<int, QJsonObject> arguments;
    for (auto it = pattern.globalMatch(content); it.hasNext();) {
        const auto m = it.next();
        const QString key = m.captured(1);
        const QString value = m.captured(2);
        if (key == QLatin1String("schemaVersion")) {
            versioned = true;
        } else if (key == QLatin1String("title")) {
            c.title = value;
        } else if (key == QLatin1String("mode")) {
            static const QHash<QString, Mode> modes{{QStringLiteral("silent"), Silent},
                                                    {QStringLiteral("compact"), Compact},
                                                    {QStringLiteral("fullOutput"), FullOutput},
                                                    {QStringLiteral("inline"), Inline}};
            c.mode = modes.value(value, Compact);
        } else if (key == QLatin1String("packageName")) {
            c.packageName = value;
        } else if (key == QLatin1String("icon")) {
            c.icon = value;
        } else if (key == QLatin1String("iconDark")) {
            c.iconDark = value;
        } else if (key == QLatin1String("description")) {
            c.description = value;
        } else if (key == QLatin1String("currentDirectoryPath")) {
            c.directory = value.startsWith(u'~') ? QDir::homePath() + value.mid(1) : value;
        } else if (key == QLatin1String("needsConfirmation")) {
            c.needsConfirmation = value == QLatin1String("true");
        } else if (key == QLatin1String("refreshTime")) {
            if (const int seconds = parseDuration(value))
                c.refreshSeconds = std::max(seconds, MinRefresh);
        } else if (key.startsWith(QLatin1String("argument"))) {
            const int index = key.mid(8).toInt();
            const QJsonObject o = QJsonDocument::fromJson(value.toUtf8()).object();
            if (index >= 1 && index <= 3 && !o.isEmpty())
                arguments.insert(index, o);
        }
    }
    if (!versioned || c.title.isEmpty())
        return std::nullopt;
    for (int index = 1; index <= 3; ++index) {
        if (!arguments.contains(index))
            continue;
        const QJsonObject o = arguments.value(index);
        Argument a = ScriptSource::argumentFrom(o, index - 1);
        a.name = QStringLiteral("argument%1").arg(index);
        c.arguments << a;
        c.percentEncoded << o.value(QStringLiteral("percentEncoded")).toBool();
    }
    if (c.directory.isEmpty())
        c.directory = QFileInfo(path).absolutePath();
    return c;
}

QStringList ScriptCommand::commandLine(const QStringList &values) const
{
    QStringList args;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString value = values.value(i);
        args << (percentEncoded.value(i) ? QString::fromUtf8(QUrl::toPercentEncoding(value)) : value);
    }
    if (QFileInfo(path).isExecutable())
        return QStringList{path} + args;
    // Not executable: run it with the interpreter its first line names.
    QFile f(path);
    QString shebang;
    if (f.open(QIODevice::ReadOnly))
        shebang = QString::fromUtf8(f.readLine()).trimmed();
    QStringList interpreter;
    if (shebang.startsWith(QLatin1String("#!")))
        interpreter = shebang.mid(2).split(u' ', Qt::SkipEmptyParts);
    if (interpreter.isEmpty()) {
        static const QHash<QString, QString> bySuffix{{QStringLiteral("py"), QStringLiteral("python3")},
                                                     {QStringLiteral("rb"), QStringLiteral("ruby")},
                                                     {QStringLiteral("js"), QStringLiteral("node")},
                                                     {QStringLiteral("php"), QStringLiteral("php")},
                                                     {QStringLiteral("pl"), QStringLiteral("perl")}};
        interpreter << bySuffix.value(QFileInfo(path).suffix(), QStringLiteral("sh"));
    }
    return interpreter + QStringList{path} + args;
}

// -------------------------------------------------------------- source

ScriptCommandSource::ScriptCommandSource(Launcher *launcher)
    : Source(launcher)
{
    QDir().mkpath(defaultFolder());
    // Editors save in bursts; look again once they are done.
    m_rescan.setSingleShot(true);
    m_rescan.setInterval(300);
    connect(&m_rescan, &QTimer::timeout, this, &ScriptCommandSource::scan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_rescan, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_rescan, qOverload<>(&QTimer::start));
    scan();
}

QString ScriptCommandSource::defaultFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/lumen/script-commands");
}

QStringList ScriptCommandSource::folders()
{
    return config().readEntry("Folders", QStringList{defaultFolder()});
}

void ScriptCommandSource::setFolders(const QStringList &folders)
{
    KConfigGroup g = config();
    g.writeEntry("Folders", folders);
    g.sync();
}

void ScriptCommandSource::scan()
{
    m_commands.clear();
    m_scanned = folders();
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    for (const QString &folder : std::as_const(m_scanned)) {
        const QDir dir(folder.startsWith(u'~') ? QDir::homePath() + folder.mid(1) : folder);
        if (!dir.exists())
            continue;
        m_watcher.addPath(dir.absolutePath());
        for (const QFileInfo &file : dir.entryInfoList(QDir::Files, QDir::Name)) {
            // Raycast's convention: templates are not commands until copied.
            if (file.fileName().contains(QLatin1String(".template.")) || file.size() > MaxScriptSize)
                continue;
            QFile f(file.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
                continue;
            if (auto command = ScriptCommand::parse(file.absoluteFilePath(), QString::fromUtf8(f.readAll()))) {
                m_commands << *command;
                m_watcher.addPath(file.absoluteFilePath());
            }
        }
    }
    if (!m_query.isEmpty())
        query(m_query);
}

void ScriptCommandSource::sessionStarted()
{
    if (folders() != m_scanned)
        scan(); // changed in the settings
    for (const ScriptCommand &c : std::as_const(m_commands)) {
        if (c.mode != ScriptCommand::Inline || c.refreshSeconds == 0)
            continue;
        if (std::any_of(c.arguments.cbegin(), c.arguments.cend(), [](const Argument &a) { return !a.optional; }))
            continue; // it cannot run by itself
        const InlineOutput out = m_inline.value(c.path);
        if (!out.running && (!out.when.isValid() || out.when.secsTo(QDateTime::currentDateTime()) >= c.refreshSeconds))
            refreshInline(c);
    }
}

Item ScriptCommandSource::itemFor(const ScriptCommand &c) const
{
    Item item;
    item.id = QStringLiteral("script:") + c.path;
    item.title = c.title;
    if (c.mode == ScriptCommand::Inline && m_inline.contains(c.path))
        item.subtitle = m_inline.value(c.path).line;
    else
        item.subtitle = c.description;
    item.kind = c.packageName.isEmpty() ? tr("Script Command") : c.packageName;
    const bool dark = Chrome::isDark(QApplication::palette());
    item.icon = ScriptSource::iconFrom(dark && !c.iconDark.isEmpty() ? c.iconDark : c.icon, QStringLiteral("application-x-shellscript"),
                                       QFileInfo(c.path).absolutePath());
    auto self = const_cast<ScriptCommandSource *>(this);
    Action run{c.arguments.isEmpty() ? tr("Run") : tr("Enter Arguments"), {}, {}, [self, c] { self->start(c, {}); }, true};
    run.asksArguments = !c.arguments.isEmpty();
    item.actions << run;
    const QUrl file = QUrl::fromLocalFile(c.path);
    item.actions << Action{tr("Edit Script"), QStringLiteral("document-edit"), QKeySequence(Qt::CTRL | Qt::Key_E), [file] {
        auto *job = new KIO::OpenUrlJob(file, QStringLiteral("text/plain"));
        job->setRunExecutables(false);
        job->start();
    }};
    return item;
}

void ScriptCommandSource::query(const QString &text)
{
    m_query = text.trimmed();
    QList<Item> items;
    if (!m_query.isEmpty()) {
        for (const ScriptCommand &c : std::as_const(m_commands)) {
            qreal score = fuzzyScore(m_query, c.title);
            if (!c.packageName.isEmpty())
                score = std::max(score, fuzzyScore(m_query, c.packageName + u' ' + c.title) * 0.9);
            if (score <= 0)
                continue;
            Item item = itemFor(c);
            item.score = score;
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}

QList<Item> ScriptCommandSource::fallbackItems(const QString &text) const
{
    QList<Item> items;
    auto self = const_cast<ScriptCommandSource *>(this);
    for (const ScriptCommand &c : m_commands) {
        if (c.arguments.isEmpty() || !c.arguments.first().options.isEmpty())
            continue;
        Item item = itemFor(c);
        item.title = tr("%1: “%2”").arg(c.title, text);
        item.subtitle.clear();
        item.actions.first() = Action{tr("Run"), {}, {}, [self, c, text] { self->start(c, {text}); }, true};
        item.actions.first().asksArguments = c.arguments.size() > 1;
        items << item;
    }
    return items;
}

void ScriptCommandSource::start(const ScriptCommand &command, const QStringList &initial)
{
    Launcher *l = launcher();
    const Item shown = itemFor(command);
    auto confirmThenRun = [this, l, command, shown](const QStringList &values) {
        if (!command.needsConfirmation) {
            run(command, values);
            return;
        }
        l->ask(tr("Confirm"), tr("Press Enter to run “%1”, Esc to cancel").arg(command.title), shown, false,
               [this, command, values](const QString &) { run(command, values); });
    };
    // A value for every argument already: no need to ask (a fallback with
    // the search text for a one-argument script).
    if (initial.size() >= command.arguments.size())
        confirmThenRun(initial);
    else
        l->askArguments(shown, command.arguments, initial, confirmThenRun);
}

void ScriptCommandSource::run(const ScriptCommand &command, const QStringList &values)
{
    Launcher *l = launcher();
    const QStringList argv = command.commandLine(values);
    auto *proc = new QProcess;
    proc->setWorkingDirectory(command.directory);
    proc->setProgram(argv.first());
    proc->setArguments(argv.mid(1));

    if (command.mode == ScriptCommand::FullOutput) {
        // Its output in a view of its own, as it arrives.
        auto *view = new OutputView(l, command.title, QStringLiteral("utilities-terminal"));
        proc->setParent(view);
        view->follow(proc);
        l->pushView(view);
        proc->start();
        return;
    }

    proc->setParent(this);
    const QString title = command.title;
    const bool inlineMode = command.mode == ScriptCommand::Inline;
    connect(proc, &QProcess::finished, this, [this, l, proc, title, inlineMode, path = command.path](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        const QString out = OutputView::plain(QString::fromUtf8(proc->readAllStandardOutput()));
        if (status != QProcess::NormalExit || code != 0) {
            const QString err = lastLine(OutputView::plain(QString::fromUtf8(proc->readAllStandardError())));
            l->notify(!err.isEmpty() ? err : !lastLine(out).isEmpty() ? lastLine(out) : tr("%1 failed").arg(title));
            return;
        }
        if (inlineMode) {
            m_inline[path] = {firstLine(out), QDateTime::currentDateTime(), false};
            query(m_query);
        }
        // Raycast shows the last line it printed.
        const QString message = lastLine(out);
        if (!message.isEmpty())
            l->notify(message);
        else if (!inlineMode)
            l->notify(tr("%1: done").arg(title));
    });
    QTimer::singleShot(RunTimeout, proc, [proc] { proc->kill(); });
    if (command.mode == ScriptCommand::Silent)
        l->hideLauncher();
    else
        l->notify(tr("Running %1…").arg(title));
    proc->start();
}

void ScriptCommandSource::refreshInline(const ScriptCommand &command)
{
    auto *proc = new QProcess(this);
    const QStringList argv = command.commandLine({});
    proc->setWorkingDirectory(command.directory);
    m_inline[command.path].running = true;
    connect(proc, &QProcess::finished, this, [this, proc, path = command.path](int code) {
        proc->deleteLater();
        InlineOutput &out = m_inline[path];
        out.running = false;
        out.when = QDateTime::currentDateTime();
        if (code == 0)
            out.line = firstLine(OutputView::plain(QString::fromUtf8(proc->readAllStandardOutput())));
        if (!m_query.isEmpty())
            query(m_query);
    });
    QTimer::singleShot(InlineTimeout, proc, [proc] { proc->kill(); });
    proc->start(argv.first(), argv.mid(1));
}
