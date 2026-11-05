// Lumen: a quick, keyboard-driven launcher for KDE Plasma.
//
//   lumen               Start in the background (does nothing if running).
//   lumen --toggle      Show or hide the launcher.
//   lumen --show TEXT   Show the launcher with TEXT already typed.
//   lumen --settings    Open the settings window.
//   lumen --view NAME   Show a view: clipboard, files, snippets, emoji,
//                       windows or notes (text
//                       after it is typed into the search).
//   lumen --tile LAYOUT Move the active window, e.g. left-half, center-third.
//
// Meta+Space (changeable) shows and hides it once it runs.

#include "clipboardhistory.h"
#include "launcher.h"
#include "settingsdialog.h"
#include "windowtiling.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QIcon>

#include <cstdlib>

namespace
{

const QString Service = QStringLiteral("io.github.lumen");
const QString Path = QStringLiteral("/Launcher");
const QString Interface = QStringLiteral("io.github.lumen.Launcher");

// The D-Bus face of the running instance, used by the command line.
class LauncherAdaptor : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.lumen.Launcher")
public:
    explicit LauncherAdaptor(Launcher *launcher)
        : QObject(launcher)
        , m_launcher(launcher)
    {
    }

public Q_SLOTS:
    Q_SCRIPTABLE void toggle() { m_launcher->toggle(); }
    Q_SCRIPTABLE void show(const QString &query) { m_launcher->showWithQuery(query); }
    Q_SCRIPTABLE void settings() { m_launcher->openSettings(Launcher::GeneralTab, false); }
    Q_SCRIPTABLE bool view(const QString &name, const QString &query) { return m_launcher->showView(name, query); }
    // Called by the KWin script behind app hotkeys when the app is not running.
    Q_SCRIPTABLE void launchApp(const QString &storageId) { m_launcher->launchApp(storageId); }

private:
    Launcher *m_launcher;
};

// Sends a request to the running instance; false if there is none.
bool callRunning(const QString &method, const QVariantList &args)
{
    auto msg = QDBusMessage::createMethodCall(Service, Path, Interface, method);
    msg.setArguments(args);
    return QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000).type() == QDBusMessage::ReplyMessage;
}

} // namespace

int main(int argc, char **argv)
{
    // Previews decode video in software. Probing for hardware decoders (CUDA,
    // VA-API) loads the whole GPU driver stack, about 140 MB that a resident
    // process would otherwise keep for good. Users can still override this.
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", ",");
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("lumen"));
    app.setApplicationDisplayName(QStringLiteral("Lumen"));
    app.setDesktopFileName(QStringLiteral("io.github.lumen"));
    app.setQuitOnLastWindowClosed(false);
    // KDE jobs (KRunner, KIO) hold Qt's quit lock; without this the
    // background process would exit when one finishes while hidden.
    app.setQuitLockEnabled(false);
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Keyboard launcher for KDE Plasma"));
    parser.addHelpOption();
    const QCommandLineOption toggle(QStringLiteral("toggle"), QStringLiteral("Show or hide the launcher."));
    const QCommandLineOption show(QStringLiteral("show"), QStringLiteral("Show the launcher with TEXT typed."), QStringLiteral("text"));
    const QCommandLineOption settings(QStringLiteral("settings"), QStringLiteral("Open the settings window."));
    QStringList layoutIds;
    for (const WindowTiling::Layout &l : WindowTiling::layouts())
        layoutIds << l.id;
    const QCommandLineOption tile(QStringLiteral("tile"), QStringLiteral("Move the active window: %1.").arg(layoutIds.join(QStringLiteral(", "))),
                                  QStringLiteral("layout"));
    const QCommandLineOption view(QStringLiteral("view"), QStringLiteral("Show a view: clipboard, files, snippets, emoji, windows, or notes."), QStringLiteral("name"));
    parser.addOptions({toggle, show, settings, tile, view});
    parser.addPositionalArgument(QStringLiteral("text"), QStringLiteral("With --view: text to search for."), QStringLiteral("[text]"));
    parser.process(app);

    if (parser.isSet(tile)) {
        const QString id = parser.value(tile);
        if (!layoutIds.contains(id)) {
            qWarning("lumen: unknown layout %s", qPrintable(id));
            return 2;
        }
        return WindowTiling::apply(id) ? 0 : 1;
    }

    // Pass requests on to an instance that is already running.
    QString method;
    QVariantList args;
    if (parser.isSet(toggle))
        method = QStringLiteral("toggle");
    else if (parser.isSet(show))
        method = QStringLiteral("show"), args = {parser.value(show)};
    else if (parser.isSet(settings))
        method = QStringLiteral("settings");
    else if (parser.isSet(view))
        method = QStringLiteral("view"), args = {parser.value(view), parser.positionalArguments().join(u' ')};
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(Service)) {
        if (!method.isEmpty() && !callRunning(method, args))
            return 1;
        return 0;
    }

    Launcher launcher;
    SettingsDialog::applySavedSettings(launcher.clipboardHistory());
    bus.registerObject(Path, new LauncherAdaptor(&launcher), QDBusConnection::ExportScriptableSlots);
    if (parser.isSet(show))
        launcher.showWithQuery(parser.value(show));
    else if (parser.isSet(toggle))
        launcher.toggle();
    else if (parser.isSet(settings))
        launcher.openSettings(Launcher::GeneralTab, false);
    else if (parser.isSet(view))
        launcher.showView(parser.value(view), parser.positionalArguments().join(u' '));
    const int code = app.exec();
    // KRunner plugins run in threads that can outlive the application object
    // and crash during a normal teardown. Everything Lumen keeps is saved as
    // soon as it changes, so leave without one.
    std::_Exit(code);
}

#include "main.moc"
