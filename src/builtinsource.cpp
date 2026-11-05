#include "builtinsource.h"
#include "plural.h"
#include "fuzzy.h"
#include "launcher.h"
#include "library.h"
#include "noteswindow.h"
#include "pastestack.h"
#include "browserhistory.h"
#include "favicons.h"
#include "windowtiling.h"

#include <KIO/OpenUrlJob>

#include <QLocale>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace
{

// Best score of the query against any of the texts.
qreal bestScore(const QString &query, const QStringList &texts)
{
    qreal best = 0;
    for (const QString &t : texts)
        best = std::max(best, fuzzyScore(query, t));
    return best;
}

// A quick link's icon: the site's own once fetched, a folder for paths.
QIcon linkIcon(const Library::QuickLink &link)
{
    if (link.target.startsWith(u'~') || link.target.startsWith(u'/'))
        return QIcon::fromTheme(QStringLiteral("folder"));
    return Favicons::instance()->iconFor(link.target, QIcon::fromTheme(QStringLiteral("internet-web-browser")));
}

} // namespace

BuiltinSource::BuiltinSource(Launcher *launcher, Library *library)
    : Source(launcher)
    , m_library(library)
{
    // A site's icon arrived: show it.
    connect(Favicons::instance(), &Favicons::changed, this, [this] {
        if (!m_lastQuery.isEmpty())
            query(m_lastQuery);
    });
}

void BuiltinSource::createQuickLink()
{
    // Filled in from a copied address, or else the page visited last
    // (most likely the one open in the browser).
    Launcher *l = launcher();
    QString url = l->recentClipboardText(0).trimmed();
    QString title;
    if (!url.startsWith(QLatin1String("http://")) && !url.startsWith(QLatin1String("https://"))) {
        url.clear();
        if (!BrowserHistory::visits().isEmpty()) {
            url = BrowserHistory::visits().first().url;
            title = BrowserHistory::visits().first().title;
        }
    }
    if (title.isEmpty() && !url.isEmpty())
        title = QUrl(url).host().remove(QRegularExpression(QStringLiteral("^www\\.")));
    Argument name, keyword, target;
    name.name = QStringLiteral("name");
    name.placeholder = tr("Name");
    keyword.name = QStringLiteral("keyword");
    keyword.placeholder = tr("Keyword");
    keyword.optional = true;
    target.name = QStringLiteral("url");
    target.placeholder = tr("URL ({query} for a search)");
    Item shown;
    shown.title = tr("Create Quick Link");
    shown.subtitle = tr("Put {query} where the search words go: https://example.com/search?q={query}");
    shown.icon = QIcon::fromTheme(QStringLiteral("bookmark-new"));
    Library *library = m_library;
    l->askArguments(shown, {name, keyword, target}, {title.left(60), {}, url}, [l, library](const QStringList &values) {
        QList<Library::QuickLink> links = library->quickLinks();
        links.removeIf([&](const Library::QuickLink &x) { return x.name.compare(values.at(0), Qt::CaseInsensitive) == 0; });
        links << Library::QuickLink{values.at(0).trimmed(), values.at(1).trimmed(), values.at(2).trimmed(), {}};
        library->setQuickLinks(links);
        l->notify(values.at(1).trimmed().isEmpty() ? tr("Saved “%1”").arg(values.at(0)) : tr("Type “%1” for %2").arg(values.at(1).trimmed(), values.at(0)));
    });
}

QList<Item> BuiltinSource::commands() const
{
    Launcher *l = launcher();
    auto self = const_cast<BuiltinSource *>(this);
    Library *library = m_library;
    auto command = [](const QString &id, const QString &title, const QString &subtitle, const QString &icon, const Action &primary) {
        Item item;
        item.id = QStringLiteral("lumen:") + id;
        item.title = title;
        item.subtitle = subtitle;
        item.kind = QObject::tr("Command");
        item.icon = QIcon::fromTheme(icon);
        item.actions << primary;
        return item;
    };

    QList<Item> list{
        command(QStringLiteral("clipboard"), tr("Clipboard History"), tr("Search and reuse what you copied"),
                QStringLiteral("klipper"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::ClipboardView); }, true}),
        command(QStringLiteral("files"), tr("Search Files"), tr("Find files and folders by name"), QStringLiteral("system-search"),
                {tr("Open"), {}, {}, [l] { l->openView(Launcher::FilesView); }, true}),
        command(QStringLiteral("snippets"), tr("Search Snippets"), tr("Copy saved text"), QStringLiteral("insert-text"),
                {tr("Open"), {}, {}, [l] { l->openView(Launcher::SnippetsView); }, true}),
        command(QStringLiteral("emoji"), tr("Search Emoji"), tr("Emoji and symbols by name"), QStringLiteral("smiley"),
                {tr("Open"), {}, {}, [l] { l->openView(Launcher::EmojiView); }, true}),
        command(QStringLiteral("windows"), tr("Switch Window"), tr("All open windows, with previews"), QStringLiteral("window"),
                {tr("Open"), {}, {}, [l] { l->openView(Launcher::WindowsView); }, true}),
        command(QStringLiteral("menu"), tr("Search Menu Items"), tr("Run a command from the menu of the app you were using"),
                QStringLiteral("application-menu"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::MenusView); }, true}),
        command(QStringLiteral("tabs"), tr("Search Browser Tabs"), tr("Switch to an open tab in your browser"),
                QStringLiteral("internet-web-browser"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::TabsView); }, true}),
        command(QStringLiteral("history"), tr("Browser History"), tr("Pages you visited, in every browser"),
                QStringLiteral("view-history"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::HistoryView); }, true}),
        command(QStringLiteral("calculations"), tr("Calculator History"), tr("Calculations and conversions you used"),
                QStringLiteral("accessories-calculator"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::CalculationsView); }, true}),
        command(QStringLiteral("apps"), tr("Force Quit Applications"), tr("Running apps: quit them, or force them to stop"),
                QStringLiteral("application-exit"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::AppsView); }, true}),
        command(QStringLiteral("packages"), tr("Install Packages"), tr("Search pacman, the AUR and Flathub; install, remove, update"),
                QStringLiteral("system-software-install"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::PackagesView); }, true}),
        command(QStringLiteral("updates"), tr("Update Packages"), tr("What can be updated, and Upgrade All"),
                QStringLiteral("system-software-update"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::PackagesView); }, true}),
        command(QStringLiteral("theme"), tr("Lumen Theme"), tr("Colours of Lumen: Light, Dark, Midnight, Paper…, and the accent"),
                QStringLiteral("preferences-desktop-color"), {tr("Open"), {}, {}, [l] { l->openView(Launcher::ThemeView); }, true}),
        command(QStringLiteral("paste-plain"), tr("Paste as Plain Text"), tr("Paste what is copied without its formatting"),
                QStringLiteral("edit-paste"), {tr("Paste"), {}, {}, [l] { l->pastePlainText(); }, true}),
        command(QStringLiteral("paste-stack"), tr("Paste Stack"),
                l->pasteStack()->isActive() ? plural(l->pasteStack()->items().size(), tr("%1 item waiting"), tr("%1 items waiting"))
                                            : tr("Copy several things, then paste them in order"),
                QStringLiteral("view-list-text"), {tr("Open"), {}, {}, [l] {
                    if (!l->pasteStack()->isActive())
                        l->pasteStack()->start();
                    l->openView(Launcher::PasteStackView);
                }, true}),
        command(QStringLiteral("notes"), tr("Notes"),
                l->notes()->firstLine().isEmpty() ? tr("A scratchpad that saves as you type") : l->notes()->firstLine(),
                QStringLiteral("note"), {tr("Open"), {}, {}, [l] { l->openNotes(); }}),
        command(QStringLiteral("recent"), tr("Recent Files"), tr("Files you opened lately"), QStringLiteral("document-open-recent"),
                {tr("Open"), {}, {}, [l] { l->openView(Launcher::FilesView); }, true}),
        command(QStringLiteral("new-snippet"), tr("Create Snippet"), tr("Save text you type often"), QStringLiteral("list-add"),
                {tr("Open"), {}, {}, [l] { l->openSettings(Launcher::SnippetsTab, true); }}),
        command(QStringLiteral("new-link"), tr("Create Quick Link"), tr("From the copied address, or the page you are on"),
                QStringLiteral("bookmark-new"), {tr("Create"), {}, {}, [self] { self->createQuickLink(); }, true}),
        command(QStringLiteral("ready-links"), tr("Add Ready-Made Quick Links"), tr("YouTube, Maps, Wikipedia, Reddit, Stack Overflow, the Arch Wiki…"),
                QStringLiteral("bookmarks"), {tr("Add"), {}, {}, [l, library] {
                    const int added = library->addReadyMadeLinks();
                    l->notify(added ? tr("Added %1 quick links").arg(added) : tr("You have them all already"));
                }, true}),
        command(QStringLiteral("new-command"), tr("Create Command"), tr("Run a shell command or script from Lumen"),
                QStringLiteral("utilities-terminal"), {tr("Open"), {}, {}, [l] { l->openSettings(Launcher::CommandsTab, true); }}),
        command(QStringLiteral("settings"), tr("Lumen Settings"), tr("Shortcut, snippets, quick links, clipboard"),
                QStringLiteral("configure"), {tr("Open"), {}, {}, [l] { l->openSettings(Launcher::GeneralTab, false); }}),
    };

    for (const WindowTiling::Layout &layout : WindowTiling::layouts()) {
        const QString id = layout.id;
        Item item = command(QStringLiteral("window-") + id, layout.name, tr("Window Management"), layout.iconName,
                            {tr("Apply to Active Window"), {}, {}, [l, id] {
                                 l->runAfterHide([l, id] {
                                     if (!WindowTiling::apply(id))
                                         l->notify(tr("Window layouts need KWin"));
                                 });
                             }});
        item.kind = tr("Window");
        list << item;
    }
    return list;
}

QList<Item> BuiltinSource::allCommands() const
{
    return commands();
}

QList<Item> BuiltinSource::snippetItems(const QString &text) const
{
    QList<Item> items;
    Launcher *l = launcher();
    for (const Library::Snippet &s : m_library->snippets()) {
        const qreal score = text.isEmpty() ? 50 : bestScore(text, {s.name, s.keyword});
        if (score <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("snippet:") + s.name;
        item.title = s.name;
        item.subtitle = s.text.section(u'\n', 0, 0).left(80);
        item.kind = s.keyword.isEmpty() ? tr("Snippet") : tr("Snippet · %1").arg(s.keyword);
        item.icon = QIcon::fromTheme(QStringLiteral("insert-text"));
        item.score = score;
        const QString body = s.text, name = s.name;
        // Snippets with arguments ask for them first, so they stay open.
        const bool asks = !s.arguments().isEmpty();
        if (l->canPaste()) {
            Action paste{tr("Paste"), {}, {}, [l, name, body] { l->useSnippet(name, body, Launcher::SnippetUse::Paste); }, asks};
            paste.asksArguments = asks;
            item.actions << paste;
        }
        Action copy{tr("Copy to Clipboard"), QStringLiteral("edit-copy"), l->canPaste() ? QKeySequence(Qt::CTRL | Qt::Key_Return) : QKeySequence(),
                    [l, name, body] { l->useSnippet(name, body, Launcher::SnippetUse::Copy); }, asks};
        copy.asksArguments = asks && !l->canPaste();
        item.actions << copy;
        item.actions << Action{tr("Edit Snippets"), QStringLiteral("document-edit"), QKeySequence(Qt::CTRL | Qt::Key_E),
                               [l] { l->openSettings(Launcher::SnippetsTab, false); }};
        items << item;
    }
    return items;
}

QString BuiltinSource::argumentNames(const QList<Argument> &arguments)
{
    QStringList names;
    for (const Argument &a : arguments)
        names << a.label();
    return QLocale().createSeparatedList(names);
}

// Opens the argument fields for a quick link, then the link.
Action BuiltinSource::askAction(const Library::QuickLink &link, const QStringList &initial) const
{
    Launcher *l = launcher();
    Item shown;
    shown.title = link.name;
    shown.subtitle = link.target;
    shown.icon = linkIcon(link);
    Action ask{tr("Enter %1").arg(argumentNames(link.arguments())), {}, {}, [l, link, shown, initial] {
        const QList<Argument> arguments = link.arguments();
        l->askArguments(shown, arguments, initial, [l, link, arguments](const QStringList &values) {
            Library::open(link, Library::resolve(link, Arguments::byName(arguments, values)));
            l->hideLauncher();
        });
    }, true};
    ask.asksArguments = true;
    return ask;
}

QList<Item> BuiltinSource::fallbackItems(const QString &text) const
{
    Launcher *l = launcher();
    Item files;
    files.id = QStringLiteral("lumen:files-for");
    files.title = tr("Search Files for “%1”").arg(text);
    files.kind = tr("Command");
    files.icon = QIcon::fromTheme(QStringLiteral("system-search"));
    files.actions << Action{tr("Search"), {}, {}, [l, text] { l->openView(Launcher::FilesView, text); }, true};
    QList<Item> items{files};
    for (const Library::QuickLink &link : m_library->quickLinks()) {
        if (!link.takesQuery())
            continue;
        Item item;
        item.id = QStringLiteral("link:") + link.name;
        item.title = tr("Search %1 for “%2”").arg(link.name, text);
        item.kind = tr("Quick Link");
        item.icon = linkIcon(link);
        if (link.arguments().size() > 1)
            item.actions << askAction(link, {text});
        else
            item.actions << Action{tr("Open"), {}, {}, [link, text] { Library::open(link, Library::resolve(link, text)); }};
        items << item;
    }
    return items;
}

QList<Item> BuiltinSource::quickLinkItems(const QString &text) const
{
    QList<Item> items;
    Launcher *l = launcher();
    const QString keyword = text.section(u' ', 0, 0);
    const QString argument = text.section(u' ', 1).trimmed();
    for (const Library::QuickLink &link : m_library->quickLinks()) {
        Item item;
        item.icon = linkIcon(link);
        item.kind = link.keyword.isEmpty() ? tr("Quick Link") : tr("Quick Link · %1").arg(link.keyword);
        item.id = QStringLiteral("link:") + link.name;

        if (link.takesQuery() && !link.keyword.isEmpty() && keyword.compare(link.keyword, Qt::CaseInsensitive) == 0) {
            // "gh layer shell" → search GitHub for "layer shell"
            item.title = argument.isEmpty() ? link.name : tr("%1: %2").arg(link.name, argument);
            item.subtitle = argument.isEmpty() ? tr("Type what to search for") : Library::resolve(link, argument).toDisplayString();
            item.score = 120; // the user typed this link's keyword: it wins
            if (link.arguments().size() > 1) {
                // More to fill in: the typed text goes in the first field.
                item.subtitle = tr("Enter %1").arg(argumentNames(link.arguments()));
                item.actions << askAction(link, {argument});
            } else if (argument.isEmpty()) {
                item.actions << Action{tr("Open"), {}, {}, [] { }, true};
            } else {
                item.actions << Action{tr("Open"), {}, {}, [link, argument] { Library::open(link, Library::resolve(link, argument)); }};
            }
            items << item;
            continue;
        }
        const qreal score = text.isEmpty() ? 50 : bestScore(text, {link.name, link.keyword});
        if (score <= 0)
            continue;
        item.title = link.name;
        item.subtitle = link.target;
        item.score = score;
        if (link.arguments().size() > 1 || (link.takesQuery() && link.keyword.isEmpty())) {
            item.actions << askAction(link, {});
        } else if (link.takesQuery()) {
            // Without an argument, fill in the keyword so the user can type one.
            const QString prefix = link.keyword + u' ';
            item.actions << Action{tr("Search %1").arg(link.name), {}, {}, [l, prefix] { l->setQuery(prefix); }, true};
        } else {
            item.actions << Action{tr("Open"), {}, {}, [link] { Library::open(link, Library::resolve(link, QString())); }};
        }
        item.actions << Action{tr("Edit Quick Links"), QStringLiteral("document-edit"), QKeySequence(Qt::CTRL | Qt::Key_E),
                               [l] { l->openSettings(Launcher::QuickLinksTab, false); }};
        items << item;
    }
    return items;
}

void BuiltinSource::query(const QString &text)
{
    m_lastQuery = text;
    const QString q = text.trimmed();
    QList<Item> items;
    if (!q.isEmpty()) {
        for (Item item : commands()) {
            item.score = bestScore(q, {item.title, item.subtitle.isEmpty() ? QString() : item.title + u' ' + item.subtitle});
            if (item.kind == tr("Window")) {
                // Window layouts also match "tile", "snap", ...
                for (const WindowTiling::Layout &layout : WindowTiling::layouts())
                    if (QStringLiteral("lumen:window-") + layout.id == item.id)
                        item.score = std::max(item.score, bestScore(q, layout.keywords) * 0.8);
            }
            if (item.score > 0)
                items << item;
        }
        items << snippetItems(q) << quickLinkItems(text);
    }
    Q_EMIT resultsReady(items);
}
