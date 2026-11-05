// The argument format shared by commands, quick links and snippets, and
// reading Raycast script commands' metadata.

#include "arguments.h"
#include "library.h"
#include "outputview.h"
#include "browserhistory.h"
#include "focusmode.h"
#include "theme.h"
#include "scriptcommands.h"
#include "windowlayouts.h"
#include "windowtiling.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QJsonObject>

#include <QTemporaryDir>
#include <QTest>

class ScriptTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void argumentsFromTemplate()
    {
        const QList<Argument> args = Arguments::fromTemplate(
            QStringLiteral(R"(translate {argument name="text" placeholder="Text"} to {argument name='to' default="en" options="en, de,fr"} {argument name="text"} {query})"));
        QCOMPARE(args.size(), 3); // "text" is asked once
        QCOMPARE(args.at(0).name, QStringLiteral("text"));
        QCOMPARE(args.at(0).label(), QStringLiteral("Text"));
        QVERIFY(!args.at(0).optional);
        QCOMPARE(args.at(1).name, QStringLiteral("to"));
        QVERIFY(args.at(1).optional); // it has a default
        QCOMPARE(args.at(1).options, (QStringList{QStringLiteral("en"), QStringLiteral("de"), QStringLiteral("fr")}));
        QCOMPARE(args.at(2).name, QStringLiteral("query"));
        QCOMPARE(Arguments::fromTemplate(QStringLiteral("{argument}")).first().name, QStringLiteral("argument"));
        QVERIFY(Arguments::fromTemplate(QStringLiteral("no {arguments} here {argument nme}")).isEmpty());
    }

    void fillingArguments()
    {
        const QString tpl = QStringLiteral(R"(echo {argument name="a"} {argument name="b" default="B"} {query})");
        const QString filled = Arguments::fill(tpl, {{QStringLiteral("a"), QStringLiteral("it's")}, {QStringLiteral("query"), QStringLiteral("q")}},
                                               [](const QString &v) { return u'[' + v + u']'; });
        QCOMPARE(filled, QStringLiteral("echo [it's] [B] [q]"));
        const auto byName = Arguments::byName(Arguments::fromTemplate(tpl), {QStringLiteral("1"), QStringLiteral("2")});
        QCOMPARE(byName.value(QStringLiteral("b")), QStringLiteral("2"));
    }

    void raycastMetadata()
    {
        const QString script = QStringLiteral(R"(#!/bin/bash

# Required parameters:
# @raycast.schemaVersion 1
# @raycast.title Open Session
# @raycast.mode inline
# @raycast.refreshTime 5s

# Optional parameters:
# @raycast.icon 🤖
# @raycast.packageName Developer Utils
# @raycast.needsConfirmation true
# @raycast.currentDirectoryPath ~/Projects
# @raycast.argument1 { "type": "text", "placeholder": "Query", "percentEncoded": true }
# @raycast.argument2 { "type": "dropdown", "placeholder": "Where", "data": [{"title": "Here", "value": "h"}, {"title": "There", "value": "t"}] }
# @raycast.argument3 { "type": "password", "placeholder": "Secret", "optional": true }
echo hi
)");
        const auto c = ScriptCommand::parse(QStringLiteral("/tmp/s.sh"), script);
        QVERIFY(c);
        QCOMPARE(c->title, QStringLiteral("Open Session"));
        QCOMPARE(c->mode, ScriptCommand::Inline);
        QCOMPARE(c->refreshSeconds, 10); // Raycast's minimum
        QCOMPARE(c->packageName, QStringLiteral("Developer Utils"));
        QCOMPARE(c->icon, QStringLiteral("🤖"));
        QVERIFY(c->needsConfirmation);
        QCOMPARE(c->directory, QDir::homePath() + QStringLiteral("/Projects"));
        QCOMPARE(c->arguments.size(), 3);
        QCOMPARE(c->arguments.at(1).options, (QStringList{QStringLiteral("Here"), QStringLiteral("There")}));
        QCOMPARE(c->arguments.at(1).valueAt(1), QStringLiteral("t"));
        QVERIFY(c->arguments.at(2).secret);
        QVERIFY(c->arguments.at(2).optional);
        QCOMPARE(c->percentEncoded, (QList<bool>{true, false, false}));

        // Not a script command: no schema version, or no title.
        QVERIFY(!ScriptCommand::parse(QStringLiteral("/x"), QStringLiteral("# @raycast.title X\n")));
        QVERIFY(!ScriptCommand::parse(QStringLiteral("/x"), QStringLiteral("# @raycast.schemaVersion 1\n")));
        // Other comment styles.
        const auto js = ScriptCommand::parse(QStringLiteral("/x.js"), QStringLiteral("// @raycast.schemaVersion 1\n// @raycast.title JS\n// @raycast.mode silent\n"));
        QVERIFY(js);
        QCOMPARE(js->mode, ScriptCommand::Silent);
        QCOMPARE(js->directory, QStringLiteral("/"));
    }

    void durations()
    {
        QCOMPARE(ScriptCommand::parseDuration(QStringLiteral("30s")), 30);
        QCOMPARE(ScriptCommand::parseDuration(QStringLiteral("10m")), 600);
        QCOMPARE(ScriptCommand::parseDuration(QStringLiteral("1h")), 3600);
        QCOMPARE(ScriptCommand::parseDuration(QStringLiteral("2d")), 172800);
        QCOMPARE(ScriptCommand::parseDuration(QStringLiteral("soon")), 0);
    }

    void commandLines()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("s.py"));
        const QByteArray content = "#!/usr/bin/env python3\n# @raycast.schemaVersion 1\n# @raycast.title T\n"
                                   "# @raycast.argument1 { \"type\": \"text\", \"placeholder\": \"Q\", \"percentEncoded\": true }\n";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
        const auto c = ScriptCommand::parse(path, QString::fromUtf8(content));
        QVERIFY(c);
        // Not executable: through the interpreter its first line names.
        QCOMPARE(c->commandLine({QStringLiteral("a b")}),
                 (QStringList{QStringLiteral("/usr/bin/env"), QStringLiteral("python3"), path, QStringLiteral("a%20b")}));
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
        QCOMPARE(c->commandLine({QStringLiteral("x")}), (QStringList{path, QStringLiteral("x")}));
    }

    void snippetPlaceholders()
    {
        const QDateTime now(QDate(2026, 11, 4), QTime(10, 0));
        auto expand = [&](const QString &text) { return Library::expandSnippet(text, {}, [](int n) { return QStringLiteral("clip%1").arg(n); }, now); };
        QCOMPARE(expand(QStringLiteral(R"({date +3d format="yyyy-MM-dd"})")).text, QStringLiteral("2026-11-07"));
        QCOMPARE(expand(QStringLiteral(R"({date offset="-1M" format="yyyy-MM"})")).text, QStringLiteral("2026-10"));
        QCOMPARE(expand(QStringLiteral(R"({time +90m format="HH:mm"})")).text, QStringLiteral("11:30"));
        QCOMPARE(expand(QStringLiteral(R"({date +1w-1d format='d'})")).text, QStringLiteral("10"));
        QCOMPARE(expand(QStringLiteral("{day +1d}")).text, QLocale().dayName(4)); // a Thursday
        QCOMPARE(expand(QStringLiteral("{clipboard} / {clipboard offset=2}")).text, QStringLiteral("clip0 / clip2"));
        QCOMPARE(expand(QStringLiteral("{uuid}")).text.size(), 36);
        QCOMPARE(expand(QStringLiteral("{nothing} {date")).text, QStringLiteral("{nothing} {date")); // left alone

        const auto cursor = Library::expandSnippet(QStringLiteral(R"(Dear {argument name="who"},{cursor}

Best, me 👋)"), {{QStringLiteral("who"), QStringLiteral("Ann")}});
        QCOMPARE(cursor.text, QStringLiteral("Dear Ann,\n\nBest, me 👋"));
        QCOMPARE(cursor.cursorBack, 12); // "\n\nBest, me 👋": the emoji is one character
        QCOMPARE(Library::expandSnippet(QStringLiteral("no cursor")).cursorBack, 0);
    }

    void windowScripts()
    {
        QTemporaryDir runtime;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        // Every window command has a script; those that move or resize the
        // window remember where it was first.
        for (const WindowTiling::Layout &l : WindowTiling::layouts()) {
            const QString script = WindowTiling::scriptFor(l.id);
            QVERIFY2(script.contains(QLatin1String("workspace.activeWindow")), qPrintable(l.id));
            const bool keepsPlace = QStringList{QStringLiteral("restore"), QStringLiteral("fullscreen"), QStringLiteral("keep-above"),
                                                QStringLiteral("next-desktop"), QStringLiteral("previous-desktop")}
                                        .contains(l.id);
            QCOMPARE(script.contains(QLatin1String("\"remember\"")), !keepsPlace);
        }
        QVERIFY(WindowTiling::scriptFor(QStringLiteral("next-screen")).contains(QLatin1String("sendClientToScreen")));
        // Restoring puts back what was remembered.
        WindowTiling::remember(QStringLiteral("{abc}"), QStringLiteral(R"({"x": 10, "y": 20, "width": 300, "height": 200})"));
        WindowTiling::remember(QStringLiteral("{def}"), QStringLiteral(R"({"x": 1, "y": 2, "width": 3, "height": 4})"));
        const QString restore = WindowTiling::scriptFor(QStringLiteral("restore"));
        QVERIFY(restore.contains(QLatin1String(R"("{abc}":{"height":200,"t":)")));
        QVERIFY(restore.contains(QLatin1String("{def}")));

        const QJsonArray saved{QJsonObject{{QStringLiteral("app"), QStringLiteral("org.kde.kate")}, {QStringLiteral("x"), 0}}};
        const QString layout = WindowLayoutSource::restoreScript(saved, true);
        QVERIFY(layout.contains(QLatin1String(R"([{"app":"org.kde.kate","x":0}])")));
        QVERIFY(layout.contains(QLatin1String("if (true && missing.length)")));
        QVERIFY(WindowLayoutSource::restoreScript(saved, false).contains(QLatin1String("if (false && missing.length)")));
        QVERIFY(WindowLayoutSource::captureScript(QStringLiteral("Co\"ding")).contains(QLatin1String("\"captured\", \"Co\\\"ding\"")));
    }

    void focusScript()
    {
        const QString script = FocusMode::script({QStringLiteral("Discord.desktop")},
                                                 {QStringLiteral("https://www.YouTube.com/feed"), QStringLiteral("x.com"), QStringLiteral(" ")});
        QVERIFY(script.contains(QLatin1String(R"(const apps = ["discord"], sites = ["youtube","x.com"])"))); // short names by domain
        QVERIFY(script.contains(QLatin1String("\"firefox\"")));
        QVERIFY(script.contains(QLatin1String("w.minimized = true")));
    }

    void browserHistories()
    {
        QTemporaryDir dir;
        auto make = [&](const QString &file, const QStringList &statements) {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), file);
            db.setDatabaseName(dir.filePath(file));
            QVERIFY(db.open());
            QSqlQuery q(db);
            for (const QString &s : statements)
                QVERIFY2(q.exec(s), qPrintable(s));
            db.close();
        };
        // 2026-11-04 10:00 UTC in each browser's own time format.
        const qint64 unixUs = QDateTime(QDate(2026, 11, 4), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch() * 1000;
        make(QStringLiteral("places.sqlite"),
             {QStringLiteral("CREATE TABLE moz_places (url TEXT, title TEXT, visit_count INT, last_visit_date INT, hidden INT, frecency INT)"),
              QStringLiteral("INSERT INTO moz_places VALUES ('https://kde.org/', 'KDE', 3, %1, 0, 100)").arg(unixUs),
              QStringLiteral("INSERT INTO moz_places VALUES ('https://hidden.example/', 'Hidden', 1, %1, 1, 50)").arg(unixUs),
              QStringLiteral("INSERT INTO moz_places VALUES ('place:sort=8', 'Most visited', 0, %1, 0, 10)").arg(unixUs)});
        make(QStringLiteral("History"),
             {QStringLiteral("CREATE TABLE urls (url TEXT, title TEXT, visit_count INT, last_visit_time INT, hidden INT)"),
              QStringLiteral("INSERT INTO urls VALUES ('https://qt.io/', 'Qt', 7, %1, 0)").arg(unixUs + 11644473600000000LL)});
        const auto firefox = BrowserHistory::readFirefox(dir.filePath(QStringLiteral("places.sqlite")), QStringLiteral("Firefox"));
        QCOMPARE(firefox.size(), 1); // not hidden, web pages only
        QCOMPARE(firefox.first().title, QStringLiteral("KDE"));
        QCOMPARE(firefox.first().visits, 3);
        QCOMPARE(firefox.first().last.toUTC(), QDateTime(QDate(2026, 11, 4), QTime(10, 0), QTimeZone::UTC));
        const auto chrome = BrowserHistory::readChromium(dir.filePath(QStringLiteral("History")), QStringLiteral("Brave"));
        QCOMPARE(chrome.size(), 1);
        QCOMPARE(chrome.first().url, QStringLiteral("https://qt.io/"));
        QCOMPARE(chrome.first().last.toUTC(), QDateTime(QDate(2026, 11, 4), QTime(10, 0), QTimeZone::UTC));
    }

    void themePalettes()
    {
        const QPalette system = QGuiApplication::palette();
        QCOMPARE(Theme::palette(QStringLiteral("system"), QStringLiteral("system"), system), system);
        const QPalette midnight = Theme::palette(QStringLiteral("midnight"), QStringLiteral("system"), system);
        QCOMPARE(midnight.color(QPalette::Window), QColor(0x12, 0x18, 0x28));
        QCOMPARE(midnight.color(QPalette::Highlight), QColor(0x5a, 0xc8, 0xfa));
        // An accent over a theme; dark text on a light accent.
        const QPalette yellow = Theme::palette(QStringLiteral("paper"), QStringLiteral("yellow"), system);
        QCOMPARE(yellow.color(QPalette::Window), QColor(0xf3, 0xee, 0xe4));
        QCOMPARE(yellow.color(QPalette::Highlight), QColor(0xe8, 0xb5, 0x00));
        QCOMPARE(Theme::palette(QStringLiteral("dark"), QStringLiteral("blue"), system).color(QPalette::HighlightedText), QColor(Qt::white));
    }

    void plainOutput()
    {
        QCOMPARE(OutputView::plain(QStringLiteral("\x1B[1;32mok\x1B[0m done\r\n")), QStringLiteral("ok done\n"));
    }
};

QTEST_MAIN(ScriptTest)
#include "scripttest.moc"
