// Drives the launcher with real key events, headless (offscreen platform),
// against a temporary home folder: search, the Space preview, views,
// actions, snippets and quick links.

#include "launcher.h"
#include "library.h"
#include "clipboardhistory.h"
#include "focusmode.h"
#include "theme.h"
#include "menusearch.h"
#include "timekeeper.h"
#include "windowlayouts.h"
#include "noteswindow.h"
#include "pastestack.h"
#include "previewpane.h"
#include "resultview.h"
#include "source.h"

#include <panel.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QClipboard>
#include <QDir>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QTest>

#include <cstdlib>

// An app's menu as it appears on D-Bus (com.canonical.dbusmenu): File with
// Save, Save As and a Recent submenu that is only filled when about to be
// shown, and Edit with a disabled Undo and Find.
class FakeMenu : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.dbusmenu")
public:
    int clicked = 0;
    bool recentShown = false;

    static DBusMenuLayout entry(int id, const QString &label, const QVariantMap &more = {}, const QList<DBusMenuLayout> &children = {})
    {
        QVariantMap properties = more;
        properties.insert(QStringLiteral("label"), label);
        return DBusMenuLayout{id, properties, children};
    }
    static QVariantMap submenu() { return {{QStringLiteral("children-display"), QStringLiteral("submenu")}}; }

public Q_SLOTS:
    uint GetLayout(int parentId, int recursionDepth, const QStringList &propertyNames, DBusMenuLayout &layout)
    {
        Q_UNUSED(recursionDepth);
        Q_UNUSED(propertyNames);
        const DBusMenuLayout recent = entry(5, QStringLiteral("Open _Recent"), submenu(),
                                            recentShown ? QList<DBusMenuLayout>{entry(6, QStringLiteral("notes.md"))} : QList<DBusMenuLayout>{});
        if (parentId == 5) {
            layout = recent;
            return 2;
        }
        // Ctrl+S as dbusmenu describes shortcuts: aas.
        QDBusArgument shortcut;
        shortcut.beginArray(qMetaTypeId<QStringList>());
        shortcut << QStringList{QStringLiteral("Control"), QStringLiteral("s")};
        shortcut.endArray();
        layout = entry(0, {}, submenu(), {
            entry(1, QStringLiteral("_File"), submenu(), {
                entry(2, QStringLiteral("_Save"), {{QStringLiteral("shortcut"), QVariant::fromValue(shortcut)}}),
                entry(3, QStringLiteral("Save &As…")),
                entry(4, {}, {{QStringLiteral("type"), QStringLiteral("separator")}}),
                recent,
            }),
            entry(7, QStringLiteral("_Edit"), submenu(), {
                entry(8, QStringLiteral("Undo"), {{QStringLiteral("enabled"), false}}),
                entry(9, QStringLiteral("_Find…")),
            }),
        });
        return 1;
    }
    bool AboutToShow(int id)
    {
        if (id == 5)
            recentShown = true;
        return true;
    }
    void Event(int id, const QString &eventId, const QDBusVariant &data, uint timestamp)
    {
        Q_UNUSED(data);
        Q_UNUSED(timestamp);
        if (eventId == QLatin1String("clicked"))
            clicked = id;
    }
};

class LauncherTest : public QObject
{
    Q_OBJECT

private:
    Launcher *m_launcher = nullptr;

    QLineEdit *search() const { return m_launcher->findChild<QLineEdit *>(); }
    QListView *list() const { return m_launcher->findChild<QListView *>(); }

    QStringList titles() const
    {
        QStringList out;
        const QAbstractItemModel *model = list()->model();
        for (int row = 0; row < model->rowCount(); ++row)
            out << model->index(row, 0).data().toString();
        return out;
    }
    QString selectedTitle() const { return list()->currentIndex().data().toString(); }

    // A child of the launcher on Wayland, its own window elsewhere.
    Panel *preview() const
    {
        if (auto *embedded = m_launcher->findChild<Panel *>())
            return embedded;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *panel = qobject_cast<Panel *>(w))
                return panel;
        return nullptr;
    }

    void key(Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(search(), k, mods); }

    // A stand-in for lumen-expand-helper's socket (LUMEN_EXPAND_SOCKET).
    static QLocalServer *helperServer()
    {
        static QLocalServer *server = [] {
            auto *s = new QLocalServer;
            s->listen(qEnvironmentVariable("LUMEN_EXPAND_SOCKET"));
            return s;
        }();
        return server;
    }
    QLocalSocket *m_helperClient = nullptr;

    // LUMEN_TEST_PAUSE=ms keeps the screen as it is for a while, for
    // screenshots when the test runs in a real (or virtual) session.
    static void pauseForScreenshot()
    {
        if (const int ms = qEnvironmentVariableIntValue("LUMEN_TEST_PAUSE"); ms > 0)
            QTest::qWait(ms);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(helperServer()->isListening());
        m_launcher = new Launcher;
        QVERIFY(preview());
    }

    void cleanupTestCase() { delete m_launcher; }

    void spacePreviewsFilesAndClosesAgain()
    {
        m_launcher->showView(QStringLiteral("files"), QStringLiteral("report"));
        QTRY_VERIFY_WITH_TIMEOUT(titles().contains(QStringLiteral("tax report.pdf")), 5000);
        QTRY_VERIFY(titles().contains(QStringLiteral("report-2025.txt")));

        // While typing, Space is a space.
        key(Qt::Key_Space);
        QCOMPARE(search()->text(), QStringLiteral("report "));
        QVERIFY(!preview()->isVisible());
        key(Qt::Key_Backspace);
        QTRY_VERIFY(titles().contains(QStringLiteral("tax report.pdf")));

        // After moving to a file, Space opens its preview...
        key(Qt::Key_Down);
        key(Qt::Key_Up);
        const QString first = selectedTitle();
        key(Qt::Key_Space);
        QVERIFY(preview()->isVisible());
        QVERIFY(preview()->currentFile().endsWith(u'/' + first));
        QCOMPARE(search()->text(), QStringLiteral("report"));
        pauseForScreenshot();

        // ...which follows the arrow keys...
        key(Qt::Key_Down);
        const QString second = selectedTitle();
        QVERIFY(second != first);
        QVERIFY(preview()->currentFile().endsWith(u'/' + second));

        // ...and Space closes it again, leaving the launcher open.
        key(Qt::Key_Space);
        QVERIFY(!preview()->isVisible());
        QVERIFY(m_launcher->isVisible());
    }

    void ctrlYAndEscape()
    {
        key(Qt::Key_Y, Qt::ControlModifier);
        QVERIFY(preview()->isVisible());
        key(Qt::Key_Escape); // closes the preview
        QVERIFY(!preview()->isVisible());
        QVERIFY(m_launcher->currentSource());
        key(Qt::Key_Escape); // leaves the Files view
        QVERIFY(!m_launcher->currentSource());
        key(Qt::Key_Escape); // closes the launcher
        QVERIFY(!m_launcher->isVisible());
    }

    void previewPaneInViews()
    {
        auto *pane = m_launcher->findChild<PreviewPane *>();
        QVERIFY(pane);
        m_launcher->showWithQuery({});
        QVERIFY(!pane->isVisible()); // not in the main search
        m_launcher->showView(QStringLiteral("files"), QStringLiteral("notes"));
        QVERIFY(pane->isVisible());
        QTRY_VERIFY(titles().contains(QStringLiteral("notes.md")));
        // The pane shows the selected file once the selection settles.
        QTRY_VERIFY(pane->findChild<QWidget *>(QString(), Qt::FindDirectChildrenOnly) != nullptr);
        key(Qt::Key_K, Qt::ControlModifier);
        QVERIFY(!pane->isVisible()); // the actions list takes the whole width
        key(Qt::Key_Escape);
        QVERIFY(pane->isVisible());
        key(Qt::Key_Escape);
        QVERIFY(!pane->isVisible());
        m_launcher->hideLauncher();
    }

    void emojiSearch()
    {
        m_launcher->showView(QStringLiteral("emoji"), QStringLiteral("shrug"));
        QTRY_VERIFY(titles().contains(QStringLiteral("Person shrugging")));
        m_launcher->hideLauncher();
    }

    void recentFilesWhenEmpty()
    {
        m_launcher->showView(QStringLiteral("files"), {});
        QTRY_COMPARE(selectedTitle(), QStringLiteral("notes.md")); // the newest entry in recently-used.xbel
        QVERIFY(titles().contains(QStringLiteral("tax report.pdf")));
        m_launcher->hideLauncher();
    }

    void customCommandListsLines()
    {
        m_launcher->library()->setCommands({{QStringLiteral("Echo Lines"), QStringLiteral("el"),
                                             QStringLiteral("printf '%s\\n' alpha beta {query}"), Library::Command::List}});
        m_launcher->showWithQuery(QStringLiteral("el it's quoted"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Echo Lines: it's quoted"));
        key(Qt::Key_Return);
        QTRY_VERIFY(titles().contains(QStringLiteral("alpha")));
        QVERIFY(titles().contains(QStringLiteral("it's quoted"))); // passed as one quoted argument
        QVERIFY(m_launcher->isVisible());
        m_launcher->hideLauncher();
    }

    void extensionResults()
    {
        m_launcher->showWithQuery(QStringLiteral("tx hello"));
        QTRY_VERIFY(titles().contains(QStringLiteral("Result for hello")));
        m_launcher->hideLauncher();
    }

    void favouritesRunFromStartScreen()
    {
        m_launcher->showWithQuery(QStringLiteral("search snippets"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Search Snippets"));
        key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(m_launcher->library()->favourites().size(), 1);
        m_launcher->hideLauncher();

        m_launcher->showWithQuery({});
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Search Snippets")); // first on the start screen
        key(Qt::Key_Return);
        QTRY_VERIFY(m_launcher->currentSource()); // it ran: the Snippets view is open
        m_launcher->hideLauncher();
        m_launcher->library()->setFavourites({});
    }

    void aliasBringsUpResult()
    {
        m_launcher->showWithQuery(QStringLiteral("search emoji"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Search Emoji"));
        key(Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClicks(search(), QStringLiteral("em"));
        key(Qt::Key_Return);
        QCOMPARE(m_launcher->library()->aliases().size(), 1);
        QCOMPARE(search()->text(), QStringLiteral("em"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Search Emoji"));
        m_launcher->hideLauncher();
        m_launcher->library()->setAliases({});
    }

    void notesSaveAsYouType()
    {
        m_launcher->showView(QStringLiteral("notes"), {});
        auto *notes = m_launcher->notes();
        QVERIFY(notes->isVisible());
        QVERIFY(!m_launcher->isVisible());
        QTest::keyClicks(notes->findChild<QPlainTextEdit *>(), QStringLiteral("Buy milk"));
        QTRY_VERIFY(QFile::exists(NotesWindow::file()));
        QFile f(NotesWindow::file());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("Buy milk"));
        QCOMPARE(notes->firstLine(), QStringLiteral("Buy milk"));
        notes->close();
    }

    void tabCompletes()
    {
        m_launcher->showWithQuery(QStringLiteral("clipbo"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Clipboard History"));
        key(Qt::Key_Tab);
        QCOMPARE(search()->text(), QStringLiteral("Clipboard History"));
        m_launcher->hideLauncher();
    }

    void syncFolderMovesLibrary()
    {
        Library *library = m_launcher->library();
        const QList<Library::Snippet> original = library->snippets();
        QTemporaryDir synced;
        library->setSnippets({{QStringLiteral("Sig"), QStringLiteral("sig"), QStringLiteral("Cheers")}});
        library->setFolder(synced.path());
        QVERIFY(QFile::exists(synced.filePath(QStringLiteral("library.json"))));
        QCOMPARE(library->snippets().first().text, QStringLiteral("Cheers"));

        // Another computer changes the synced file: picked up.
        QFile f(synced.filePath(QStringLiteral("library.json")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray json = f.readAll().replace("Cheers", "Best wishes");
        f.close();
        QSaveFile out(synced.filePath(QStringLiteral("library.json")));
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(json);
        QVERIFY(out.commit());
        QTRY_COMPARE(library->snippets().first().text, QStringLiteral("Best wishes"));

        library->setFolder({});
        QCOMPARE(library->folder(), Library::defaultFolder());
        library->setSnippets(original);
    }

    void expansionTalksToHelper()
    {
        // initTestCase's launcher connected to the stand-in helper socket.
        QTRY_VERIFY(helperServer()->hasPendingConnections() || m_helperClient);
        if (!m_helperClient)
            m_helperClient = helperServer()->nextPendingConnection();
        QTRY_VERIFY(m_helperClient->bytesAvailable() > 0 || m_helperClient->waitForReadyRead(200));
        const QByteArray keywords = m_helperClient->readAll();
        QVERIFY2(keywords.startsWith("keywords\t"), keywords.constData());
        QVERIFY(keywords.contains("\t;shrug"));

        // A reported match puts the snippet in place (offscreen it cannot type,
        // so it lands on the clipboard instead).
        m_helperClient->write("match\t;shrug\n");
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("¯\\_(ツ)_/¯"));
        QVERIFY(!m_launcher->isVisible());
    }

    void timersAndReminders()
    {
        m_launcher->showWithQuery(QStringLiteral("timer 10m tea"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Start a 10:00 Timer: tea"));
        key(Qt::Key_Return);
        m_launcher->showWithQuery({});
        QTRY_VERIFY(selectedTitle().startsWith(QStringLiteral("tea · "))); // running timers lead the start screen
        key(Qt::Key_Return); // Stop
        m_launcher->showWithQuery(QStringLiteral("remind me in 2 hours to stretch"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("stretch"));
        key(Qt::Key_Return);
        m_launcher->showWithQuery(QStringLiteral("reminders"));
        QTRY_VERIFY(titles().contains(QStringLiteral("stretch")));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("stretch"));
        key(Qt::Key_Return); // Delete Reminder
        QTRY_VERIFY(!titles().contains(QStringLiteral("stretch")));
        m_launcher->hideLauncher();
    }

    void projectsAndSsh()
    {
        m_launcher->showWithQuery(QStringLiteral("demo-app"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("demo-app"));
        QVERIFY(m_launcher->findChild<QListView *>()->currentIndex().data(ResultModel::SubtitleRole).toString().endsWith(QStringLiteral("feature/login")));
        m_launcher->showWithQuery(QStringLiteral("ssh"));
        QTRY_VERIFY(titles().contains(QStringLiteral("SSH to alpha")));
        QVERIFY(titles().contains(QStringLiteral("SSH to beta")));
        QVERIFY(titles().contains(QStringLiteral("SSH to gamma"))); // from known_hosts, "[gamma]:2222"
        for (const QString &t : titles())
            QVERIFY2(!t.contains(u'*') && !t.contains(QLatin1String("|1|")), qPrintable(t)); // no patterns or hashed names
        m_launcher->hideLauncher();
    }

    void appHotkeyCapture()
    {
        m_launcher->showWithQuery(QStringLiteral("konsole"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Konsole"));
        key(Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier);
        key(Qt::Key_Control, Qt::ControlModifier); // a modifier alone does not end the capture
        key(Qt::Key_K, Qt::MetaModifier);
        const KConfigGroup hotkeys = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("AppHotkeys"));
        QCOMPARE(hotkeys.readEntry("org.kde.konsole.desktop"), QStringLiteral("Meta+K"));
        m_launcher->showWithQuery(QStringLiteral("konsole"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Konsole"));
        QVERIFY(list()->currentIndex().data(ResultModel::KindRole).toString().contains(QKeySequence(QStringLiteral("Meta+K")).toString(QKeySequence::NativeText)));
        m_launcher->hideLauncher();
    }

    void secretPrompt()
    {
        m_launcher->showWithQuery({});
        QString answer;
        Item shown;
        shown.title = QStringLiteral("HomeWifi");
        m_launcher->ask(QStringLiteral("Password"), QStringLiteral("Wi-Fi password"), shown, true, [&](const QString &a) { answer = a; });
        QCOMPARE(search()->echoMode(), QLineEdit::Password);
        QCOMPARE(titles(), QStringList{QStringLiteral("HomeWifi")});
        QTest::keyClicks(search(), QStringLiteral("hunter2"));
        key(Qt::Key_Return);
        QCOMPARE(answer, QStringLiteral("hunter2"));
        QCOMPARE(search()->echoMode(), QLineEdit::Normal);
        QVERIFY(!search()->text().contains(QStringLiteral("hunter2")));
        m_launcher->hideLauncher();
    }

    void systemAndDownloads()
    {
        m_launcher->showWithQuery(QStringLiteral("memory"));
        QTRY_VERIFY(selectedTitle().startsWith(QStringLiteral("Memory ")));
        m_launcher->showView(QStringLiteral("downloads"), {});
        QTRY_COMPARE(titles().value(0), QStringLiteral("new.pdf")); // newest first
        QCOMPARE(titles().value(1), QStringLiteral("old.zip"));
        // A file row can be dragged out as a file.
        const auto *model = qobject_cast<const ResultModel *>(list()->model());
        QVERIFY(model->flags(model->index(0)) & Qt::ItemIsDragEnabled);
        std::unique_ptr<QMimeData> drag(model->mimeData({model->index(0)}));
        QCOMPARE(drag->urls().value(0).fileName(), QStringLiteral("new.pdf"));
        m_launcher->showWithQuery(QStringLiteral("latest download"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("new.pdf"));
        m_launcher->hideLauncher();
    }

    void backspaceLeavesView()
    {
        m_launcher->showView(QStringLiteral("snippets"), {});
        QVERIFY(m_launcher->currentSource());
        key(Qt::Key_Backspace);
        QVERIFY(!m_launcher->currentSource());
        m_launcher->hideLauncher();
    }

    void actionsList()
    {
        m_launcher->showView(QStringLiteral("files"), QStringLiteral("tax"));
        QTRY_VERIFY_WITH_TIMEOUT(titles().contains(QStringLiteral("tax report.pdf")), 5000);
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(titles().contains(QStringLiteral("Show in Folder")));
        QVERIFY(titles().contains(QStringLiteral("Copy Path")));
        QTest::keyClicks(search(), QStringLiteral("copy p"));
        QCOMPARE(titles(), QStringList{QStringLiteral("Copy Path")});
        // Leaving the actions keeps the selected file.
        key(Qt::Key_Escape);
        QTRY_COMPARE(selectedTitle(), QStringLiteral("tax report.pdf"));
        key(Qt::Key_K, Qt::ControlModifier);
        QTest::keyClicks(search(), QStringLiteral("copy p"));
        key(Qt::Key_Return);
        QCOMPARE(QGuiApplication::clipboard()->text(), QDir::home().filePath(QStringLiteral("Documents/tax report.pdf")));
        QVERIFY(!m_launcher->isVisible());
    }

    void snippetCopies()
    {
        m_launcher->showView(QStringLiteral("snippets"), QStringLiteral("shrug"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Shrug"));
        key(Qt::Key_Return);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("¯\\_(ツ)_/¯"));
        QVERIFY(!m_launcher->isVisible());
    }

    void quickLinkKeyword()
    {
        m_launcher->showWithQuery(QStringLiteral("gh layer shell"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("GitHub: layer shell"));
        m_launcher->hideLauncher();
    }

    QList<QWidget *> argumentFields() const { return m_launcher->findChildren<QWidget *>(QStringLiteral("argument")); }

    void commandArguments()
    {
        m_launcher->library()->setCommands({{QStringLiteral("Pair Words"), {},
                                             QStringLiteral(R"(printf '%s-%s' {argument name="first"} {argument name="second" default="z"})"),
                                             Library::Command::Copy}});
        m_launcher->showWithQuery(QStringLiteral("pair words"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Pair Words"));
        key(Qt::Key_Return);
        QTRY_COMPARE(argumentFields().size(), 2);
        QVERIFY(!search()->isVisible());
        // The first is required: Enter waits for it.
        QTest::keyClick(argumentFields().first(), Qt::Key_Return);
        QCOMPARE(argumentFields().size(), 2);
        QTest::keyClicks(argumentFields().first(), QStringLiteral("it's"));
        QTest::keyClick(argumentFields().first(), Qt::Key_Return);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("it's-z")); // quoted, default used
        QVERIFY(argumentFields().isEmpty());
        QVERIFY(!m_launcher->isVisible());

        // Esc leaves the fields and restores the search.
        m_launcher->showWithQuery(QStringLiteral("pair words"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Pair Words"));
        key(Qt::Key_Tab); // Tab opens them too
        QTRY_COMPARE(argumentFields().size(), 2);
        QTest::keyClick(argumentFields().first(), Qt::Key_Escape);
        QVERIFY(argumentFields().isEmpty());
        QCOMPARE(search()->text(), QStringLiteral("pair words"));
        QVERIFY(search()->isVisible());
        m_launcher->hideLauncher();
        m_launcher->library()->setCommands({});
    }

    void raycastScriptCommand()
    {
        m_launcher->showWithQuery(QStringLiteral("say hello"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Say Hello"));
        key(Qt::Key_Return);
        QTRY_COMPARE(argumentFields().size(), 2);
        QTest::keyClicks(argumentFields().first(), QStringLiteral("Lumen"));
        QTest::keyClick(argumentFields().first(), Qt::Key_Return);
        // fullOutput: a view with what it printed.
        QTRY_VERIFY(titles().contains(QStringLiteral("Hello Lumen")));
        QTRY_VERIFY(titles().contains(QStringLiteral("from the script folder")));
        QVERIFY(m_launcher->currentSource());
        key(Qt::Key_Escape);
        QVERIFY(!m_launcher->currentSource());
        m_launcher->hideLauncher();
    }

    void extensionView()
    {
        m_launcher->showWithQuery(QStringLiteral("test view"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Test View"));
        key(Qt::Key_Return);
        QTRY_VERIFY(m_launcher->currentSource());
        Source *view = m_launcher->currentSource();
        QTest::keyClicks(search(), QStringLiteral("abc"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Item abc"));
        // Its detail is in the pane; Enter pushes the nested list.
        QVERIFY(m_launcher->findChild<PreviewPane *>()->isVisible());
        key(Qt::Key_Return);
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Child of child-abc"));
        QVERIFY(m_launcher->currentSource() != view);
        // Esc goes back one level, keeping what was typed there.
        key(Qt::Key_Escape);
        QCOMPARE(m_launcher->currentSource(), view);
        QCOMPARE(search()->text(), QStringLiteral("abc"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Item abc"));
        // The second action, from the actions list.
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(titles().contains(QStringLiteral("Copy It")));
        QTest::keyClicks(search(), QStringLiteral("copy it"));
        key(Qt::Key_Return);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("copied abc"));
        m_launcher->hideLauncher();
    }

    void fallbackCommands()
    {
        m_launcher->showWithQuery(QStringLiteral("zzqxv"));
        QTRY_VERIFY(!titles().isEmpty() && titles().last() == QStringLiteral("Search Files for “zzqxv”"));
        m_launcher->library()->setFallbacks({QStringLiteral("link:GitHub"), QStringLiteral("lumen:files-for")});
        QTRY_VERIFY(titles().size() >= 2 && titles().mid(titles().size() - 2)
                        == (QStringList{QStringLiteral("Search GitHub for “zzqxv”"), QStringLiteral("Search Files for “zzqxv”")}));
        // Ctrl+K on a fallback-capable result offers to add or remove it.
        m_launcher->setQuery(QStringLiteral("say hello"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Say Hello"));
        key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(titles().contains(QStringLiteral("Use as Fallback")));
        m_launcher->hideLauncher();
        m_launcher->library()->setFallbacks({QStringLiteral("lumen:files-for")});
    }

    void snippetArguments()
    {
        const QList<Library::Snippet> saved = m_launcher->library()->snippets();
        m_launcher->library()->setSnippets(saved + QList<Library::Snippet>{
            {QStringLiteral("Greeting Letter"), QStringLiteral("dear"), QStringLiteral(R"(Dear {argument name="who" placeholder="Name"},{cursor})")}});
        m_launcher->showView(QStringLiteral("snippets"), QStringLiteral("greeting letter"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Greeting Letter"));
        key(Qt::Key_Return);
        QTRY_COMPARE(argumentFields().size(), 1);
        QTest::keyClicks(argumentFields().first(), QStringLiteral("Ann"));
        QTest::keyClick(argumentFields().first(), Qt::Key_Return);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Dear Ann,"));
        QVERIFY(!m_launcher->isVisible());
        m_launcher->library()->setSnippets(saved);
    }

    void pasteStackInOrder()
    {
        PasteStack *stack = m_launcher->pasteStack();
        stack->start();
        QVERIFY(stack->isActive());
        // Copies join the stack (here through the clipboard history).
        for (const char *text : {"first copy", "second copy"}) {
            QGuiApplication::clipboard()->setText(QString::fromLatin1(text));
            QTRY_COMPARE(m_launcher->clipboardHistory()->entries().value(0).text, QString::fromLatin1(text));
        }
        QTRY_COMPARE(stack->items().size(), 2);
        m_launcher->showView(QStringLiteral("paste-stack"), {});
        QTRY_COMPARE(titles(), (QStringList{QStringLiteral("first copy"), QStringLiteral("second copy")}));
        m_launcher->hideLauncher();
        // Ctrl+Alt+V takes the oldest; putting it on the clipboard is not a new copy.
        stack->pasteNext();
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("first copy"));
        QTest::qWait(300);
        QCOMPARE(stack->items().size(), 1);
        stack->pasteNext();
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("second copy"));
        QVERIFY(!stack->isActive()); // empty: done
    }

    void imagesFoundByTheirText()
    {
        m_launcher->clipboardHistory()->setRecognizer({QStringLiteral("echo"), QStringLiteral("Invoice 42 from ACME")});
        QImage image(24, 24, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QGuiApplication::clipboard()->setImage(image);
        QTRY_VERIFY(m_launcher->clipboardHistory()->entries().value(0).recognized);
        m_launcher->showView(QStringLiteral("clipboard"), QStringLiteral("invoice"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Invoice 42 from ACME"));
        m_launcher->hideLauncher();
        m_launcher->clipboardHistory()->setRecognizer({});
    }

    void menuSearch()
    {
        FakeMenu menu;
        QVERIFY(QDBusConnection::sessionBus().registerObject(QStringLiteral("/MenuBar"), &menu, QDBusConnection::ExportAllSlots));
        m_launcher->showView(QStringLiteral("menu"), {});
        auto *source = static_cast<MenuSource *>(m_launcher->currentSource());
        source->load(QDBusConnection::sessionBus().baseService(), QStringLiteral("/MenuBar"), QStringLiteral("Kate"));
        // Every command, with its menu; the lazily filled submenu was asked for.
        QTRY_VERIFY(titles().contains(QStringLiteral("notes.md")));
        QCOMPARE(titles(), (QStringList{QStringLiteral("Save"), QStringLiteral("Save As…"), QStringLiteral("notes.md"), QStringLiteral("Find…")}));
        QCOMPARE(list()->model()->index(2, 0).data(ResultModel::SubtitleRole).toString(), QStringLiteral("File › Open Recent"));
        QCOMPARE(list()->model()->index(0, 0).data(ResultModel::KindRole).toString(), QStringLiteral("Ctrl+S"));
        QTest::keyClicks(search(), QStringLiteral("find"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Find…"));
        key(Qt::Key_Return);
        QTRY_COMPARE(menu.clicked, 9);
        QVERIFY(!m_launcher->isVisible());
        QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/MenuBar"));
    }

    void focusSession()
    {
        FocusMode *focus = m_launcher->focusMode();
        focus->start(30, QStringLiteral("Write the report"));
        QVERIFY(focus->isActive());
        // The countdown runs in the timer badge, and leads the start screen.
        const auto timers = m_launcher->findChild<Timekeeper *>()->timers();
        QVERIFY(std::any_of(timers.cbegin(), timers.cend(), [](const Timekeeper::Timer &t) { return t.label == QStringLiteral("Write the report"); }));
        m_launcher->showWithQuery({});
        QTRY_COMPARE(titles().value(0), QStringLiteral("Focus · 30 min left"));
        m_launcher->setQuery(QStringLiteral("focus"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Focus · 30 min left"));
        key(Qt::Key_Return); // ends it
        QTRY_VERIFY(!focus->isActive());
        const auto after = m_launcher->findChild<Timekeeper *>()->timers();
        QVERIFY(std::none_of(after.cbegin(), after.cend(), [](const Timekeeper::Timer &t) { return t.label == QStringLiteral("Write the report"); }));
        // Starting asks how long, and for what.
        m_launcher->setQuery(QStringLiteral("start focus"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Start Focus Session"));
        key(Qt::Key_Return);
        QTRY_COMPARE(argumentFields().size(), 2);
        QTest::keyClicks(argumentFields().first(), QStringLiteral("45"));
        QTest::keyClick(argumentFields().first(), Qt::Key_Return);
        QTRY_VERIFY(focus->isActive());
        QVERIFY(qAbs(QDateTime::currentDateTime().secsTo(focus->until()) - 45 * 60) < 5);
        focus->stop(false);
        m_launcher->hideLauncher();
    }

    void savedWindowLayouts()
    {
        auto *layouts = m_launcher->findChild<WindowLayoutSource *>();
        QVERIFY(layouts);
        // What KWin's script reports for the windows of the desktop.
        layouts->captured(QStringLiteral("Coding"), QStringLiteral(R"([{"app": "org.kde.kate", "cls": "kate", "caption": "a.cpp", "x": 0, "y": 0, "width": 800, "height": 900},
                                                                    {"app": "org.kde.konsole", "cls": "konsole", "caption": "~", "x": 800, "y": 0, "width": 800, "height": 900}])"));
        QCOMPARE(m_launcher->library()->arrangements().size(), 1);
        m_launcher->showWithQuery(QStringLiteral("coding"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Coding"));
        QCOMPARE(list()->currentIndex().data(ResultModel::SubtitleRole).toString(), QStringLiteral("2 windows · kate, konsole"));
        m_launcher->setQuery(QStringLiteral("save window layout"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Save Window Layout"));
        m_launcher->hideLauncher();
        m_launcher->library()->setArrangements({});
    }

    void browserHistoryInSearch()
    {
        // The Firefox profile set up in main(): loaded when Lumen opens.
        m_launcher->showWithQuery({});
        m_launcher->setQuery(QStringLiteral("lumen guide"));
        QTRY_VERIFY_WITH_TIMEOUT(titles().contains(QStringLiteral("Lumen Guide – Example")), 5000);
        const int row = int(titles().indexOf(QStringLiteral("Lumen Guide – Example")));
        QCOMPARE(list()->model()->index(row, 0).data(ResultModel::KindRole).toString(), QStringLiteral("History · Firefox"));
        m_launcher->showView(QStringLiteral("history"), {});
        QTRY_COMPARE(titles().value(0), QStringLiteral("Lumen Guide – Example"));
        m_launcher->hideLauncher();
    }

    void browserTabsNeedTheExtension()
    {
        m_launcher->showView(QStringLiteral("tabs"), {});
        QTRY_COMPARE(selectedTitle(), QStringLiteral("No tabs found"));
        m_launcher->hideLauncher();
    }

    void calculationsAreKept()
    {
        m_launcher->showWithQuery(QStringLiteral("15% of 80"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("12"));
        key(Qt::Key_Return);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("12"));
        m_launcher->showView(QStringLiteral("calculations"), {});
        QTRY_COMPARE(selectedTitle(), QStringLiteral("12"));
        QCOMPARE(list()->currentIndex().data(ResultModel::SubtitleRole).toString(), QStringLiteral("15% of 80"));
        m_launcher->hideLauncher();
    }

    void reopensWhereItWas()
    {
        m_launcher->showView(QStringLiteral("snippets"), QStringLiteral("shr"));
        Source *snippets = m_launcher->currentSource();
        m_launcher->toggle(); // closes
        QVERIFY(!m_launcher->isVisible());
        m_launcher->toggle(); // soon after: the same place, the text selected
        QCOMPARE(m_launcher->currentSource(), snippets);
        QCOMPARE(search()->text(), QStringLiteral("shr"));
        QCOMPARE(search()->selectedText(), QStringLiteral("shr"));
        // Back and forward.
        key(Qt::Key_BracketLeft, Qt::ControlModifier);
        QVERIFY(!m_launcher->currentSource());
        key(Qt::Key_BracketRight, Qt::ControlModifier);
        QCOMPARE(m_launcher->currentSource(), snippets);
        QCOMPARE(search()->text(), QStringLiteral("shr"));
        // After running something it starts afresh.
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Shrug"));
        key(Qt::Key_Return);
        QVERIFY(!m_launcher->isVisible());
        m_launcher->toggle();
        QVERIFY(!m_launcher->currentSource());
        QVERIFY(search()->text().isEmpty());
        m_launcher->hideLauncher();
    }

    void themes()
    {
        const QPalette before = QGuiApplication::palette();
        Theme::apply(QStringLiteral("midnight"), QStringLiteral("pink"));
        // Widgets hear of a new application palette a moment later.
        QTRY_COMPARE(m_launcher->palette().color(QPalette::Window), QColor(0x12, 0x18, 0x28));
        QCOMPARE(m_launcher->palette().color(QPalette::Highlight), QColor(0xff, 0x5f, 0xa2));
        m_launcher->showView(QStringLiteral("theme"), QStringLiteral("paper"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Paper"));
        key(Qt::Key_Return); // applies it, staying open
        QTRY_COMPARE(m_launcher->palette().color(QPalette::Window), QColor(0xf3, 0xee, 0xe4));
        QVERIFY(m_launcher->isVisible());
        m_launcher->hideLauncher();
        Theme::apply(QStringLiteral("system"), QStringLiteral("system"));
        QCOMPARE(QGuiApplication::palette(), before);
        QTRY_COMPARE(m_launcher->palette().color(QPalette::Window), before.color(QPalette::Window));
    }

    void windowLayoutsAreFound()
    {
        m_launcher->showWithQuery(QStringLiteral("left half"));
        QTRY_COMPARE(selectedTitle(), QStringLiteral("Left Half"));
        m_launcher->hideLauncher();
    }
};

int main(int argc, char **argv)
{
    // A throw-away home folder with a few files to find.
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/.config")).toUtf8());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/.local/share")).toUtf8());
    qputenv("XDG_STATE_HOME", (home.path() + QStringLiteral("/.local/state")).toUtf8());
    QDir(home.path()).mkpath(QStringLiteral("Documents"));
    for (const char *name : {"Documents/tax report.pdf", "Documents/report-2025.txt", "Documents/notes.md"}) {
        QFile f(QDir(home.path()).filePath(QLatin1String(name)));
        if (!f.open(QIODevice::WriteOnly))
            return 1;
        f.write("sample\n");
    }

    // Two recently used files, as Dolphin records them.
    QDir(home.path()).mkpath(QStringLiteral(".local/share"));
    QFile xbel(QDir(home.path()).filePath(QStringLiteral(".local/share/recently-used.xbel")));
    if (!xbel.open(QIODevice::WriteOnly))
        return 1;
    const QString docs = QUrl::fromLocalFile(QDir(home.path()).filePath(QStringLiteral("Documents"))).toString();
    xbel.write(QStringLiteral("<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
                              "<bookmark href=\"%1/tax%20report.pdf\" modified=\"2026-10-01T10:00:00.000000Z\"/>\n"
                              "<bookmark href=\"%1/notes.md\" modified=\"2026-11-01T10:00:00.000000Z\"/>\n"
                              "</xbel>\n").arg(docs).toUtf8());
    xbel.close();

    // A git project and some SSH hosts.
    QDir(home.path()).mkpath(QStringLiteral("Projects/demo-app/.git"));
    QDir(home.path()).mkpath(QStringLiteral(".ssh"));
    for (const auto &[file, content] : {std::pair{"Projects/demo-app/.git/HEAD", "ref: refs/heads/feature/login\n"},
                                        std::pair{".ssh/config", "Host alpha beta\n  User me\nHost *.internal\n"},
                                        std::pair{".ssh/known_hosts", "[gamma]:2222 ssh-ed25519 AAAA\n|1|abc=|def= ssh-rsa AAAA\n"}}) {
        QFile f(QDir(home.path()).filePath(QLatin1String(file)));
        if (!f.open(QIODevice::WriteOnly))
            return 1;
        f.write(content);
    }

    // Two downloads, an hour apart.
    QDir(home.path()).mkpath(QStringLiteral("Downloads"));
    for (const auto &[name, age] : {std::pair{"old.zip", 7200}, std::pair{"new.pdf", 60}}) {
        QFile f(QDir(home.path()).filePath(QStringLiteral("Downloads/") + QLatin1String(name)));
        if (!f.open(QIODevice::WriteOnly))
            return 1;
        f.write("x");
        f.setFileTime(QDateTime::currentDateTime().addSecs(-age), QFileDevice::FileModificationTime);
    }

    // A tiny extension (keyword "tx").
    QDir(home.path()).mkpath(QStringLiteral(".config/lumen/extensions"));
    QFile ext(QDir(home.path()).filePath(QStringLiteral(".config/lumen/extensions/test-ext")));
    if (!ext.open(QIODevice::WriteOnly))
        return 1;
    ext.write("#!/bin/sh\ncase \"$1\" in\n"
              "info) echo '{\"name\": \"Test\", \"keyword\": \"tx\"}' ;;\n"
              "search) printf '[{\"title\": \"Result for %s\", \"copy\": \"%s\"}]' \"$2\" \"$2\" ;;\n"
              "esac\n");
    ext.close();
    ext.setPermissions(ext.permissions() | QFileDevice::ExeOwner);

    // An extension with a view of its own (keyword "tv"): details, a nested
    // list and a second action.
    QFile view(QDir(home.path()).filePath(QStringLiteral(".config/lumen/extensions/test-view")));
    if (!view.open(QIODevice::WriteOnly))
        return 1;
    view.write("#!/bin/sh\ncase \"$1\" in\n"
               "info) echo '{\"name\": \"Test View\", \"keyword\": \"tv\", \"preview\": true}' ;;\n"
               "search) printf '[{\"title\": \"Item %s\", \"detail\": \"# About %s\", \"push\": \"child-%s\", "
               "\"actions\": [{\"title\": \"Copy It\", \"copy\": \"copied %s\"}]}]' \"$2\" \"$2\" \"$2\" \"$2\" ;;\n"
               "open) printf '[{\"title\": \"Child of %s\", \"copy\": \"%s\"}]' \"$2\" \"$2\" ;;\n"
               "esac\n");
    view.close();
    view.setPermissions(view.permissions() | QFileDevice::ExeOwner);

    // A Raycast script command with two arguments.
    QDir(home.path()).mkpath(QStringLiteral(".config/lumen/script-commands"));
    QFile script(QDir(home.path()).filePath(QStringLiteral(".config/lumen/script-commands/hello.sh")));
    if (!script.open(QIODevice::WriteOnly))
        return 1;
    script.write("#!/bin/sh\n"
                 "# @raycast.schemaVersion 1\n# @raycast.title Say Hello\n# @raycast.mode fullOutput\n"
                 "# @raycast.packageName Greetings\n# @raycast.icon 👋\n"
                 "# @raycast.argument1 { \"type\": \"text\", \"placeholder\": \"Name\" }\n"
                 "# @raycast.argument2 { \"type\": \"text\", \"placeholder\": \"Greeting\", \"optional\": true }\n"
                 "echo \"${2:-Hello} $1\"\n"
                 "[ \"$(basename \"$PWD\")\" = script-commands ] && echo from the script folder\n");
    script.close();

    // A Firefox profile with one visited page.
    QDir(home.path()).mkpath(QStringLiteral(".config/mozilla/firefox/abc.default"));
    QFile profiles(QDir(home.path()).filePath(QStringLiteral(".config/mozilla/firefox/profiles.ini")));
    if (!profiles.open(QIODevice::WriteOnly))
        return 1;
    profiles.write("[Profile0]\nName=default\nIsRelative=1\nPath=abc.default\nDefault=1\n");
    profiles.close();

    qputenv("LUMEN_EXPAND_SOCKET", QDir(home.path()).filePath(QStringLiteral("expand.socket")).toUtf8());

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    // (SQLite needs the application to exist.)
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("setup"));
        db.setDatabaseName(QDir(home.path()).filePath(QStringLiteral(".config/mozilla/firefox/abc.default/places.sqlite")));
        if (!db.open())
            return 1;
        QSqlQuery q(db);
        q.exec(QStringLiteral("CREATE TABLE moz_places (url TEXT, title TEXT, visit_count INT, last_visit_date INT, hidden INT, frecency INT)"));
        q.exec(QStringLiteral("INSERT INTO moz_places VALUES ('https://example.org/lumen', 'Lumen Guide – Example', 4, %1, 0, 100)")
                   .arg(QDateTime::currentMSecsSinceEpoch() * 1000));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("setup"));
    LauncherTest test;
    const int failures = QTest::qExec(&test, argc, argv);
    // Skip the teardown, as lumen's main() does: KRunner's plugin threads can
    // outlive the application object.
    home.remove();
    std::_Exit(failures);
}

#include "launchertest.moc"
