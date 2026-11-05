#include "answers.h"

#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace Answers
{

namespace
{

QString titleCase(const QString &text)
{
    QStringList words = text.split(u' ', Qt::SkipEmptyParts);
    for (QString &w : words)
        if (w.size() > 3 || words.size() == 1) // keep short abbreviations as typed ("pst" → "PST")
            w[0] = w[0].toUpper();
        else
            w = w.toUpper();
    return words.join(u' ');
}

// Common names that are not the city in an IANA zone id.
const QHash<QString, QByteArray> &aliases()
{
    static const QHash<QString, QByteArray> table{
        {QStringLiteral("utc"), "UTC"},
        {QStringLiteral("gmt"), "UTC"},
        {QStringLiteral("est"), "America/New_York"},
        {QStringLiteral("edt"), "America/New_York"},
        {QStringLiteral("et"), "America/New_York"},
        {QStringLiteral("cst"), "America/Chicago"},
        {QStringLiteral("cdt"), "America/Chicago"},
        {QStringLiteral("mst"), "America/Denver"},
        {QStringLiteral("mdt"), "America/Denver"},
        {QStringLiteral("pst"), "America/Los_Angeles"},
        {QStringLiteral("pdt"), "America/Los_Angeles"},
        {QStringLiteral("pt"), "America/Los_Angeles"},
        {QStringLiteral("bst"), "Europe/London"},
        {QStringLiteral("cet"), "Europe/Berlin"},
        {QStringLiteral("cest"), "Europe/Berlin"},
        {QStringLiteral("ist"), "Asia/Kolkata"},
        {QStringLiteral("jst"), "Asia/Tokyo"},
        {QStringLiteral("aest"), "Australia/Sydney"},
        {QStringLiteral("san francisco"), "America/Los_Angeles"},
        {QStringLiteral("seattle"), "America/Los_Angeles"},
        {QStringLiteral("la"), "America/Los_Angeles"},
        {QStringLiteral("nyc"), "America/New_York"},
        {QStringLiteral("boston"), "America/New_York"},
        {QStringLiteral("washington"), "America/New_York"},
        {QStringLiteral("miami"), "America/New_York"},
        {QStringLiteral("dallas"), "America/Chicago"},
        {QStringLiteral("houston"), "America/Chicago"},
        {QStringLiteral("austin"), "America/Chicago"},
        {QStringLiteral("calgary"), "America/Edmonton"},
        {QStringLiteral("delhi"), "Asia/Kolkata"},
        {QStringLiteral("new delhi"), "Asia/Kolkata"},
        {QStringLiteral("mumbai"), "Asia/Kolkata"},
        {QStringLiteral("bangalore"), "Asia/Kolkata"},
        {QStringLiteral("bengaluru"), "Asia/Kolkata"},
        {QStringLiteral("hyderabad"), "Asia/Kolkata"},
        {QStringLiteral("chennai"), "Asia/Kolkata"},
        {QStringLiteral("beijing"), "Asia/Shanghai"},
        {QStringLiteral("hong kong"), "Asia/Hong_Kong"},
    };
    return table;
}

// Zone ids by their city in lower case ("new york" → America/New_York).
const QHash<QString, QByteArray> &cities()
{
    static const QHash<QString, QByteArray> table = [] {
        QHash<QString, QByteArray> t;
        for (const QByteArray &id : QTimeZone::availableTimeZoneIds()) {
            const QString city = QString::fromLatin1(id.mid(id.lastIndexOf('/') + 1)).replace(u'_', u' ').toLower();
            if (!t.contains(city))
                t.insert(city, id);
        }
        return t;
    }();
    return table;
}

// A zone for a country, if all of its zones currently agree on the offset
// ("india" yes; "canada", spanning six offsets, no).
QTimeZone countryZone(const QString &name)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int t = int(QLocale::AnyTerritory) + 1; t <= int(QLocale::LastTerritory); ++t) {
        const auto territory = QLocale::Territory(t);
        if (QLocale::territoryToString(territory).compare(name, Qt::CaseInsensitive) != 0)
            continue;
        const QList<QByteArray> ids = QTimeZone::availableTimeZoneIds(territory);
        if (ids.isEmpty())
            return {};
        const int offset = QTimeZone(ids.first()).offsetFromUtc(now);
        for (const QByteArray &id : ids)
            if (QTimeZone(id).offsetFromUtc(now) != offset)
                return {};
        return QTimeZone(ids.first());
    }
    return {};
}

// "3pm", "3:30 pm", "15:30". Plain numbers are not times.
std::optional<QTime> parseTime(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{1,2})(?::(\\d{2}))?\\s*(am|pm)?$"));
    const auto m = re.match(text.trimmed());
    if (!m.hasMatch() || (m.captured(2).isEmpty() && m.captured(3).isEmpty()))
        return std::nullopt;
    int hour = m.captured(1).toInt();
    const int minute = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    if (!m.captured(3).isEmpty()) {
        if (hour < 1 || hour > 12)
            return std::nullopt;
        hour = hour % 12 + (m.captured(3) == QLatin1String("pm") ? 12 : 0);
    }
    const QTime t(hour, minute);
    return t.isValid() ? std::optional(t) : std::nullopt;
}

QString offsetText(qint64 seconds)
{
    if (seconds == 0)
        return QObject::tr("same time as you");
    const double hours = std::abs(seconds) / 3600.0;
    const QString amount = std::fmod(hours, 1.0) == 0 ? QString::number(int(hours)) : QString::number(hours, 'f', 1);
    return seconds > 0 ? QObject::tr("%1 h ahead of you").arg(amount) : QObject::tr("%1 h behind you").arg(amount);
}

QString timeText(const QDateTime &dt)
{
    const QLocale locale;
    return locale.toString(dt.time(), QLocale::ShortFormat) + QStringLiteral(" · ")
        + locale.toString(dt.date(), QStringLiteral("ddd, MMM d"));
}

// Month names in English and the user's language.
int monthNumber(const QString &word)
{
    for (const QLocale &locale : {QLocale(QLocale::English), QLocale()}) {
        for (int m = 1; m <= 12; ++m)
            if (word.compare(locale.monthName(m), Qt::CaseInsensitive) == 0
                || word.compare(locale.monthName(m, QLocale::ShortFormat).remove(u'.'), Qt::CaseInsensitive) == 0)
                return m;
    }
    return 0;
}

// "2026-12-25", "dec 25", "25 december", "december 25 2026", "christmas".
// Without a year it means the next such day (today included).
std::optional<QDate> parseDate(const QString &text, const QDate &today)
{
    const QString t = text.trimmed().remove(u',');
    const QDate iso = QDate::fromString(t, Qt::ISODate);
    if (iso.isValid())
        return iso;
    if (t == QLatin1String("christmas"))
        return parseDate(QStringLiteral("dec 25"), today);
    if (t == QLatin1String("new year") || t == QLatin1String("new years") || t == QLatin1String("new year's"))
        return parseDate(QStringLiteral("jan 1"), today.addDays(1));

    static const QRegularExpression monthDay(QStringLiteral("^([a-z]+)\\.? (\\d{1,2})(?:st|nd|rd|th)?(?: (\\d{4}))?$"));
    static const QRegularExpression dayMonth(QStringLiteral("^(\\d{1,2})(?:st|nd|rd|th)? ([a-z]+)\\.?(?: (\\d{4}))?$"));
    int month = 0, day = 0, year = 0;
    if (const auto m = monthDay.match(t); m.hasMatch()) {
        month = monthNumber(m.captured(1));
        day = m.captured(2).toInt();
        year = m.captured(3).toInt();
    } else if (const auto m = dayMonth.match(t); m.hasMatch()) {
        day = m.captured(1).toInt();
        month = monthNumber(m.captured(2));
        year = m.captured(3).toInt();
    }
    if (month == 0)
        return std::nullopt;
    QDate date(year ? year : today.year(), month, day);
    if (!year && date.isValid() && date < today)
        date = date.addYears(1);
    return date.isValid() ? std::optional(date) : std::nullopt;
}

QString longDate(const QDate &date)
{
    return QLocale().toString(date, QLocale::LongFormat);
}

QString daysText(qint64 days)
{
    QString text = days == 1 ? QObject::tr("1 day") : QObject::tr("%1 days").arg(days);
    if (days >= 14)
        text += QStringLiteral(" · %1 weeks %2 days").arg(days / 7).arg(days % 7);
    return text;
}

} // namespace

QTimeZone findZone(const QString &rawName)
{
    const QString name = rawName.trimmed().toLower();
    if (name.isEmpty())
        return {};
    if (const auto it = aliases().constFind(name); it != aliases().constEnd())
        return QTimeZone(*it);
    if (const auto it = cities().constFind(name); it != cities().constEnd())
        return QTimeZone(*it);
    return countryZone(name);
}

std::optional<Answer> timeZone(const QString &query, const QDateTime &now)
{
    const QString q = query.simplified().toLower().remove(u'?');
    static const QRegularExpression currentIn(
        QStringLiteral("^(?:what(?:'s| is) the |current )?time(?: is it)?(?: now)? in (.+)$"));
    static const QRegularExpression placeTime(QStringLiteral("^(.+) time$"));
    static const QRegularExpression convert(QStringLiteral("^(\\d{1,2}(?::\\d{2})?\\s*(?:am|pm)?) (?:in )?(.+?) (?:in|to) (.+)$"));
    static const QRegularExpression timeIn(QStringLiteral("^(\\d{1,2}(?::\\d{2})?\\s*(?:am|pm)?) in (.+)$"));
    static const QRegularExpression timeAt(QStringLiteral("^(\\d{1,2}(?::\\d{2})?\\s*(?:am|pm)?) (.+)$"));
    const QTimeZone local = QTimeZone::systemTimeZone();

    // The current time somewhere.
    QString place;
    if (auto m = currentIn.match(q); m.hasMatch())
        place = m.captured(1);
    else if (auto m = placeTime.match(q); m.hasMatch())
        place = m.captured(1);
    if (!place.isEmpty()) {
        const QTimeZone zone = findZone(place);
        if (!zone.isValid())
            return std::nullopt;
        const QDateTime there = now.toTimeZone(zone);
        const qint64 diff = zone.offsetFromUtc(now) - local.offsetFromUtc(now);
        return Answer{timeText(there), titleCase(place) + QStringLiteral(" · ") + offsetText(diff),
                      QLocale().toString(there.time(), QLocale::ShortFormat), {}, {}};
    }

    // A time in one place, converted to another (or to here).
    QString timeWord, from, to;
    if (auto m = convert.match(q); m.hasMatch())
        timeWord = m.captured(1), from = m.captured(2), to = m.captured(3);
    else if (auto m = timeIn.match(q); m.hasMatch())
        timeWord = m.captured(1), to = m.captured(2); // from here
    else if (auto m = timeAt.match(q); m.hasMatch())
        timeWord = m.captured(1), from = m.captured(2); // to here
    if (timeWord.isEmpty())
        return std::nullopt;
    const auto time = parseTime(timeWord);
    const QTimeZone fromZone = from.isEmpty() ? local : findZone(from);
    const QTimeZone toZone = to.isEmpty() ? local : findZone(to);
    if (!time || !fromZone.isValid() || !toZone.isValid())
        return std::nullopt;
    const QDateTime start(now.toTimeZone(fromZone).date(), *time, fromZone);
    const QDateTime result = start.toTimeZone(toZone);
    const QString fromName = from.isEmpty() ? QObject::tr("here") : titleCase(from);
    const QString toName = to.isEmpty() ? QObject::tr("here") : titleCase(to);
    return Answer{timeText(result) + QStringLiteral(" ") + (to.isEmpty() ? QObject::tr("here") : QObject::tr("in %1").arg(toName)),
                  QObject::tr("%1 in %2").arg(QLocale().toString(*time, QLocale::ShortFormat), fromName),
                  QLocale().toString(result.time(), QLocale::ShortFormat), {}, {}};
}

std::optional<Answer> dates(const QString &query, const QDate &today)
{
    const QString q = query.simplified().toLower().remove(u'?');
    if (q == QLatin1String("week number") || q == QLatin1String("week") || q == QLatin1String("what week is it")) {
        const QDate monday = today.addDays(1 - today.dayOfWeek());
        const QString n = QString::number(today.weekNumber());
        return Answer{QObject::tr("Week %1").arg(n),
                      QStringLiteral("%1 – %2").arg(QLocale().toString(monday, QStringLiteral("MMM d")),
                                                     QLocale().toString(monday.addDays(6), QStringLiteral("MMM d"))),
                      n, {}, {}};
    }

    static const QRegularExpression until(QStringLiteral("^(?:how many )?days (?:until|till|til|to|before) (.+)$"));
    static const QRegularExpression since(QStringLiteral("^(?:how many )?days (?:since|after) (.+)$"));
    if (auto m = until.match(q); m.hasMatch()) {
        if (const auto d = parseDate(m.captured(1), today)) {
            const qint64 n = today.daysTo(*d);
            return Answer{daysText(n), QObject::tr("until %1").arg(longDate(*d)), QString::number(n), {}, {}};
        }
    }
    if (auto m = since.match(q); m.hasMatch()) {
        if (auto d = parseDate(m.captured(1), today)) {
            if (*d > today && !m.captured(1).contains(QRegularExpression(QStringLiteral("\\d{4}"))))
                d = d->addYears(-1); // "days since dec 25" means the last one
            const qint64 n = d->daysTo(today);
            return Answer{daysText(n), QObject::tr("since %1").arg(longDate(*d)), QString::number(n), {}, {}};
        }
    }

    static const QRegularExpression offset(
        QStringLiteral("^(in )?(\\d+) (day|week|month|year)s?( from (?:now|today)| ago)?$"));
    if (auto m = offset.match(q); m.hasMatch() && (!m.captured(1).isEmpty() || !m.captured(4).isEmpty())) {
        const int n = m.captured(2).toInt() * (m.captured(4) == QLatin1String(" ago") ? -1 : 1);
        const QString unit = m.captured(3);
        const QDate d = unit == QLatin1String("day") ? today.addDays(n)
            : unit == QLatin1String("week")          ? today.addDays(7 * n)
            : unit == QLatin1String("month")         ? today.addMonths(n)
                                                     : today.addYears(n);
        return Answer{longDate(d), q, QLocale().toString(d, QLocale::ShortFormat), {}, {}};
    }

    static const QRegularExpression next(QStringLiteral("^next ([a-z]+)$"));
    if (auto m = next.match(q); m.hasMatch()) {
        for (int day = 1; day <= 7; ++day) {
            if (m.captured(1).compare(QLocale(QLocale::English).dayName(day), Qt::CaseInsensitive) == 0) {
                int ahead = (day - today.dayOfWeek() + 7) % 7;
                const QDate d = today.addDays(ahead == 0 ? 7 : ahead);
                return Answer{longDate(d), QObject::tr("in %1").arg(daysText(today.daysTo(d))),
                              QLocale().toString(d, QLocale::ShortFormat), {}, {}};
            }
        }
    }
    return std::nullopt;
}

std::optional<Answer> percent(const QString &query)
{
    const QString q = query.trimmed().toLower().replace(u',', QString()); // thousands separators
    static const QString n = QStringLiteral(R"((-?\d+(?:\.\d+)?))");
    auto match = [&](const QString &pattern) { return QRegularExpression(QStringLiteral("^") + pattern + QStringLiteral("$")).match(q); };
    auto number = [](double v) {
        // Up to 4 decimals, without trailing zeros.
        QString s = QString::number(v, 'f', 4);
        if (s.contains(u'.')) {
            while (s.endsWith(u'0'))
                s.chop(1);
            if (s.endsWith(u'.'))
                s.chop(1);
        }
        return s == QLatin1String("-0") ? QStringLiteral("0") : s;
    };
    auto answer = [&](double value, const QString &what, bool isPercent) {
        const QString text = number(value) + (isPercent ? QStringLiteral("%") : QString());
        return Answer{text, what, text, {}, {}};
    };
    QRegularExpressionMatch m;
    if ((m = match(n + QStringLiteral(R"(\s*%\s*of\s*)") + n)).hasMatch())
        return answer(m.captured(1).toDouble() / 100 * m.captured(2).toDouble(), QObject::tr("%1% of %2").arg(m.captured(1), m.captured(2)), false);
    if ((m = match(n + QStringLiteral(R"(\s*%\s*off\s*)") + n)).hasMatch())
        return answer(m.captured(2).toDouble() * (1 - m.captured(1).toDouble() / 100), QObject::tr("%2 less %1%").arg(m.captured(1), m.captured(2)), false);
    if ((m = match(n + QStringLiteral(R"(\s*([+-])\s*)") + n + QStringLiteral(R"(\s*%)"))).hasMatch()) {
        const double sign = m.captured(2) == QLatin1String("-") ? -1 : 1;
        return answer(m.captured(1).toDouble() * (1 + sign * m.captured(3).toDouble() / 100),
                      QObject::tr("%1 %2 %3%").arg(m.captured(1), m.captured(2), m.captured(3)), false);
    }
    if ((m = match(n + QStringLiteral(R"(\s*is\s*what\s*(?:%|percent)\s*of\s*)") + n)).hasMatch() && m.captured(2).toDouble() != 0)
        return answer(m.captured(1).toDouble() / m.captured(2).toDouble() * 100, QObject::tr("%1 of %2").arg(m.captured(1), m.captured(2)), true);
    if ((m = match(QStringLiteral(R"(what\s*(?:%|percent)\s*of\s*)") + n + QStringLiteral(R"(\s*is\s*)") + n)).hasMatch() && m.captured(1).toDouble() != 0)
        return answer(m.captured(2).toDouble() / m.captured(1).toDouble() * 100, QObject::tr("%1 of %2").arg(m.captured(2), m.captured(1)), true);
    if ((m = match(QStringLiteral(R"((?:from\s*)?)") + n + QStringLiteral(R"(\s*to\s*)") + n + QStringLiteral(R"(\s*(?:in\s*)?(?:%|percent))"))).hasMatch()
        && m.captured(1).toDouble() != 0) {
        const double change = (m.captured(2).toDouble() - m.captured(1).toDouble()) / std::abs(m.captured(1).toDouble()) * 100;
        Answer a = answer(change, QObject::tr("Change from %1 to %2").arg(m.captured(1), m.captured(2)), true);
        if (change > 0)
            a.title.prepend(u'+');
        return a;
    }
    return std::nullopt;
}

std::optional<Answer> colour(const QString &query)
{
    const QString q = query.trimmed().toLower();
    static const QRegularExpression hex(QStringLiteral("^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$"));
    static const QRegularExpression rgb(QStringLiteral("^rgba?\\(\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*(?:,\\s*([\\d.]+)\\s*)?\\)$"));
    static const QRegularExpression hsl(QStringLiteral("^hsla?\\(\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})%?\\s*,\\s*(\\d{1,3})%?\\s*\\)$"));
    QColor c;
    if (auto m = hex.match(q); m.hasMatch()) {
        QString digits = m.captured(1);
        if (digits.size() == 8) // CSS order is #rrggbbaa; QColor wants #aarrggbb
            digits = digits.right(2) + digits.left(6);
        c = QColor(u'#' + digits);
    } else if (auto m = rgb.match(q); m.hasMatch()) {
        if (m.captured(1).toInt() > 255 || m.captured(2).toInt() > 255 || m.captured(3).toInt() > 255)
            return std::nullopt;
        c = QColor(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        if (!m.captured(4).isEmpty())
            c.setAlphaF(std::clamp(m.captured(4).toFloat(), 0.f, 1.f));
    } else if (auto m = hsl.match(q); m.hasMatch()) {
        if (m.captured(1).toInt() > 360 || m.captured(2).toInt() > 100 || m.captured(3).toInt() > 100)
            return std::nullopt;
        c = QColor::fromHslF(m.captured(1).toInt() % 360 / 360.f, m.captured(2).toInt() / 100.f, m.captured(3).toInt() / 100.f);
    }
    if (!c.isValid())
        return std::nullopt;

    const QString hexText = c.alpha() < 255 ? QStringLiteral("#%1%2").arg(c.name().mid(1)).arg(c.alpha(), 2, 16, QLatin1Char('0')).toUpper()
                                            : c.name().toUpper();
    const QString rgbText = c.alpha() < 255 ? QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'g', 2)
                                            : QStringLiteral("rgb(%1, %2, %3)").arg(c.red()).arg(c.green()).arg(c.blue());
    const QString hslText = QStringLiteral("hsl(%1, %2%, %3%)")
                                .arg(qMax(0, c.hslHue()))
                                .arg(qRound(c.hslSaturationF() * 100))
                                .arg(qRound(c.lightnessF() * 100));
    return Answer{hexText, rgbText + QStringLiteral(" · ") + hslText, hexText, c,
                  {{QObject::tr("Copy RGB"), rgbText}, {QObject::tr("Copy HSL"), hslText}}};
}

} // namespace Answers
