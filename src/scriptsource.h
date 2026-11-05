#pragma once

#include "arguments.h"
#include "source.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QProcess>
#include <QTimer>

class Library;

// The user's own programs as results:
//
//  - Custom commands (Settings → Commands): a shell line run with sh -c.
//    {query} or {argument name="…"} are replaced by (quoted) values typed
//    in fields in the search row; a single one can also be typed after the
//    command's keyword. The output can be ignored, shown, copied, pasted, or
//    listed as results (one per line).
//
//  - Extensions: executables in ~/.config/lumen/extensions/. See README.md
//    ("Writing an extension") for the protocol: `EXT info` describes it,
//    `EXT search TEXT` lists results (shown in a view of their own, with a
//    Markdown detail pane, several actions per result and nested lists),
//    `EXT run` runs a command-style extension.
class ScriptSource : public Source
{
    Q_OBJECT
public:
    struct Extension {
        QString path;
        QString name;
        QString keyword;
        QString description;
        QString icon;
        QString placeholder;
        bool runs = false;    // mode "run": a command, not a list
        bool preview = false; // its list shows the detail pane
        QList<Argument> arguments;
    };

    ScriptSource(Launcher *launcher, Library *library);

    void query(const QString &text) override;
    QList<Item> fallbackItems(const QString &text) const override;

    static QString extensionsFolder();
    // An icon name, a file path (relative to baseDir) or an emoji.
    static QIcon iconFrom(const QString &spec, const QString &fallback, const QString &baseDir = {});
    // An extension's (or Raycast script's) argument description.
    static Argument argumentFrom(const QJsonObject &object, int index);
    // The results an extension printed. `push` opens a nested list.
    static QList<Item> resultsFrom(const QJsonArray &results, Launcher *launcher, const Extension &extension);
    // "--arg name=value" flags passing argument values to an extension.
    static QStringList argumentFlags(const QList<Argument> &arguments, const QStringList &values);

private:
    void scanExtensions();
    void runCommand(const QString &name, const QString &shellLine, int output);
    void searchExtension();
    void openExtension(const Extension &extension, const QString &text);
    void runExtension(const Extension &extension, const QStringList &flags);
    QList<Item> commandItems(const QString &text) const;
    QList<Item> extensionItems(const QString &text) const;
    void publish();

    Library *m_library;
    QFileSystemWatcher m_watcher;
    QList<Extension> m_extensions;
    QString m_query;
    QList<Item> m_listed;    // output lines of a "list" command, or extension results
    QString m_listedFor;     // the query those belong to
    QTimer m_delay;          // debounce for extension searches
    QProcess *m_proc = nullptr;
};

// An extension's list as a view of its own: `EXT search TEXT` (or, for a
// list pushed from a result, `EXT open ID TEXT`) for every query.
class ExtensionView : public Source
{
    Q_OBJECT
public:
    // command: the arguments before the search text ({"search"}, or
    // {"open", ID}); flags: those after it (argument values).
    ExtensionView(Launcher *launcher, const ScriptSource::Extension &extension, const QStringList &command, const QStringList &flags,
                  const QString &title);
    ~ExtensionView() override;

    void query(const QString &text) override;
    QString viewTitle() const override { return m_title; }
    QString viewPlaceholder() const override;
    bool hasPreviewPane() const override { return m_extension.preview; }

private:
    void search();

    ScriptSource::Extension m_extension;
    QStringList m_command;
    QStringList m_flags;
    QString m_title;
    QString m_query;
    QTimer m_delay;
    QProcess *m_proc = nullptr;
};
