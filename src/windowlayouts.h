#pragma once

#include "source.h"

#include <QJsonArray>

class Library;

// Saved window layouts and the window commands that depend on the session:
//  - "Save Window Layout": the windows on the current desktop (app, title,
//    place) under a name, in the library;
//  - "Coding" (a saved layout): puts those apps' windows back in place,
//    starting the ones that are not running and placing them once they
//    appear;
//  - "Move Window to Desktop N", one per virtual desktop.
// KWin scripts report back over D-Bus (/Tiling): captured windows, apps a
// layout is missing, and window geometry to remember for "Restore Previous
// Size" (see WindowTiling::remember).
class WindowLayoutSource : public Source
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.lumen.Tiling")
public:
    WindowLayoutSource(Launcher *launcher, Library *library);

    void query(const QString &text) override;
    void sessionStarted() override;

    // The KWin script that restores a saved layout (tested without KWin).
    static QString restoreScript(const QJsonArray &windows, bool reportMissing);
    static QString captureScript(const QString &name);

public Q_SLOTS:
    Q_SCRIPTABLE void remember(const QString &windowId, const QString &geometryJson);
    // The windows of the layout being saved, as JSON.
    Q_SCRIPTABLE void captured(const QString &name, const QString &windowsJson);
    // Apps (desktop file names) a layout wants that have no window.
    Q_SCRIPTABLE void missing(const QString &appsJson);

private:
    void save(const QString &name);
    void restore(const QString &name);

    Library *m_library;
    QJsonArray m_restoring; // the layout being restored, to place apps started for it
    int m_desktops = 0; // virtual desktops, read when Lumen opens
};
