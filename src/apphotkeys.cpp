#include "apphotkeys.h"
#include "windowtiling.h"

#include <KConfigGroup>
#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#include <KIO/ApplicationLauncherJob>
#include <KSharedConfig>

#include <QAction>

namespace
{

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("AppHotkeys"));
}

} // namespace

AppHotkeys::AppHotkeys(QObject *parent)
    : QObject(parent)
{
    const KConfigGroup group = config();
    for (const QString &storageId : group.keyList()) {
        const KService::Ptr app = KService::serviceByStorageId(storageId);
        const QKeySequence keys(group.readEntry(storageId), QKeySequence::PortableText);
        if (app && !keys.isEmpty())
            KGlobalAccel::self()->setShortcut(actionFor(app), {keys}, KGlobalAccel::NoAutoloading);
    }
}

QAction *AppHotkeys::actionFor(const KService::Ptr &app)
{
    QAction *&action = m_actions[app->storageId()];
    if (!action) {
        action = new QAction(tr("Open %1").arg(app->name()), this);
        action->setObjectName(QStringLiteral("app-") + app->storageId());
        const QString storageId = app->storageId();
        connect(action, &QAction::triggered, this, [storageId] { activate(storageId); });
    }
    return action;
}

bool AppHotkeys::set(const KService::Ptr &app, const QKeySequence &keys, QString *conflict)
{
    const QList<KGlobalShortcutInfo> taken = KGlobalAccel::globalShortcutsByKey(keys);
    for (const KGlobalShortcutInfo &info : taken) {
        if (info.uniqueName() != QStringLiteral("app-") + app->storageId()) {
            if (conflict)
                *conflict = info.friendlyName().isEmpty() ? info.uniqueName() : info.friendlyName();
            return false;
        }
    }
    KGlobalAccel::self()->setShortcut(actionFor(app), {keys}, KGlobalAccel::NoAutoloading);
    KConfigGroup g = config();
    g.writeEntry(app->storageId(), keys.toString(QKeySequence::PortableText));
    g.sync();
    return true;
}

void AppHotkeys::remove(const QString &storageId)
{
    if (QAction *action = m_actions.take(storageId)) {
        KGlobalAccel::self()->removeAllShortcuts(action);
        delete action;
    }
    KConfigGroup g = config();
    g.deleteEntry(storageId);
    g.sync();
}

QKeySequence AppHotkeys::keysFor(const QString &storageId) const
{
    return QKeySequence(config().readEntry(storageId), QKeySequence::PortableText);
}

void AppHotkeys::activate(const QString &storageId)
{
    // KWin knows the windows: bring the app's newest to the front, minimize it
    // if it is already in front, or ask Lumen to start the app.
    const QString id = storageId.endsWith(QLatin1String(".desktop")) ? storageId.chopped(8) : storageId;
    const QString script = QStringLiteral(R"js(
const id = %1;
const active = workspace.activeWindow;
if (active && active.desktopFileName === id) {
    active.minimized = true;
} else {
    let found = null;
    for (const w of workspace.windowList())
        if (w.desktopFileName === id && w.normalWindow)
            found = w; // the last is the topmost
    if (found) {
        found.minimized = false;
        workspace.activeWindow = found;
    } else {
        callDBus("io.github.lumen", "/Launcher", "io.github.lumen.Launcher", "launchApp", %2);
    }
}
)js")
                               .arg(WindowTiling::jsString(id), WindowTiling::jsString(storageId));
    if (!WindowTiling::runScript(QStringLiteral("app-hotkey"), script)) {
        // No KWin: just start it.
        if (const KService::Ptr app = KService::serviceByStorageId(storageId))
            (new KIO::ApplicationLauncherJob(app))->start();
    }
}
