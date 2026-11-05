#include "timeparse.h"

#include <QRegularExpression>
#include <QTimeZone>

namespace TimeParse
{

std::optional<int> duration(const QString &raw)
{
    const QString text = raw.simplified().toLower();
    if (text.isEmpty())
        return std::nullopt;
    // A sequence of number + unit pairs; a bare number after hours means minutes ("1h 30").
    static const QRegularExpression part(
        QStringLiteral("(\\d+(?:\\.\\d+)?)\\s*(h|hr|hrs|hour|hours|m|min|mins|minute|minutes|s|sec|secs|second|seconds)?(?=\\s|$|\\d)"));
    double seconds = 0;
    qsizetype pos = 0;
    QString lastUnit;
    auto it = part.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        if (m.capturedStart() != pos && !text.mid(pos, m.capturedStart() - pos).trimmed().isEmpty())
            return std::nullopt; // something else between the parts
        pos = m.capturedEnd();
        const double n = m.captured(1).toDouble();
        QString unit = m.captured(2);
        if (unit.isEmpty())
            unit = lastUnit.startsWith(u'h') ? QStringLiteral("m") : lastUnit.startsWith(u'm') ? QStringLiteral("s") : QString();
        if (unit.isEmpty())
            return std::nullopt; // "10" alone is not a duration
        seconds += n * (unit.startsWith(u'h') ? 3600 : unit.startsWith(u'm') ? 60 : 1);
        lastUnit = unit;
    }
    if (pos != text.size() || seconds < 1 || seconds > 7 * 24 * 3600)
        return std::nullopt;
    return int(seconds + 0.5);
}

std::optional<QDateTime> moment(const QString &raw, const QDateTime &now)
{
    QString text = raw.simplified().toLower();
    bool tomorrow = false;
    if (text.startsWith(QLatin1String("tomorrow"))) {
        tomorrow = true;
        text = text.mid(8).trimmed();
    }
    if (text.startsWith(QLatin1String("at ")))
        text = text.mid(3);
    if (text.isEmpty())
        text = QStringLiteral("9am"); // "tomorrow" alone: the start of a working day
    static const QRegularExpression re(QStringLiteral("^(\\d{1,2})(?::(\\d{2}))?\\s*(am|pm)?$"));
    const auto m = re.match(text);
    if (!m.hasMatch())
        return std::nullopt;
    int hour = m.captured(1).toInt();
    const int minute = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    if (!m.captured(3).isEmpty()) {
        if (hour < 1 || hour > 12)
            return std::nullopt;
        hour = hour % 12 + (m.captured(3) == QLatin1String("pm") ? 12 : 0);
    }
    const QTime time(hour, minute);
    if (!time.isValid())
        return std::nullopt;
    QDateTime when(now.date().addDays(tomorrow ? 1 : 0), time, now.timeZone());
    if (!tomorrow && when <= now)
        when = when.addDays(1);
    return when;
}

std::optional<Timer> timer(const QString &query)
{
    QStringList words = query.simplified().toLower().split(u' ', Qt::SkipEmptyParts);
    if (words.size() < 2)
        return std::nullopt;
    if (words.first() == QLatin1String("timer"))
        words.removeFirst(); // "timer 10m tea"
    else if (words.last() == QLatin1String("timer"))
        words.removeLast(); // "10 min tea timer"
    else
        return std::nullopt;
    if (!words.isEmpty() && words.first() == QLatin1String("for"))
        words.removeFirst();
    // The longest run of leading words that reads as a duration; the rest is the label.
    for (qsizetype n = words.size(); n > 0; --n) {
        if (const auto seconds = duration(words.mid(0, n).join(u' '))) {
            QStringList label = words.mid(n);
            if (!label.isEmpty() && label.first() == QLatin1String("for"))
                label.removeFirst();
            return Timer{*seconds, label.join(u' ')};
        }
    }
    return std::nullopt;
}

std::optional<Reminder> reminder(const QString &query, const QDateTime &now)
{
    const QString q = query.simplified();
    static const QRegularExpression start(QStringLiteral("^remind me\\s+"), QRegularExpression::CaseInsensitiveOption);
    if (!start.match(q).hasMatch())
        return std::nullopt;
    const QString rest = q.mid(start.match(q).capturedEnd());

    // "in 20 min to call mom" / "at 5pm to leave" / "tomorrow at 9 to …"
    static const QRegularExpression whenFirst(QStringLiteral("^(in .+?|at .+?|tomorrow(?: at)? ?.*?) to (.+)$"),
                                              QRegularExpression::CaseInsensitiveOption);
    // "to stretch in 1h" / "to leave at 5pm"
    static const QRegularExpression textFirst(QStringLiteral("^to (.+) ((?:in|at) .+|tomorrow(?: at)? ?.*)$"),
                                              QRegularExpression::CaseInsensitiveOption);
    QString when, text;
    if (const auto m = whenFirst.match(rest); m.hasMatch())
        when = m.captured(1), text = m.captured(2);
    else if (const auto m = textFirst.match(rest); m.hasMatch())
        text = m.captured(1), when = m.captured(2);
    else
        return std::nullopt;

    if (when.startsWith(QLatin1String("in "), Qt::CaseInsensitive)) {
        if (const auto seconds = duration(when.mid(3)))
            return Reminder{now.addSecs(*seconds), text};
        return std::nullopt;
    }
    if (const auto at = moment(when, now))
        return Reminder{*at, text};
    return std::nullopt;
}

QString clock(int seconds)
{
    seconds = qMax(0, seconds);
    const int h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace TimeParse
