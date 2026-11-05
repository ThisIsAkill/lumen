#include "windowlayouts.h"
#include "plural.h"
#include "fuzzy.h"
#include "launcher.h"
#include "library.h"
#include "windowtiling.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

#include <algorithm>

namespace
{

// Placing apps that were started for a layout, once their windows are up.
constexpr int PlaceAgainSoon = 2500;  // ms
constexpr int PlaceAgainLater = 7000; // ms, for slow starters

} // namespace

WindowLayoutSource::WindowLayoutSource(Launcher *launcher, Library *library)
    : Source(launcher)
    , m_library(library)
{
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Tiling"), this, QDBusConnection::ExportScriptableSlots);
}

QString WindowLayoutSource::captureScript(const QString &name)
{
    // windowList() is in stacking order, bottom first: restoring in this
    // order stacks them the same way again.
    return QStringLiteral(R"js(
const out = [];
const here = workspace.currentDesktop;
for (const w of workspace.windowList()) {
    if (!w.normalWindow || w.skipTaskbar || (!w.desktopFileName && !w.resourceClass))
        continue;
    if (w.resourceClass === "lumen" || w.desktopFileName === "io.github.lumen")
        continue; // Lumen's own messages
    if (!w.onAllDesktops && !w.desktops.some(d => d.id === here.id))
        continue;
    const g = w.frameGeometry;
    const a = workspace.clientArea(KWin.MaximizeArea, w);
    out.push({app: w.desktopFileName, cls: w.resourceClass, caption: w.caption,
              x: g.x, y: g.y, width: g.width, height: g.height,
              maximized: g.x === a.x && g.y === a.y && g.width === a.width && g.height === a.height,
              fullScreen: w.fullScreen, minimized: w.minimized});
}
callDBus("io.github.lumen", "/Tiling", "io.github.lumen.Tiling", "captured", %1, JSON.stringify(out));
)js")
        .arg(WindowTiling::jsString(name));
}

QString WindowLayoutSource::restoreScript(const QJsonArray &windows, bool reportMissing)
{
    // Each saved window takes an open window of the same app, the one with
    // the same title if there is one, and goes back to its place on the
    // current desktop.
    return QStringLiteral(R"js(
const saved = %1;
const open = workspace.windowList().filter(w => w.normalWindow && !w.skipTaskbar);
const used = [];
const missing = [];
for (const s of saved) {
    const same = open.filter(w => !used.includes(w) && (s.app ? w.desktopFileName === s.app : w.resourceClass === s.cls));
    const w = same.find(x => x.caption === s.caption) || same[0];
    if (!w) {
        if (s.app && !missing.includes(s.app))
            missing.push(s.app);
        continue;
    }
    used.push(w);
    if (!w.onAllDesktops)
        w.desktops = [workspace.currentDesktop];
    if (s.minimized) {
        w.minimized = true;
        continue;
    }
    w.minimized = false;
    w.fullScreen = false;
    w.setMaximize(false, false);
    w.frameGeometry = {x: s.x, y: s.y, width: s.width, height: s.height};
    if (s.maximized)
        w.setMaximize(true, true);
    if (s.fullScreen)
        w.fullScreen = true;
    workspace.activeWindow = w;
}
if (%2 && missing.length)
    callDBus("io.github.lumen", "/Tiling", "io.github.lumen.Tiling", "missing", JSON.stringify(missing));
)js")
        .arg(QString::fromUtf8(QJsonDocument(windows).toJson(QJsonDocument::Compact)), reportMissing ? QStringLiteral("true") : QStringLiteral("false"));
}

void WindowLayoutSource::remember(const QString &windowId, const QString &geometryJson)
{
    WindowTiling::remember(windowId, geometryJson);
}

void WindowLayoutSource::captured(const QString &name, const QString &windowsJson)
{
    const QJsonArray windows = QJsonDocument::fromJson(windowsJson.toUtf8()).array();
    if (windows.isEmpty()) {
        launcher()->notify(tr("No windows to save on this desktop"));
        return;
    }
    QList<Library::WindowArrangement> arrangements = m_library->arrangements();
    arrangements.removeIf([&](const Library::WindowArrangement &a) { return a.name.compare(name, Qt::CaseInsensitive) == 0; });
    arrangements << Library::WindowArrangement{name, windows};
    m_library->setArrangements(arrangements);
    launcher()->notify(plural(windows.size(), tr("Saved “%2”: %1 window"), tr("Saved “%2”: %1 windows")).arg(name));
}

void WindowLayoutSource::missing(const QString &appsJson)
{
    const QJsonArray apps = QJsonDocument::fromJson(appsJson.toUtf8()).array();
    for (const QJsonValue &app : apps)
        launcher()->launchApp(app.toString() + QStringLiteral(".desktop"));
    if (apps.isEmpty() || m_restoring.isEmpty())
        return;
    // Place them once they have opened; the script leaves the others alone.
    const QJsonArray windows = m_restoring;
    for (int delay : {PlaceAgainSoon, PlaceAgainLater})
        QTimer::singleShot(delay, this, [windows] { WindowTiling::runScript(QStringLiteral("window-layout"), restoreScript(windows, false)); });
}

void WindowLayoutSource::save(const QString &name)
{
    if (!WindowTiling::runScript(QStringLiteral("window-layout-save"), captureScript(name)))
        launcher()->notify(tr("Window layouts need KWin"));
}

void WindowLayoutSource::restore(const QString &name)
{
    const auto arrangements = m_library->arrangements();
    const auto it = std::find_if(arrangements.cbegin(), arrangements.cend(), [&](const Library::WindowArrangement &a) { return a.name == name; });
    if (it == arrangements.cend())
        return;
    m_restoring = it->windows;
    if (!WindowTiling::runScript(QStringLiteral("window-layout"), restoreScript(it->windows, true)))
        launcher()->notify(tr("Window layouts need KWin"));
}

void WindowLayoutSource::sessionStarted()
{
    QDBusInterface desktops(QStringLiteral("org.kde.KWin"), QStringLiteral("/VirtualDesktopManager"), QStringLiteral("org.kde.KWin.VirtualDesktopManager"));
    m_desktops = desktops.isValid() ? desktops.property("count").toInt() : 0;
}

void WindowLayoutSource::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    if (q.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    Launcher *l = launcher();
    auto self = this;

    for (const Library::WindowArrangement &a : m_library->arrangements()) {
        const qreal score = std::max(fuzzyScore(q, a.name), fuzzyScore(q, a.name + QStringLiteral(" layout")) * 0.9);
        if (score <= 0)
            continue;
        QStringList apps;
        for (const QJsonValue &w : a.windows) {
            const QString app = w.toObject().value(QStringLiteral("cls")).toString();
            if (!app.isEmpty() && !apps.contains(app, Qt::CaseInsensitive))
                apps << app;
        }
        Item item;
        item.id = QStringLiteral("window-layout:") + a.name;
        item.title = a.name;
        item.subtitle = plural(a.windows.size(), tr("%1 window · %2"), tr("%1 windows · %2")).arg(apps.join(QStringLiteral(", ")));
        item.kind = tr("Window Layout");
        item.icon = QIcon::fromTheme(QStringLiteral("view-grid"));
        item.score = score;
        const QString name = a.name;
        item.actions << Action{tr("Restore Layout"), {}, {}, [l, self, name] { l->runAfterHide([self, name] { self->restore(name); }); }};
        item.actions << Action{tr("Update with Current Windows"), QStringLiteral("view-refresh"), QKeySequence(Qt::CTRL | Qt::Key_U),
                               [self, name] { self->save(name); }, true};
        item.actions << Action{tr("Delete Layout"), QStringLiteral("edit-delete"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), [self, name] {
            QList<Library::WindowArrangement> arrangements = self->m_library->arrangements();
            arrangements.removeIf([&](const Library::WindowArrangement &x) { return x.name == name; });
            self->m_library->setArrangements(arrangements);
        }, true};
        items << item;
    }

    const qreal saveScore = std::max({fuzzyScore(q, QStringLiteral("Save Window Layout")), fuzzyScore(q, QStringLiteral("window layout")),
                                      fuzzyScore(q, QStringLiteral("arrangement"))});
    if (saveScore >= 40) {
        Item item;
        item.id = QStringLiteral("lumen:save-window-layout");
        item.title = tr("Save Window Layout");
        item.subtitle = tr("Remember where this desktop's windows are, by name");
        item.kind = tr("Window");
        item.icon = QIcon::fromTheme(QStringLiteral("document-save"));
        item.score = saveScore;
        const Item shown = item;
        item.actions << Action{tr("Name It"), {}, {}, [l, self, shown] {
            l->ask(tr("Layout"), tr("Name this layout (e.g. Coding), then press Enter"), shown, false, [self](const QString &typed) {
                if (!typed.trimmed().isEmpty())
                    self->save(typed.trimmed());
            });
        }, true};
        items << item;
    }

    for (int n = 1; n <= m_desktops; ++n) {
        const QString title = tr("Move Window to Desktop %1").arg(n);
        const qreal score = std::max(fuzzyScore(q, title), fuzzyScore(q, QStringLiteral("desktop %1").arg(n)));
        if (score < 40)
            continue;
        Item item;
        item.id = QStringLiteral("lumen:window-desktop-%1").arg(n);
        item.title = title;
        item.subtitle = tr("Window Management");
        item.kind = tr("Window");
        item.icon = QIcon::fromTheme(QStringLiteral("virtual-desktops"));
        item.score = score;
        item.actions << Action{tr("Move"), {}, {}, [l, n] {
            l->runAfterHide([n] {
                WindowTiling::runScript(QStringLiteral("window-desktop"), QStringLiteral(
                    "const w = workspace.activeWindow, d = workspace.desktops[%1];\n"
                    "if (w && w.normalWindow && d) { w.desktops = [d]; workspace.currentDesktop = d; workspace.activeWindow = w; }\n").arg(n - 1));
            });
        }};
        items << item;
    }
    Q_EMIT resultsReady(items);
}
