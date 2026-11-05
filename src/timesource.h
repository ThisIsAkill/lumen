#pragma once

#include "source.h"

class Timekeeper;

// Timers, Pomodoro and reminders in the main search: "timer 10m tea",
// "pomodoro", "remind me in 20 min to call mom"; "timers" and "reminders"
// list what is running, with stop/pause/delete actions.
class TimeSource : public Source
{
    Q_OBJECT
public:
    TimeSource(Launcher *launcher, Timekeeper *keeper);

    void query(const QString &text) override;

    // Running timers, for the start screen.
    QList<Item> startItems() const;

private:
    QList<Item> timerItems(const QString &filter) const;
    QList<Item> reminderItems(const QString &filter) const;

    Timekeeper *m_keeper;
    QString m_query;
};
