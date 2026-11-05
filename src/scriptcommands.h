#pragma once

#include "arguments.h"
#include "source.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QTimer>

#include <optional>

// A Raycast script command: a script whose comments describe it,
//   # @raycast.schemaVersion 1
//   # @raycast.title Say Hello
//   # @raycast.mode compact
//   # @raycast.argument1 { "type": "text", "placeholder": "Name" }
// so the scripts written for Raycast (github.com/raycast/script-commands)
// run in Lumen as they are.
struct ScriptCommand {
    enum Mode { Silent, Compact, FullOutput, Inline };

    QString path;
    QString title;
    Mode mode = Compact;
    QString packageName;
    QString icon;
    QString iconDark;
    QString description;
    QString directory; // where it runs
    bool needsConfirmation = false;
    int refreshSeconds = 0; // inline: how often its output is refreshed
    QList<Argument> arguments;
    QList<bool> percentEncoded; // per argument: URL-encode the value

    // Reads the metadata; nothing when it is not a script command.
    static std::optional<ScriptCommand> parse(const QString &path, const QString &content);
    // "1h", "10m", "30s", "1d" in seconds (0 when not understood).
    static int parseDuration(const QString &text);
    // The program and arguments that run the script with these values.
    QStringList commandLine(const QStringList &values) const;
};

// Script commands as results. They are found in the folders listed in
// lumenrc [ScriptCommands] Folders (~/.config/lumen/script-commands by
// default; Settings → Commands), which are watched for changes.
class ScriptCommandSource : public Source
{
    Q_OBJECT
public:
    explicit ScriptCommandSource(Launcher *launcher);

    void query(const QString &text) override;
    QList<Item> fallbackItems(const QString &text) const override;
    void sessionStarted() override;

    static QString defaultFolder();
    static QStringList folders();
    static void setFolders(const QStringList &folders);

private:
    void scan();
    Item itemFor(const ScriptCommand &command) const;
    // Asks for arguments and confirmation as the script wants, then runs it.
    void start(const ScriptCommand &command, const QStringList &initial);
    void run(const ScriptCommand &command, const QStringList &values);
    void refreshInline(const ScriptCommand &command);

    QList<ScriptCommand> m_commands;
    QFileSystemWatcher m_watcher;
    QTimer m_rescan;
    QString m_query;
    QStringList m_scanned; // the folders the commands came from
    struct InlineOutput {
        QString line;
        QDateTime when;
        bool running = false;
    };
    QHash<QString, InlineOutput> m_inline; // by path
};
