#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

// Reading calendar feeds (iCalendar, .ics): the events, with repeating ones
// expanded into their occurrences within a time window. Handles time zones
// (IANA and Windows names), all-day events, RRULE (daily, weekly with BYDAY,
// monthly, yearly; INTERVAL, COUNT, UNTIL), EXDATE, moved occurrences
// (RECURRENCE-ID) and cancelled events.
namespace Ics
{

struct Event {
    QString title;
    QString location;
    QString description;
    QString url;
    QDateTime start;
    QDateTime end;
    bool allDay = false;
    QString meetingLink() const; // a Meet/Zoom/Teams/Jitsi/Webex link, if any
};

// Occurrences that overlap [from, to), sorted by start.
QList<Event> parse(const QByteArray &ics, const QDateTime &from, const QDateTime &to);

} // namespace Ics
