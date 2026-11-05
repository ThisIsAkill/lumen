#pragma once

#include "source.h"

#include <QDateTime>
#include <QTimer>

// "Keep Awake" (Raycast's Coffee): the computer neither sleeps nor locks
// its screen for a while, or until stopped. It holds the same inhibitions a
// video player does (the freedesktop power and screen saver interfaces,
// which Plasma honours), shows on the start screen while on, and carries on
// if Lumen restarts.
class KeepAwake : public Source
{
    Q_OBJECT
public:
    explicit KeepAwake(Launcher *launcher);
    ~KeepAwake() override;

    void query(const QString &text) override;
    QList<Item> startItems() const;

    bool isOn() const { return m_on; }
    QDateTime until() const { return m_until; } // invalid: until stopped
    void start(int minutes); // 0: until stopped
    void stop();

private:
    void hold();
    void release();
    Item statusItem() const;

    bool m_on = false;
    QDateTime m_until;
    uint m_sleepCookie = 0;
    uint m_screenCookie = 0;
    QTimer m_end;
};
