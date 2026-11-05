#include "ics.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

namespace Ics
{

namespace
{

constexpr int MaxOccurrences = 2000; // per repeating event, a guard against endless rules

struct Property {
    QString name;
    QHash<QString, QString> params;
    QString value;
};

// Lines folded with a leading space or tab belong to the line before.
QStringList unfold(const QByteArray &ics)
{
    QStringList lines;
    for (QString line : QString::fromUtf8(ics).split(u'\n')) {
        if (line.endsWith(u'\r'))
            line.chop(1);
        if ((line.startsWith(u' ') || line.startsWith(u'\t')) && !lines.isEmpty())
            lines.last() += line.mid(1);
        else
            lines << line;
    }
    return lines;
}

Property readProperty(const QString &line)
{
    // NAME;PARAM=VALUE;PARAM="VALUE":value — the first colon outside quotes ends the name part.
    Property p;
    bool quoted = false;
    qsizetype colon = -1;
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (line.at(i) == u'"')
            quoted = !quoted;
        else if (line.at(i) == u':' && !quoted) {
            colon = i;
            break;
        }
    }
    if (colon < 0)
        return p;
    const QStringList head = line.left(colon).split(u';');
    p.name = head.first().toUpper();
    for (const QString &param : head.mid(1)) {
        const qsizetype eq = param.indexOf(u'=');
        if (eq > 0)
            p.params.insert(param.left(eq).toUpper(), param.mid(eq + 1).remove(u'"'));
    }
    p.value = line.mid(colon + 1);
    return p;
}

QString unescape(QString text)
{
    text.replace(QLatin1String("\\n"), QLatin1String("\n")).replace(QLatin1String("\\N"), QLatin1String("\n"));
    text.replace(QLatin1String("\\,"), QLatin1String(",")).replace(QLatin1String("\\;"), QLatin1String(";"));
    text.replace(QLatin1String("\\\\"), QLatin1String("\\"));
    return text;
}

QTimeZone zoneFor(const QString &tzid)
{
    if (tzid.isEmpty())
        return QTimeZone::systemTimeZone(); // "floating" times are local
    QTimeZone zone(tzid.toUtf8());
    if (zone.isValid())
        return zone;
    // Outlook writes Windows names ("Mountain Standard Time").
    const QByteArray iana = QTimeZone::windowsIdToDefaultIanaId(tzid.toUtf8());
    return iana.isEmpty() ? QTimeZone::systemTimeZone() : QTimeZone(iana);
}

// A DATE or DATE-TIME value; allDay is set for plain dates.
QDateTime readTime(const Property &p, bool *allDay = nullptr)
{
    const QString v = p.value.trimmed();
    if (v.size() == 8 || p.params.value(QStringLiteral("VALUE")) == QLatin1String("DATE")) {
        if (allDay)
            *allDay = true;
        return QDateTime(QDate::fromString(v.left(8), QStringLiteral("yyyyMMdd")), QTime(0, 0), QTimeZone::systemTimeZone());
    }
    if (allDay)
        *allDay = false;
    const QDate date = QDate::fromString(v.left(8), QStringLiteral("yyyyMMdd"));
    const QTime time = QTime::fromString(v.mid(9, 6), QStringLiteral("HHmmss"));
    if (v.endsWith(u'Z'))
        return QDateTime(date, time, QTimeZone::UTC);
    return QDateTime(date, time, zoneFor(p.params.value(QStringLiteral("TZID"))));
}

// ISO 8601 durations as used by DURATION: P1D, PT1H30M, P1W.
qint64 durationSeconds(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^([+-])?P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$"));
    const auto m = re.match(text.trimmed());
    if (!m.hasMatch())
        return 0;
    const qint64 s = m.captured(2).toLongLong() * 604800 + m.captured(3).toLongLong() * 86400 + m.captured(4).toLongLong() * 3600
        + m.captured(5).toLongLong() * 60 + m.captured(6).toLongLong();
    return m.captured(1) == QLatin1String("-") ? -s : s;
}

struct RawEvent {
    QString uid;
    Event event;
    QString rrule;
    QList<QDateTime> exdates;
    QDateTime recurrenceId; // set when this moves one occurrence of a repeating event
    bool cancelled = false;
};

// The starts of a repeating event's occurrences up to `to`.
QList<QDateTime> occurrences(const QDateTime &first, const QString &rule, const QDateTime &to)
{
    QHash<QString, QString> parts;
    for (const QString &part : rule.split(u';')) {
        const qsizetype eq = part.indexOf(u'=');
        if (eq > 0)
            parts.insert(part.left(eq).toUpper(), part.mid(eq + 1));
    }
    const QString freq = parts.value(QStringLiteral("FREQ"));
    const int interval = qMax(1, parts.value(QStringLiteral("INTERVAL"), QStringLiteral("1")).toInt());
    const int count = parts.value(QStringLiteral("COUNT")).toInt(); // 0: unlimited
    QDateTime until;
    if (parts.contains(QStringLiteral("UNTIL"))) {
        Property p;
        p.value = parts.value(QStringLiteral("UNTIL"));
        until = readTime(p);
        if (p.value.size() == 8)
            until = until.addDays(1); // a date: the whole day counts
    }
    // BYDAY for weekly rules: MO,WE,FR.
    static const QHash<QString, int> days{{QStringLiteral("MO"), 1}, {QStringLiteral("TU"), 2}, {QStringLiteral("WE"), 3},
                                          {QStringLiteral("TH"), 4}, {QStringLiteral("FR"), 5}, {QStringLiteral("SA"), 6},
                                          {QStringLiteral("SU"), 7}};
    QList<int> byDay;
    for (const QString &d : parts.value(QStringLiteral("BYDAY")).split(u',', Qt::SkipEmptyParts))
        if (days.contains(d.right(2)))
            byDay << days.value(d.right(2));
    std::sort(byDay.begin(), byDay.end());

    QList<QDateTime> starts;
    auto add = [&](const QDateTime &dt) {
        if (dt >= first && (!until.isValid() || dt <= until) && dt <= to)
            starts << dt;
    };
    auto full = [&] { return count > 0 && starts.size() >= count; };
    for (int n = 0; n < MaxOccurrences && !full(); ++n) {
        QDateTime period;
        if (freq == QLatin1String("DAILY"))
            period = first.addDays(qint64(n) * interval);
        else if (freq == QLatin1String("WEEKLY"))
            period = first.addDays(qint64(n) * 7 * interval);
        else if (freq == QLatin1String("MONTHLY"))
            period = first.addMonths(n * interval);
        else if (freq == QLatin1String("YEARLY"))
            period = first.addYears(n * interval);
        else
            return {first}; // a rule we do not know: just the first occurrence
        if (period > to || (until.isValid() && period > until))
            break;
        if (freq == QLatin1String("WEEKLY") && !byDay.isEmpty()) {
            // Each listed weekday of this period's week, at the event's time.
            const QDateTime monday = period.addDays(1 - period.date().dayOfWeek());
            for (int d : std::as_const(byDay)) {
                if (full())
                    break;
                add(monday.addDays(d - 1));
            }
        } else {
            add(period);
        }
    }
    return starts;
}

} // namespace

QString Event::meetingLink() const
{
    static const QRegularExpression link(
        QStringLiteral("https://[^\\s<>\"]*(?:meet\\.google\\.com|zoom\\.us|teams\\.microsoft\\.com|teams\\.live\\.com|meet\\.jit\\.si|webex\\.com|whereby\\.com)[^\\s<>\"]*"));
    for (const QString &text : {location, url, description}) {
        const auto m = link.match(text);
        if (m.hasMatch())
            return m.captured(0);
    }
    return {};
}

QList<Event> parse(const QByteArray &ics, const QDateTime &from, const QDateTime &to)
{
    QList<RawEvent> raw;
    RawEvent *current = nullptr;
    int nesting = 0; // VALARM etc. inside an event
    for (const QString &line : unfold(ics)) {
        const Property p = readProperty(line);
        if (p.name == QLatin1String("BEGIN")) {
            if (p.value == QLatin1String("VEVENT")) {
                raw << RawEvent();
                current = &raw.last();
            } else if (current) {
                ++nesting;
            }
            continue;
        }
        if (p.name == QLatin1String("END")) {
            if (p.value == QLatin1String("VEVENT"))
                current = nullptr;
            else if (current && nesting > 0)
                --nesting;
            continue;
        }
        if (!current || nesting > 0)
            continue;
        Event &e = current->event;
        if (p.name == QLatin1String("UID"))
            current->uid = p.value;
        else if (p.name == QLatin1String("SUMMARY"))
            e.title = unescape(p.value);
        else if (p.name == QLatin1String("LOCATION"))
            e.location = unescape(p.value);
        else if (p.name == QLatin1String("DESCRIPTION"))
            e.description = unescape(p.value);
        else if (p.name == QLatin1String("URL"))
            e.url = p.value;
        else if (p.name == QLatin1String("DTSTART"))
            e.start = readTime(p, &e.allDay);
        else if (p.name == QLatin1String("DTEND"))
            e.end = readTime(p);
        else if (p.name == QLatin1String("DURATION"))
            e.end = e.start.addSecs(durationSeconds(p.value)); // DTSTART comes first in practice
        else if (p.name == QLatin1String("RRULE"))
            current->rrule = p.value;
        else if (p.name == QLatin1String("EXDATE"))
            for (const QString &v : p.value.split(u','))
                current->exdates << readTime(Property{p.name, p.params, v});
        else if (p.name == QLatin1String("RECURRENCE-ID"))
            current->recurrenceId = readTime(p);
        else if (p.name == QLatin1String("STATUS"))
            current->cancelled = p.value.compare(QLatin1String("CANCELLED"), Qt::CaseInsensitive) == 0;
    }

    // Occurrences moved or cancelled individually replace the rule's ones.
    QHash<QString, QList<QDateTime>> moved;
    for (const RawEvent &r : std::as_const(raw))
        if (r.recurrenceId.isValid())
            moved[r.uid] << r.recurrenceId;

    QList<Event> events;
    for (const RawEvent &r : std::as_const(raw)) {
        if (!r.event.start.isValid())
            continue;
        Event base = r.event;
        if (!base.end.isValid())
            base.end = base.allDay ? base.start.addDays(1) : base.start;
        const qint64 length = base.start.secsTo(base.end);
        const QList<QDateTime> starts = r.rrule.isEmpty() || r.recurrenceId.isValid() ? QList<QDateTime>{base.start}
                                                                                       : occurrences(base.start, r.rrule, to);
        for (const QDateTime &start : starts) {
            if (r.cancelled || r.exdates.contains(start) || (!r.recurrenceId.isValid() && moved.value(r.uid).contains(start)))
                continue;
            Event e = base;
            e.start = start;
            e.end = start.addSecs(length);
            if (e.end > from && e.start < to)
                events << e;
        }
    }
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.start < b.start; });
    return events;
}

} // namespace Ics
