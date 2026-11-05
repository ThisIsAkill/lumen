#include "windowtiling.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QHash>
#include <QObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace
{

struct Geometry {
    qreal x, y, w, h; // fractions of the work area
};

// Layouts that fill a fixed part of the work area. "maximize" and "center"
// are handled separately in the script.
const QHash<QString, Geometry> &geometries()
{
    static const QHash<QString, Geometry> table{
        {QStringLiteral("left-half"), {0, 0, 0.5, 1}},
        {QStringLiteral("right-half"), {0.5, 0, 0.5, 1}},
        {QStringLiteral("top-half"), {0, 0, 1, 0.5}},
        {QStringLiteral("bottom-half"), {0, 0.5, 1, 0.5}},
        {QStringLiteral("top-left"), {0, 0, 0.5, 0.5}},
        {QStringLiteral("top-right"), {0.5, 0, 0.5, 0.5}},
        {QStringLiteral("bottom-left"), {0, 0.5, 0.5, 0.5}},
        {QStringLiteral("bottom-right"), {0.5, 0.5, 0.5, 0.5}},
        {QStringLiteral("first-third"), {0, 0, 1.0 / 3, 1}},
        {QStringLiteral("center-third"), {1.0 / 3, 0, 1.0 / 3, 1}},
        {QStringLiteral("last-third"), {2.0 / 3, 0, 1.0 / 3, 1}},
        {QStringLiteral("first-two-thirds"), {0, 0, 2.0 / 3, 1}},
        {QStringLiteral("last-two-thirds"), {1.0 / 3, 0, 2.0 / 3, 1}},
        {QStringLiteral("almost-maximize"), {0.05, 0.05, 0.9, 0.9}},
    };
    return table;
}

// Commands that are not a fixed part of the work area. `w` is the active
// window, `a` its screen's work area.
const QHash<QString, QString> &specialScripts()
{
    static const QHash<QString, QString> table{
        // Keeps the window's place on the screen, scaled to the other one.
        {QStringLiteral("next-screen"), QStringLiteral("const outs = workspace.screens;\n"
                                                       "workspace.sendClientToScreen(w, outs[(outs.indexOf(w.output) + 1) % outs.length]);")},
        {QStringLiteral("previous-screen"), QStringLiteral("const outs = workspace.screens;\n"
                                                           "workspace.sendClientToScreen(w, outs[(outs.indexOf(w.output) + outs.length - 1) % outs.length]);")},
        // About 60 % of the width (at least 1000 px) and 75 % of the height, centred.
        {QStringLiteral("reasonable-size"), QStringLiteral("w.setMaximize(false, false);\n"
                                                           "const width = Math.min(a.width, Math.max(Math.round(a.width * 0.6), 1000));\n"
                                                           "const height = Math.min(a.height, Math.round(a.height * 0.75));\n"
                                                           "w.frameGeometry = {x: Math.round(a.x + (a.width - width) / 2), y: Math.round(a.y + (a.height - height) / 2),"
                                                           " width: width, height: height};")},
        // Grows or shrinks by a tenth of the work area, around its centre, inside the area.
        {QStringLiteral("larger"), QStringLiteral("resizeBy(w, a, 0.1);")},
        {QStringLiteral("smaller"), QStringLiteral("resizeBy(w, a, -0.1);")},
        {QStringLiteral("fullscreen"), QStringLiteral("w.fullScreen = !w.fullScreen;")},
        {QStringLiteral("keep-above"), QStringLiteral("w.keepAbove = !w.keepAbove;")},
        // Moves the window to the neighbouring desktop and goes with it.
        {QStringLiteral("next-desktop"), QStringLiteral("const ds = workspace.desktops;\n"
                                                        "const t = ds[(ds.indexOf(workspace.currentDesktop) + 1) % ds.length];\n"
                                                        "w.desktops = [t]; workspace.currentDesktop = t; workspace.activeWindow = w;")},
        {QStringLiteral("previous-desktop"), QStringLiteral("const ds = workspace.desktops;\n"
                                                            "const t = ds[(ds.indexOf(workspace.currentDesktop) + ds.length - 1) % ds.length];\n"
                                                            "w.desktops = [t]; workspace.currentDesktop = t; workspace.activeWindow = w;")},
    };
    return table;
}

// The helper used by "larger" and "smaller".
const char *ResizeHelper = R"js(
function resizeBy(w, a, step) {
    w.setMaximize(false, false);
    const g = w.frameGeometry;
    const dw = Math.round(a.width * step / 2), dh = Math.round(a.height * step / 2);
    const width = Math.max(200, Math.min(a.width, g.width + 2 * dw)), height = Math.max(150, Math.min(a.height, g.height + 2 * dh));
    const x = Math.min(Math.max(a.x, g.x + (g.width - width) / 2), a.x + a.width - width);
    const y = Math.min(Math.max(a.y, g.y + (g.height - height) / 2), a.y + a.height - height);
    w.frameGeometry = {x: Math.round(x), y: Math.round(y), width: width, height: height};
}
)js";

QString memoryFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/lumen-window-memory.json");
}

QString script(const QString &layoutId)
{
    QString body;
    if (layoutId == QLatin1String("restore")) {
        // The geometry each window had before Lumen last moved it.
        QFile f(memoryFile());
        const QByteArray memory = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("{}");
        body = QStringLiteral("const memory = %1;\n"
                              "const m = memory[w.internalId.toString()];\n"
                              "if (m) { w.setMaximize(false, false); w.fullScreen = false; w.frameGeometry = m; }")
                   .arg(QString::fromUtf8(memory.trimmed().isEmpty() ? QByteArray("{}") : memory));
        return QStringLiteral("const w = workspace.activeWindow;\nif (w && w.normalWindow) {\n%1\n}\n").arg(body);
    }
    if (specialScripts().contains(layoutId)) {
        body = specialScripts().value(layoutId);
    } else if (layoutId == QLatin1String("maximize")) {
        body = QStringLiteral("w.setMaximize(true, true);");
    } else if (layoutId == QLatin1String("center")) {
        body = QStringLiteral(
            "w.setMaximize(false, false);\n"
            "const g = w.frameGeometry;\n"
            "const width = Math.min(g.width, a.width), height = Math.min(g.height, a.height);\n"
            "w.frameGeometry = {x: Math.round(a.x + (a.width - width) / 2), y: Math.round(a.y + (a.height - height) / 2),"
            " width: width, height: height};");
    } else {
        const Geometry g = geometries().value(layoutId);
        body = QStringLiteral(
                   "w.setMaximize(false, false);\n"
                   "w.frameGeometry = {x: Math.round(a.x + a.width * %1), y: Math.round(a.y + a.height * %2),"
                   " width: Math.round(a.width * %3), height: Math.round(a.height * %4)};")
                   .arg(g.x)
                   .arg(g.y)
                   .arg(g.w)
                   .arg(g.h);
    }
    // Full screen, on top, desktops and screens work on any window (also a
    // full-screen one, which cannot be moved or resized); the rest needs a
    // window that can be moved and resized.
    static const QStringList anyWindow{QStringLiteral("fullscreen"), QStringLiteral("keep-above"), QStringLiteral("next-desktop"),
                                       QStringLiteral("previous-desktop"), QStringLiteral("next-screen"), QStringLiteral("previous-screen")};
    // Remember where the window was, for "Restore Previous Size", before
    // anything that changes its size or place on the screen.
    static const QStringList keepsGeometry{QStringLiteral("fullscreen"), QStringLiteral("keep-above"), QStringLiteral("next-desktop"),
                                           QStringLiteral("previous-desktop")};
    const QString remember = keepsGeometry.contains(layoutId)
        ? QString()
        : QStringLiteral("const before = w.frameGeometry;\n"
                         "callDBus(\"io.github.lumen\", \"/Tiling\", \"io.github.lumen.Tiling\", \"remember\", w.internalId.toString(),"
                         " JSON.stringify({x: before.x, y: before.y, width: before.width, height: before.height}));\n");
    return QString::fromLatin1(ResizeHelper)
        + QStringLiteral("const w = workspace.activeWindow;\n"
                         "if (w && w.normalWindow%1) {\n"
                         "const a = workspace.clientArea(KWin.MaximizeArea, w);\n"
                         "%2"
                         "%3\n"
                         "}\n")
              .arg(anyWindow.contains(layoutId) ? QString() : QStringLiteral(" && w.moveable && w.resizeable"), remember, body);
}

QDBusMessage scriptingCall(const QString &method, const QVariantList &args)
{
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
                                              QStringLiteral("org.kde.kwin.Scripting"), method);
    msg.setArguments(args);
    return msg;
}

} // namespace

namespace WindowTiling
{

const QList<Layout> &layouts()
{
    static const QList<Layout> list{
        {QStringLiteral("left-half"), QObject::tr("Left Half"), {QStringLiteral("tile"), QStringLiteral("snap")}, QStringLiteral("view-split-left-right")},
        {QStringLiteral("right-half"), QObject::tr("Right Half"), {QStringLiteral("tile"), QStringLiteral("snap")}, QStringLiteral("view-split-left-right")},
        {QStringLiteral("top-half"), QObject::tr("Top Half"), {QStringLiteral("tile")}, QStringLiteral("view-split-top-bottom")},
        {QStringLiteral("bottom-half"), QObject::tr("Bottom Half"), {QStringLiteral("tile")}, QStringLiteral("view-split-top-bottom")},
        {QStringLiteral("top-left"), QObject::tr("Top Left Quarter"), {QStringLiteral("tile")}, QStringLiteral("view-grid")},
        {QStringLiteral("top-right"), QObject::tr("Top Right Quarter"), {QStringLiteral("tile")}, QStringLiteral("view-grid")},
        {QStringLiteral("bottom-left"), QObject::tr("Bottom Left Quarter"), {QStringLiteral("tile")}, QStringLiteral("view-grid")},
        {QStringLiteral("bottom-right"), QObject::tr("Bottom Right Quarter"), {QStringLiteral("tile")}, QStringLiteral("view-grid")},
        {QStringLiteral("first-third"), QObject::tr("First Third"), {QStringLiteral("tile")}, QStringLiteral("view-split-left-right")},
        {QStringLiteral("center-third"), QObject::tr("Center Third"), {QStringLiteral("tile")}, QStringLiteral("view-split-left-right")},
        {QStringLiteral("last-third"), QObject::tr("Last Third"), {QStringLiteral("tile")}, QStringLiteral("view-split-left-right")},
        {QStringLiteral("first-two-thirds"), QObject::tr("First Two Thirds"), {QStringLiteral("tile")}, QStringLiteral("view-split-left-right")},
        {QStringLiteral("last-two-thirds"), QObject::tr("Last Two Thirds"), {QStringLiteral("tile")}, QStringLiteral("view-split-left-right")},
        {QStringLiteral("maximize"), QObject::tr("Maximize Window"), {QStringLiteral("fullscreen"), QStringLiteral("tile")}, QStringLiteral("window-maximize")},
        {QStringLiteral("almost-maximize"), QObject::tr("Almost Maximize"), {QStringLiteral("tile")}, QStringLiteral("window-maximize")},
        {QStringLiteral("center"), QObject::tr("Center Window"), {QStringLiteral("middle")}, QStringLiteral("zoom-fit-best")},
        {QStringLiteral("reasonable-size"), QObject::tr("Reasonable Size"), {QStringLiteral("resize"), QStringLiteral("center")}, QStringLiteral("zoom-fit-best")},
        {QStringLiteral("larger"), QObject::tr("Make Window Larger"), {QStringLiteral("grow"), QStringLiteral("resize"), QStringLiteral("bigger")}, QStringLiteral("zoom-in")},
        {QStringLiteral("smaller"), QObject::tr("Make Window Smaller"), {QStringLiteral("shrink"), QStringLiteral("resize")}, QStringLiteral("zoom-out")},
        {QStringLiteral("restore"), QObject::tr("Restore Previous Size"), {QStringLiteral("undo"), QStringLiteral("unsnap"), QStringLiteral("back")}, QStringLiteral("edit-undo")},
        {QStringLiteral("next-screen"), QObject::tr("Move to Next Screen"), {QStringLiteral("display"), QStringLiteral("monitor"), QStringLiteral("output")}, QStringLiteral("video-display")},
        {QStringLiteral("previous-screen"), QObject::tr("Move to Previous Screen"), {QStringLiteral("display"), QStringLiteral("monitor"), QStringLiteral("output")}, QStringLiteral("video-display")},
        {QStringLiteral("next-desktop"), QObject::tr("Move to Next Desktop"), {QStringLiteral("workspace"), QStringLiteral("virtual desktop")}, QStringLiteral("go-next")},
        {QStringLiteral("previous-desktop"), QObject::tr("Move to Previous Desktop"), {QStringLiteral("workspace"), QStringLiteral("virtual desktop")}, QStringLiteral("go-previous")},
        {QStringLiteral("fullscreen"), QObject::tr("Toggle Full Screen"), {QStringLiteral("fullscreen")}, QStringLiteral("view-fullscreen")},
        {QStringLiteral("keep-above"), QObject::tr("Toggle Always on Top"), {QStringLiteral("pin"), QStringLiteral("keep above"), QStringLiteral("float")}, QStringLiteral("window-keep-above")},
    };
    return list;
}

QString jsString(const QString &text)
{
    // JSON's string syntax is valid JavaScript: ["…"] without the brackets.
    const QString array = QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact));
    return array.mid(1, array.size() - 2);
}

bool runScript(const QString &name, const QString &code)
{
    const QString file = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/lumen-") + name + QStringLiteral(".js");
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(code.toUtf8());
    if (!out.commit())
        return false;

    const QString plugin = QStringLiteral("lumen-") + name;
    auto bus = QDBusConnection::sessionBus();
    bus.call(scriptingCall(QStringLiteral("unloadScript"), {plugin})); // a leftover from an earlier call
    const QDBusReply<int> id = bus.call(scriptingCall(QStringLiteral("loadScript"), {file, plugin}));
    if (!id.isValid() || id.value() < 0)
        return false;
    bus.call(QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting/Script%1").arg(id.value()),
                                            QStringLiteral("org.kde.kwin.Script"), QStringLiteral("run")));
    bus.call(scriptingCall(QStringLiteral("unloadScript"), {plugin}), QDBus::NoBlock);
    return true;
}

bool apply(const QString &layoutId)
{
    return runScript(QStringLiteral("window-tiling"), script(layoutId));
}

QString scriptFor(const QString &layoutId)
{
    return script(layoutId);
}

void remember(const QString &windowId, const QString &geometryJson)
{
    QFile f(memoryFile());
    QJsonObject memory = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    f.close();
    QJsonObject geometry = QJsonDocument::fromJson(geometryJson.toUtf8()).object();
    geometry.insert(QStringLiteral("t"), QDateTime::currentSecsSinceEpoch()); // for forgetting the oldest
    memory.insert(windowId, geometry);
    // Windows come and go: keep the most recent ones only.
    constexpr int MaxRemembered = 50;
    while (memory.size() > MaxRemembered) {
        auto oldest = memory.begin();
        for (auto it = memory.begin(); it != memory.end(); ++it)
            if (it->toObject().value(QStringLiteral("t")).toInteger() < oldest->toObject().value(QStringLiteral("t")).toInteger())
                oldest = it;
        memory.erase(oldest);
    }
    QSaveFile out(memoryFile());
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(memory).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

bool loadScript(const QString &name, const QString &code)
{
    const QString file = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/lumen-") + name + QStringLiteral(".js");
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(code.toUtf8());
    if (!out.commit())
        return false;
    const QString plugin = QStringLiteral("lumen-") + name;
    auto bus = QDBusConnection::sessionBus();
    bus.call(scriptingCall(QStringLiteral("unloadScript"), {plugin}));
    const QDBusReply<int> id = bus.call(scriptingCall(QStringLiteral("loadScript"), {file, plugin}));
    if (!id.isValid() || id.value() < 0)
        return false;
    bus.call(QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting/Script%1").arg(id.value()),
                                            QStringLiteral("org.kde.kwin.Script"), QStringLiteral("run")));
    return true;
}

void unloadScript(const QString &name)
{
    QDBusConnection::sessionBus().call(scriptingCall(QStringLiteral("unloadScript"), {QStringLiteral("lumen-") + name}));
}

} // namespace WindowTiling
