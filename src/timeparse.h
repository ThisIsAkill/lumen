#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

// Reading timers and reminders out of what the user typed. Pure functions,
// with `now` passed in so they can be tested.
namespace TimeParse
{

// "10m", "10 min", "1h30m", "1h 30", "90s", "1.5 hours", "2 hours 5 minutes".
// Returns seconds, or nullopt when the text is not a duration.
std::optional<int> duration(const QString &text);

// "5pm", "17:30", "9", "tomorrow at 9am", "tomorrow 8:15": the next such
// moment after now (today if still ahead, else tomorrow).
std::optional<QDateTime> moment(const QString &text, const QDateTime &now);

struct Timer {
    int seconds = 0;
    QString label; // "tea" in "timer 5m tea"
};
// "timer 10m", "timer 1h30m tea", "10 min timer", "5m tea timer".
std::optional<Timer> timer(const QString &query);

struct Reminder {
    QDateTime when;
    QString text;
};
// "remind me in 20 min to call mom", "remind me at 5pm to leave",
// "remind me tomorrow at 9 to send the report", "remind me to stretch in 1h".
std::optional<Reminder> reminder(const QString &query, const QDateTime &now);

// "4:59", "1:02:03": a countdown display.
QString clock(int seconds);

} // namespace TimeParse
