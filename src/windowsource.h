#pragma once

#include "source.h"

// The "Switch Window" view: every open window, with a thumbnail of the
// selected one in the preview pane. KWin reports the windows through a short
// script that calls back over D-Bus (report()); thumbnails come from KWin's
// ScreenShot2 interface, which KWin only offers to programs whose desktop
// entry asks for it (install.sh sets that up).
class WindowSource : public Source
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.lumen.Windows")
public:
    explicit WindowSource(Launcher *launcher);

    void query(const QString &text) override;
    void sessionStarted() override;
    QString viewTitle() const override { return tr("Windows"); }
    QString viewPlaceholder() const override { return tr("Search open windows…"); }
    bool hasPreviewPane() const override { return true; }

public Q_SLOTS:
    // Called by the KWin script with a JSON list of windows.
    Q_SCRIPTABLE void report(const QString &json);

private:
    struct Window {
        QString id; // KWin's internal id, "{uuid}"
        QString caption;
        QString app;
        QString icon;
        bool minimized = false;
        bool active = false;
        QString thumbnail; // PNG file, once captured
    };

    void requestWindows();
    void captureThumbnails();
    void windowScript(const QString &id, const QString &body);
    void publish();

    QList<Window> m_windows;
    QString m_query;
    bool m_open = false;
};
