#pragma once

#include "arguments.h"

#include <QDateTime>
#include <QList>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

// The user's snippets, quick links, commands, favourites and aliases, in
// library.json inside the library folder: ~/.config/lumen, or a folder the
// user syncs between computers (Settings → General). Changes made there by
// another computer are picked up while Lumen runs.
class Library : public QObject
{
    Q_OBJECT
public:
    struct Snippet {
        QString name;
        QString keyword; // optional short trigger, e.g. "addr"
        QString text;    // may contain placeholders, see expandSnippet()
        QList<Argument> arguments() const { return Arguments::fromTemplate(text); }
    };
    // A snippet with its placeholders filled in.
    struct Expanded {
        QString text;
        int cursorBack = 0; // characters after {cursor}: how far to move back after pasting
    };
    struct QuickLink {
        QString name;
        QString keyword; // typed before an argument: "gh qt layer shell"
        QString target;  // URL or path; {query} or {argument name="…"} are replaced by what is typed
        QString app;     // desktop entry to open it with; empty: the default browser or app
        QList<Argument> arguments() const { return Arguments::fromTemplate(target); }
        bool takesQuery() const { return !arguments().isEmpty(); }
    };

    struct Command {
        enum Output { Silent, Notify, Copy, Paste, List };
        QString name;
        QString keyword; // typed before an argument when the command uses {query}
        QString command; // run with sh -c; {query} and {argument name="…"} become the quoted arguments
        Output output = Notify;
        QList<Argument> arguments() const { return Arguments::fromTemplate(command); }
        bool takesQuery() const { return !arguments().isEmpty(); }
    };
    // A result kept on the start screen. Results from KRunner cannot be
    // stored, so Lumen keeps what it showed and finds the result again by id
    // when the favourite is used.
    struct Favourite {
        QString id;
        QString title;
        QString subtitle;
        QString kind;
        QString iconFile; // the result's icon, saved as an image
    };
    // A saved arrangement of windows ("Coding": the editor left, a terminal
    // right): each window's app, title and place (see WindowLayoutSource).
    struct WindowArrangement {
        QString name;
        QJsonArray windows;
    };
    // A short word that brings up a result ("ff" → Firefox).
    struct Alias {
        QString alias;
        QString id;
        QString title; // searched for to find the result again
    };

    static QString outputName(Command::Output output);
    static Command::Output outputFromName(const QString &name);

    explicit Library(QObject *parent = nullptr);

    QString folder() const { return m_folder; }
    // Moves the library (and notes and favourite icons) to folder; an empty
    // folder means the default. A library already there is used as it is.
    void setFolder(const QString &folder);
    static QString defaultFolder();
    // The folder chosen in the settings, or the default.
    static QString configuredFolder();

    QList<Snippet> snippets() const { return m_snippets; }
    QList<QuickLink> quickLinks() const { return m_links; }
    void setSnippets(const QList<Snippet> &snippets);
    void setQuickLinks(const QList<QuickLink> &links);
    QList<Command> commands() const { return m_commands; }
    void setCommands(const QList<Command> &commands);
    QList<Favourite> favourites() const { return m_favourites; }
    void setFavourites(const QList<Favourite> &favourites);
    QList<Alias> aliases() const { return m_aliases; }
    void setAliases(const QList<Alias> &aliases);
    // Results offered for any search, below the others ("Use “…” with"),
    // by id: "lumen:files-for", "link:Google", "cmd:Name", "script:/path"…
    QStringList fallbacks() const { return m_fallbacks; }
    QList<WindowArrangement> arrangements() const { return m_arrangements; }
    void setArrangements(const QList<WindowArrangement> &arrangements);
    void setFallbacks(const QStringList &ids);

    // Fills in a snippet's placeholders:
    //  {date} {time} {datetime} {day}: now, or moved with an offset —
    //      {date +3d}, {time offset="-2h"}, {date +1M format="yyyy-MM-dd"}
    //      (s, m, h, d, w, M for months, y) — in the locale's short format
    //      or a Qt date format;
    //  {clipboard}: what is copied, {clipboard offset=1} the item before;
    //  {uuid}: a new random identifier;
    //  {argument name="…"}: from `values` (asked for before using it);
    //  {cursor}: where the cursor goes once the text is pasted.
    // `clipboard(n)` gives the nth most recent copied text.
    static Expanded expandSnippet(const QString &text, const QHash<QString, QString> &values = {},
                                  const std::function<QString(int)> &clipboard = {}, const QDateTime &now = QDateTime::currentDateTime());
    // The text alone ({cursor} dropped).
    static QString expand(const QString &text) { return expandSnippet(text).text; }

    // Common links (YouTube, Wikipedia, Maps, the Arch Wiki…): offered by
    // "Add Ready-Made Links"; addReadyMadeLinks() adds those not yet there
    // (by name or keyword) and says how many.
    static QList<QuickLink> readyMadeLinks();
    int addReadyMadeLinks();
    // Opens a resolved link with its app, or the default one.
    static void open(const QuickLink &link, const QUrl &url);

    // The URL a quick link opens for its arguments (by name), or for one
    // text given to its first argument.
    static QUrl resolve(const QuickLink &link, const QHash<QString, QString> &values);
    static QUrl resolve(const QuickLink &link, const QString &query);

Q_SIGNALS:
    void changed();

private:
    void load();
    void save();
    void reloadIfChanged();

    QString m_folder;
    QString m_file;
    QFileSystemWatcher m_watcher;
    QByteArray m_saved; // what we last wrote, to tell our own changes apart
    QList<Snippet> m_snippets;
    QList<QuickLink> m_links;
    QList<Command> m_commands;
    QList<Favourite> m_favourites;
    QList<Alias> m_aliases;
    QStringList m_fallbacks;
    QList<WindowArrangement> m_arrangements;
};
