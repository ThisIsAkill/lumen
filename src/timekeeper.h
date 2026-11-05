#pragma once

#include <QDateTime>
#include <QObject>
#include <QTimer>

class TimerBadge;

// Running timers, Pomodoro and reminders. They are saved, so they survive a
// restart (a reminder that came due meanwhile is shown at start-up). When one
// ends, a notification stays on screen until dismissed. While timers run, a
// small badge in the top-right corner counts down the next one.
class Timekeeper : public QObject
{
    Q_OBJECT
public:
    struct Timer {
        int id = 0;
        QString label;
        QDateTime end;       // when it rings (if running)
        int total = 0;       // seconds
        int pausedLeft = -1; // seconds left while paused, else -1
        bool pomodoro = false;
        bool onBreak = false;
        int round = 1;       // Pomodoro focus round
        int secondsLeft() const;
    };
    struct Reminder {
        int id = 0;
        QDateTime when;
        QString text;
    };

    explicit Timekeeper(QObject *parent = nullptr);
    ~Timekeeper() override;

    QList<Timer> timers() const { return m_timers; }
    QList<Reminder> reminders() const { return m_reminders; }

    void startTimer(int seconds, const QString &label);
    void startPomodoro();
    void stopTimer(int id);
    void setPaused(int id, bool paused);
    void addTime(int id, int seconds);
    void addReminder(const QDateTime &when, const QString &text);
    void removeReminder(int id);

    static QString file();

Q_SIGNALS:
    void changed();
    void ticked(); // every second while timers run

private:
    void tick();
    void ring(const QString &title, const QString &body);
    void scheduleReminders();
    void load();
    void save() const;
    void update();

    QList<Timer> m_timers;
    QList<Reminder> m_reminders;
    int m_nextId = 1;
    QTimer m_tick;          // every second while timers run
    QTimer m_reminderTimer; // the next reminder
    TimerBadge *m_badge;
};
