// Durations, moments, timers and reminders, with a fixed clock.

#include "timeparse.h"

#include <QTest>
#include <QTimeZone>

using namespace TimeParse;

class TimeParseTest : public QObject
{
    Q_OBJECT

private:
    // Wednesday, 4 November 2026, 14:00 UTC.
    const QDateTime m_now{QDate(2026, 11, 4), QTime(14, 0), QTimeZone::UTC};

private Q_SLOTS:
    void durations()
    {
        QCOMPARE(duration(QStringLiteral("10m")), 600);
        QCOMPARE(duration(QStringLiteral("10 min")), 600);
        QCOMPARE(duration(QStringLiteral("1h30m")), 5400);
        QCOMPARE(duration(QStringLiteral("1h 30")), 5400);
        QCOMPARE(duration(QStringLiteral("90s")), 90);
        QCOMPARE(duration(QStringLiteral("1.5 hours")), 5400);
        QCOMPARE(duration(QStringLiteral("2 hours 5 minutes")), 7500);
        QVERIFY(!duration(QStringLiteral("10")));
        QVERIFY(!duration(QStringLiteral("tea")));
        QVERIFY(!duration(QStringLiteral("10m tea")));
    }

    void moments()
    {
        QCOMPARE(moment(QStringLiteral("5pm"), m_now)->time(), QTime(17, 0));
        QCOMPARE(moment(QStringLiteral("5pm"), m_now)->date(), QDate(2026, 11, 4));
        QCOMPARE(moment(QStringLiteral("9am"), m_now)->date(), QDate(2026, 11, 5)); // already past today
        QCOMPARE(moment(QStringLiteral("tomorrow at 8:15"), m_now)->time(), QTime(8, 15));
        QCOMPARE(moment(QStringLiteral("tomorrow"), m_now)->time(), QTime(9, 0));
        QVERIFY(!moment(QStringLiteral("13pm"), m_now));
        QVERIFY(!moment(QStringLiteral("soon"), m_now));
    }

    void timers()
    {
        QCOMPARE(timer(QStringLiteral("timer 10m"))->seconds, 600);
        QCOMPARE(timer(QStringLiteral("timer 2 hours"))->seconds, 7200);
        QCOMPARE(timer(QStringLiteral("timer 1h30m tea"))->label, QStringLiteral("tea"));
        QCOMPARE(timer(QStringLiteral("timer 5 min for pasta"))->label, QStringLiteral("pasta"));
        QCOMPARE(timer(QStringLiteral("10 min timer"))->seconds, 600);
        QCOMPARE(timer(QStringLiteral("5m tea timer"))->label, QStringLiteral("tea"));
        QVERIFY(!timer(QStringLiteral("timer")));
        QVERIFY(!timer(QStringLiteral("timer tea")));
        QVERIFY(!timer(QStringLiteral("timers")));
    }

    void reminders()
    {
        auto r = reminder(QStringLiteral("remind me in 20 min to call mom"), m_now);
        QVERIFY(r);
        QCOMPARE(r->when, m_now.addSecs(1200));
        QCOMPARE(r->text, QStringLiteral("call mom"));
        r = reminder(QStringLiteral("remind me at 5pm to leave"), m_now);
        QCOMPARE(r->when.time(), QTime(17, 0));
        QCOMPARE(r->text, QStringLiteral("leave"));
        r = reminder(QStringLiteral("remind me tomorrow at 9 to send the report"), m_now);
        QCOMPARE(r->when, QDateTime(QDate(2026, 11, 5), QTime(9, 0), QTimeZone::UTC));
        QCOMPARE(r->text, QStringLiteral("send the report"));
        r = reminder(QStringLiteral("remind me to stretch in 1h"), m_now);
        QCOMPARE(r->when, m_now.addSecs(3600));
        QCOMPARE(r->text, QStringLiteral("stretch"));
        QVERIFY(!reminder(QStringLiteral("remind me"), m_now));
        QVERIFY(!reminder(QStringLiteral("remind me to call mom"), m_now)); // when?
    }

    void clocks()
    {
        QCOMPARE(clock(299), QStringLiteral("4:59"));
        QCOMPARE(clock(3723), QStringLiteral("1:02:03"));
    }
};

QTEST_GUILESS_MAIN(TimeParseTest)
#include "timeparsetest.moc"
