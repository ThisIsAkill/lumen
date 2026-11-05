#pragma once

#include "source.h"

// Quitting apps the way macOS's Force Quit window does: running apps by
// name and icon (from their windows, which KWin lists), not processes.
//  - "quit firefox" / "force quit firefox" in the main search;
//  - "Quit All Apps";
//  - the "Running Apps" view: every app with its windows, Enter quits it
//    (its windows are asked to close, so it can save), Ctrl+Delete forces it.
class AppQuitSource : public Source
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.lumen.Apps")
public:
    AppQuitSource(Launcher *launcher, bool view);

    void query(const QString &text) override;
    void sessionStarted() override;
    QString viewTitle() const override { return tr("Running Apps"); }
    QString viewPlaceholder() const override { return tr("Search running apps…"); }

    struct App {
        QString id;    // the desktop file name, or the window class
        QString name;
        QString icon;
        QList<qint64> pids;
        QStringList captions;
    };
    // The apps behind a window list from KWin (tested without KWin).
    static QList<App> appsFrom(const QString &windowsJson);

public Q_SLOTS:
    // From the KWin script: the windows, as JSON.
    Q_SCRIPTABLE void report(const QString &windowsJson);

private:
    void request();
    void publish();
    void quit(const App &app);
    void forceQuit(const App &app);
    Item itemFor(const App &app, qreal score) const;

    bool m_view;
    QList<App> m_apps;
    QString m_query;
};
