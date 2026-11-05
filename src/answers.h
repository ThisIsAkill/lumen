#pragma once

#include <QColor>
#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

// Instant answers computed from the query text: time zones, date maths and
// colours. Pure functions, so they are easy to test; AnswerSource turns
// them into results.
namespace Answers
{

struct Answer {
    QString title;    // the answer itself, e.g. "07:12 · Thursday, Nov 5"
    QString subtitle; // what was computed, e.g. "Tokyo, 15 hours ahead"
    QString copyText; // what Enter copies
    QColor colour;    // set for colour answers (drawn as a swatch)
    QList<std::pair<QString, QString>> alternatives; // extra copy actions: label, text
};

// "time in tokyo", "tokyo time", "3pm tokyo in edmonton", "15:30 utc to pst",
// "9am in india". `now` is injectable for tests.
std::optional<Answer> timeZone(const QString &query, const QDateTime &now = QDateTime::currentDateTime());

// "days until dec 25", "days since 2026-01-01", "in 3 weeks", "100 days ago",
// "45 days from now", "next friday", "week number".
std::optional<Answer> dates(const QString &query, const QDate &today = QDate::currentDate());

// "#ff8800", "#f80", "rgb(255, 136, 0)", "hsl(32, 100%, 50%)".
std::optional<Answer> colour(const QString &query);

// Percentages in words: "15% of 80", "80 + 15%", "80 - 15%", "15% off 80",
// "12 is what % of 80", "what % of 80 is 12", "80 to 92 in %" (the change).
std::optional<Answer> percent(const QString &query);

// The IANA zone for a city, country or abbreviation ("tokyo", "india",
// "pst"), or an invalid zone.
QTimeZone findZone(const QString &name);

} // namespace Answers
