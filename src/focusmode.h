#pragma once

#include "source.h"

#include <QDateTime>
#include <QTimer>

class Timekeeper;

// Focus sessions: for a set time, distracting apps and websites are hidden
// whenever they come up (a KWin script minimizes their windows; websites are
// recognized by name in browser window titles), notifications are held back
// (Do Not Disturb) and the countdown shows in the timer badge. Settings →
// Focus lists the apps and sites. A session survives Lumen restarting.
class FocusMode : public Source
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.lumen.Focus")
public:
    FocusMode(Launcher *launcher, Timekeeper *timekeeper);
    ~FocusMode() override;

    void query(const QString &text) override;
    QList<Item> startItems() const;

    bool isActive() const { return m_until.isValid(); }
    QDateTime until() const { return m_until; }
    QString goal() const { return m_goal; }
    void start(int minutes, const QString &goal);
    void extend(int minutes);
    void stop(bool finished);

    // Settings (lumenrc [Focus]).
    static QStringList blockedApps();    // desktop file names
    static QStringList blockedSites();   // "youtube.com", …
    static int defaultMinutes();
    static bool doNotDisturb();
    // The KWin script that keeps them away (tested without KWin).
    static QString script(const QStringList &apps, const QStringList &sites);

public Q_SLOTS:
    // From the KWin script: a distraction was just hidden.
    Q_SCRIPTABLE void blocked(const QString &what);

private:
    void enforce(); // script, Do Not Disturb, end timer
    void release();
    Item sessionItem() const;

    Timekeeper *m_timekeeper;
    QDateTime m_until;
    QString m_goal;
    int m_timerId = 0;     // the countdown in the Timekeeper
    uint m_inhibit = 0;    // Do Not Disturb cookie
    QTimer m_end;
    QDateTime m_lastNotice; // "… is blocked" at most every few seconds
};
