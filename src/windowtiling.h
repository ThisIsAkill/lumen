#pragma once

#include <QString>
#include <QStringList>

// Moves and resizes the active window through a short KWin script, e.g. to
// the left half or the middle third of its screen's work area.
namespace WindowTiling
{

struct Layout {
    QString id;
    QString name;
    QStringList keywords;
    QString iconName;
};

const QList<Layout> &layouts();

// A JavaScript string literal for text, to put values into scripts.
QString jsString(const QString &text);

// Runs a KWin script (JavaScript, see KWin's scripting API) once. Returns
// false when KWin's scripting interface is not available.
bool runScript(const QString &name, const QString &code);

// Applies the layout to the active window. Returns false when KWin's
// scripting interface is not available (another window manager).
bool apply(const QString &layoutId);
QString scriptFor(const QString &layoutId); // the script apply() runs

// Keeps a window's geometry (JSON {x, y, width, height}) from before Lumen
// moved it, for "restore" (in a file in the runtime folder, so the
// command-line `lumen --tile restore` finds it too).
void remember(const QString &windowId, const QString &geometryJson);

// Loads a KWin script that keeps running (it reacts to events) until
// unloadScript() with the same name.
bool loadScript(const QString &name, const QString &code);
void unloadScript(const QString &name);

} // namespace WindowTiling
