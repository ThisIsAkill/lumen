// Instant answers with a fixed clock, local time zone (UTC) and C locale.

#include "answers.h"
#include "fuzzy.h"
#include "monitorsource.h"
#include "screentools.h"
#include "transforms.h"
#include "weathersource.h"

#include <QTest>
#include <QTimeZone>

using namespace Answers;

class AnswersTest : public QObject
{
    Q_OBJECT

private:
    // Wednesday, 4 November 2026, noon UTC.
    const QDateTime m_now{QDate(2026, 11, 4), QTime(12, 0), QTimeZone::UTC};
    const QDate m_today{2026, 11, 4};

private Q_SLOTS:
    void percentages()
    {
        auto title = [](const char *q) { const auto a = Answers::percent(QString::fromUtf8(q)); return a ? a->title : QStringLiteral("—"); };
        QCOMPARE(title("15% of 80"), QStringLiteral("12"));
        QCOMPARE(title("12.5 % of 1,000"), QStringLiteral("125"));
        QCOMPARE(title("80 + 15%"), QStringLiteral("92"));
        QCOMPARE(title("80 - 15%"), QStringLiteral("68"));
        QCOMPARE(title("15% off 80"), QStringLiteral("68"));
        QCOMPARE(title("12 is what % of 80"), QStringLiteral("15%"));
        QCOMPARE(title("what percent of 80 is 12"), QStringLiteral("15%"));
        QCOMPARE(title("80 to 92 in %"), QStringLiteral("+15%"));
        QCOMPARE(title("from 80 to 60 percent"), QStringLiteral("-25%"));
        QCOMPARE(title("1 is what % of 3"), QStringLiteral("33.3333%"));
        QCOMPARE(title("15 of 80"), QStringLiteral("—"));
        QCOMPARE(title("12 is what % of 0"), QStringLiteral("—"));
    }

    void zonesByName()
    {
        QVERIFY(findZone(QStringLiteral("tokyo")).isValid());
        QVERIFY(findZone(QStringLiteral("New York")).isValid());
        QCOMPARE(findZone(QStringLiteral("india")).id(), QByteArray("Asia/Kolkata"));
        QCOMPARE(findZone(QStringLiteral("pst")).id(), QByteArray("America/Los_Angeles"));
        QVERIFY(!findZone(QStringLiteral("canada")).isValid()); // six offsets: ambiguous
        QVERIFY(!findZone(QStringLiteral("lunch")).isValid());
    }

    void currentTimeSomewhere()
    {
        const auto a = timeZone(QStringLiteral("time in tokyo"), m_now);
        QVERIFY(a);
        QCOMPARE(a->copyText, QStringLiteral("21:00"));
        QVERIFY(a->title.contains(QStringLiteral("Wed")));
        QCOMPARE(a->subtitle, QStringLiteral("Tokyo · 9 h ahead of you"));
        const auto india = timeZone(QStringLiteral("india time"), m_now);
        QVERIFY(india);
        QCOMPARE(india->copyText, QStringLiteral("17:30"));
        QCOMPARE(india->subtitle, QStringLiteral("India · 5.5 h ahead of you"));
    }

    void convertTimes()
    {
        const auto a = timeZone(QStringLiteral("9am pst to cet"), m_now);
        QVERIFY(a);
        QCOMPARE(a->copyText, QStringLiteral("18:00"));
        const auto b = timeZone(QStringLiteral("3pm tokyo in utc"), m_now);
        QVERIFY(b);
        QCOMPARE(b->copyText, QStringLiteral("06:00"));
        const auto c = timeZone(QStringLiteral("5pm in tokyo"), m_now); // from here
        QVERIFY(c);
        QCOMPARE(c->copyText, QStringLiteral("02:00"));
    }

    void notTimes()
    {
        QVERIFY(!timeZone(QStringLiteral("3 tokyo"), m_now));
        QVERIFY(!timeZone(QStringLiteral("lunch time"), m_now));
        QVERIFY(!timeZone(QStringLiteral("firefox"), m_now));
    }

    void dateMaths()
    {
        QCOMPARE(dates(QStringLiteral("days until dec 25"), m_today)->copyText, QStringLiteral("51"));
        QCOMPARE(dates(QStringLiteral("days since 2026-01-01"), m_today)->copyText, QStringLiteral("307"));
        QCOMPARE(dates(QStringLiteral("days until christmas"), m_today)->copyText, QStringLiteral("51"));
        QVERIFY(dates(QStringLiteral("in 3 weeks"), m_today)->title.contains(QStringLiteral("25")));
        QVERIFY(dates(QStringLiteral("100 days ago"), m_today)->title.contains(QStringLiteral("27")));
        QVERIFY(dates(QStringLiteral("next friday"), m_today)->title.contains(QStringLiteral("6")));
        QCOMPARE(dates(QStringLiteral("week number"), m_today)->copyText, QStringLiteral("45"));
        QVERIFY(!dates(QStringLiteral("3 weeks"), m_today));
        QVERIFY(!dates(QStringLiteral("days"), m_today));
    }

    void typos()
    {
        QVERIFY(fuzzyScore(QStringLiteral("firfox"), QStringLiteral("Firefox")) > 0);       // missing letter
        QVERIFY(fuzzyScore(QStringLiteral("clipbaord"), QStringLiteral("Clipboard History")) > 0); // swapped letters
        QVERIFY(fuzzyScore(QStringLiteral("dolhpin"), QStringLiteral("Dolphin")) > 0);
        QVERIFY(fuzzyScore(QStringLiteral("firefox"), QStringLiteral("Firefox")) > fuzzyScore(QStringLiteral("firfox"), QStringLiteral("Firefox")));
        QCOMPARE(fuzzyScore(QStringLiteral("cat"), QStringLiteral("Car")), 0.0); // too short for typos
        QCOMPARE(fuzzyScore(QStringLiteral("zzzzzz"), QStringLiteral("Firefox")), 0.0);
    }

    void transforms()
    {
        auto apply = [](const char *id, const QString &text) -> QString {
            for (const auto &t : Transforms::all())
                if (t.id == QLatin1String(id))
                    return t.apply(text).value_or(QStringLiteral("<none>"));
            return QStringLiteral("<missing>");
        };
        QCOMPARE(apply("camel", QStringLiteral("hello big world")), QStringLiteral("helloBigWorld"));
        QCOMPARE(apply("snake", QStringLiteral("helloBigWorld")), QStringLiteral("hello_big_world"));
        QCOMPARE(apply("kebab", QStringLiteral("Hello, Big World!")), QStringLiteral("hello-big-world"));
        QCOMPARE(apply("title", QStringLiteral("don't stop me now")), QStringLiteral("Don't Stop Me Now"));
        QCOMPARE(apply("sentence", QStringLiteral("HELLO. HOW ARE YOU?")), QStringLiteral("Hello. How are you?"));
        QCOMPARE(apply("base64", QStringLiteral("hello")), QStringLiteral("aGVsbG8="));
        QCOMPARE(apply("unbase64", QStringLiteral("aGVsbG8=")), QStringLiteral("hello"));
        QCOMPARE(apply("unbase64", QStringLiteral("not base64!")), QStringLiteral("<none>"));
        QCOMPARE(apply("url", QStringLiteral("a b&c")), QStringLiteral("a%20b%26c"));
        QCOMPARE(apply("json", QStringLiteral("{\"a\":1}")), QStringLiteral("{\n    \"a\": 1\n}"));
        QCOMPARE(apply("json", QStringLiteral("{oops")), QStringLiteral("<none>"));
        QCOMPARE(apply("unique", QStringLiteral("b\na\nb")), QStringLiteral("b\na"));
        QCOMPARE(Transforms::counts(QStringLiteral("one two\nthree")), QStringLiteral("3 words · 13 characters · 2 lines"));
    }

    void generators()
    {
        QCOMPARE(Transforms::generate(QStringLiteral("uuid"))->value.size(), 36);
        QCOMPARE(Transforms::generate(QStringLiteral("password 24"))->value.size(), 24);
        QCOMPARE(Transforms::generate(QStringLiteral("pin 4"))->value.size(), 4);
        const int n = Transforms::generate(QStringLiteral("random 5-7"))->value.toInt();
        QVERIFY(n >= 5 && n <= 7);
        QCOMPARE(Transforms::generate(QStringLiteral("sha256 abc"))->value,
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(Transforms::generate(QStringLiteral("md5 abc"))->value, QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));
        QVERIFY(Transforms::generate(QStringLiteral("1793817403"))->subtitle.contains(QStringLiteral("2026-11-04")));
        QVERIFY(Transforms::generate(QStringLiteral("lorem 2"))->value.contains(QStringLiteral("\n\n")));
        QVERIFY(!Transforms::generate(QStringLiteral("firefox")));
        QVERIFY(!Transforms::generate(QStringLiteral("12345")));
    }

    void qrRoundTrip()
    {
        const QString text = QStringLiteral("https://example.com/lumen?x=1&y=ünïcode");
        const QImage code = ScreenTools::makeQr(text);
        QVERIFY(!code.isNull());
        QCOMPARE(ScreenTools::readCodes(code), QStringList{text});
        QVERIFY(ScreenTools::readCodes(QImage(64, 64, QImage::Format_RGB32)).isEmpty());
    }

    void systemReadings()
    {
        const auto cpu = MonitorSource::readCpu();
        QVERIFY(cpu.total > cpu.busy && cpu.busy > 0);
        const auto memory = MonitorSource::readMemory();
        QVERIFY(memory[0] > 0 && memory[1] <= memory[0]);
    }

    void weatherParsing()
    {
        // A trimmed real Open-Meteo answer.
        const QByteArray json = R"({"current":{"time":"2026-11-04T12:00","temperature_2m":-3.4,"apparent_temperature":-8.1,
            "relative_humidity_2m":81,"weather_code":71,"wind_speed_10m":12.2,"is_day":1},
            "daily":{"time":["2026-11-04","2026-11-05","2026-11-06"],"weather_code":[71,3,0],
            "temperature_2m_max":[-1.0,2.5,4.1],"temperature_2m_min":[-6.2,-4.0,-3.3]}})";
        const auto f = WeatherSource::parse(json);
        QVERIFY(f);
        QCOMPARE(qRound(f->temperature), -3);
        QCOMPARE(f->humidity, 81);
        QCOMPARE(WeatherSource::describe(f->code), QStringLiteral("Light snow"));
        QCOMPARE(f->days.size(), 3);
        QCOMPARE(f->days.at(1).date, QDate(2026, 11, 5));
        QCOMPARE(WeatherSource::iconName(f->days.at(2).code, true), QStringLiteral("weather-clear"));
        QVERIFY(!WeatherSource::parse("{}"));
        QCOMPARE(WeatherSource::defaultPlace(), QStringLiteral("UTC")); // TZ=UTC here; Edmonton on the real desktop
    }

    void colours()
    {
        const auto a = colour(QStringLiteral("#ff8800"));
        QVERIFY(a);
        QCOMPARE(a->copyText, QStringLiteral("#FF8800"));
        QCOMPARE(a->alternatives.at(0).second, QStringLiteral("rgb(255, 136, 0)"));
        QCOMPARE(a->alternatives.at(1).second, QStringLiteral("hsl(32, 100%, 50%)"));
        QCOMPARE(colour(QStringLiteral("#f80"))->copyText, QStringLiteral("#FF8800"));
        QCOMPARE(colour(QStringLiteral("rgb(255,136,0)"))->copyText, QStringLiteral("#FF8800"));
        QCOMPARE(colour(QStringLiteral("#ff880080"))->colour.alpha(), 0x80);
        QVERIFY(colour(QStringLiteral("hsl(32, 100%, 50%)"))->copyText.startsWith(QStringLiteral("#FF8")));
        QVERIFY(!colour(QStringLiteral("add")));
        QVERIFY(!colour(QStringLiteral("rgb(300, 0, 0)")));
    }
};

int main(int argc, char **argv)
{
    qputenv("TZ", "UTC");
    QLocale::setDefault(QLocale::c());
    QCoreApplication app(argc, argv);
    AnswersTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "answerstest.moc"
