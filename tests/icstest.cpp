// Calendar feed parsing: time zones, all-day events, repeats and exceptions.

#include "ics.h"

#include <QTest>
#include <QTimeZone>

class IcsTest : public QObject
{
    Q_OBJECT

private:
    const QByteArray m_ics = "BEGIN:VCALENDAR\r\n"
                             "BEGIN:VEVENT\r\nUID:dentist\r\nSUMMARY:Dentist\r\n"
                             "DTSTART;TZID=America/New_York:20261105T090000\r\nDTEND;TZID=America/New_York:20261105T100000\r\n"
                             "LOCATION:12 Main St\\, Suite 4\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:holiday\r\nSUMMARY:Day off\r\nDTSTART;VALUE=DATE:20261106\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:standup\r\nSUMMARY:Standup\r\nDTSTART:20261102T150000Z\r\nDURATION:PT15M\r\n"
                             "RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=6\r\nEXDATE:20261104T150000Z\r\n"
                             "DESCRIPTION:Join here: https://us02web.zoom.us/j/123\r\n 456?pwd=abc\r\n"
                             "BEGIN:VALARM\r\nTRIGGER:-PT5M\r\nDESCRIPTION:ignored\r\nEND:VALARM\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:standup\r\nRECURRENCE-ID:20261106T150000Z\r\nSUMMARY:Standup (late)\r\n"
                             "DTSTART:20261106T170000Z\r\nDTEND:20261106T171500Z\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:gone\r\nSUMMARY:Cancelled\r\nSTATUS:CANCELLED\r\nDTSTART:20261107T100000Z\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:win\r\nSUMMARY:Outlook meeting\r\n"
                             "DTSTART;TZID=\"Mountain Standard Time\":20261110T080000\r\nDTEND;TZID=\"Mountain Standard Time\":20261110T090000\r\n"
                             "END:VEVENT\r\nEND:VCALENDAR\r\n";
    QList<Ics::Event> events() const
    {
        return Ics::parse(m_ics, QDateTime(QDate(2026, 11, 1), QTime(0, 0), QTimeZone::UTC),
                          QDateTime(QDate(2026, 11, 30), QTime(0, 0), QTimeZone::UTC));
    }
    QList<Ics::Event> named(const QString &title) const
    {
        QList<Ics::Event> out;
        for (const Ics::Event &e : events())
            if (e.title == title)
                out << e;
        return out;
    }

private Q_SLOTS:
    void timeZones()
    {
        const Ics::Event dentist = named(QStringLiteral("Dentist")).value(0);
        QCOMPARE(dentist.start.toUTC(), QDateTime(QDate(2026, 11, 5), QTime(14, 0), QTimeZone::UTC)); // EST = UTC−5
        QCOMPARE(dentist.location, QStringLiteral("12 Main St, Suite 4"));
        const Ics::Event outlook = named(QStringLiteral("Outlook meeting")).value(0);
        QVERIFY(outlook.start.isValid());
        QCOMPARE(outlook.start.toUTC().time(), QTime(15, 0)); // Windows "Mountain Standard Time" = UTC−7 in November
    }

    void allDay()
    {
        const auto off = named(QStringLiteral("Day off"));
        QCOMPARE(off.size(), 1);
        QVERIFY(off.first().allDay);
        QCOMPARE(off.first().start.daysTo(off.first().end), 1);
    }

    void repeats()
    {
        const auto standups = named(QStringLiteral("Standup"));
        // COUNT=6 on Mon/Wed/Fri from Nov 2: 2, 4, 6, 9, 11, 13; the 4th is
        // excluded and the 6th moved (a separate event).
        QList<int> days;
        for (const auto &e : standups)
            days << e.start.toUTC().date().day();
        QCOMPARE(days, (QList<int>{2, 9, 11, 13}));
        QCOMPARE(standups.first().start.secsTo(standups.first().end), 15 * 60);
        const auto late = named(QStringLiteral("Standup (late)"));
        QCOMPARE(late.size(), 1);
        QCOMPARE(late.first().start.toUTC().time(), QTime(17, 0));
    }

    void meetingLinks()
    {
        QCOMPARE(named(QStringLiteral("Standup")).first().meetingLink(), QStringLiteral("https://us02web.zoom.us/j/123456?pwd=abc"));
        QVERIFY(named(QStringLiteral("Dentist")).first().meetingLink().isEmpty());
    }

    void cancelledAndSorted()
    {
        QVERIFY(named(QStringLiteral("Cancelled")).isEmpty());
        const auto all = events();
        for (int i = 1; i < all.size(); ++i)
            QVERIFY(all.at(i - 1).start <= all.at(i).start);
    }
};

QTEST_GUILESS_MAIN(IcsTest)
#include "icstest.moc"
