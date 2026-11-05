#include "weathersource.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KIO/OpenUrlJob>
#include <KSharedConfig>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrlQuery>


namespace
{

constexpr int CacheMinutes = 15;

QString degrees(double value)
{
    return QStringLiteral("%1°").arg(qRound(value));
}

} // namespace

WeatherSource::WeatherSource(Launcher *launcher)
    : Source(launcher)
{
}

bool WeatherSource::imperial()
{
    const QString units = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Weather")).readEntry("Units", QStringLiteral("auto"));
    if (units != QLatin1String("auto"))
        return units == QLatin1String("imperial");
    return QLocale().measurementSystem() == QLocale::ImperialUSSystem;
}

QString WeatherSource::defaultPlace()
{
    const QString place = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Weather")).readEntry("Location");
    if (!place.isEmpty())
        return place;
    // "America/Edmonton" → "Edmonton"
    const QString zone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    return zone.section(u'/', -1).replace(u'_', u' ');
}

QString WeatherSource::describe(int code)
{
    switch (code) {
    case 0: return tr("Clear");
    case 1: return tr("Mainly clear");
    case 2: return tr("Partly cloudy");
    case 3: return tr("Overcast");
    case 45: case 48: return tr("Fog");
    case 51: case 53: case 55: return tr("Drizzle");
    case 56: case 57: return tr("Freezing drizzle");
    case 61: return tr("Light rain");
    case 63: return tr("Rain");
    case 65: return tr("Heavy rain");
    case 66: case 67: return tr("Freezing rain");
    case 71: return tr("Light snow");
    case 73: return tr("Snow");
    case 75: return tr("Heavy snow");
    case 77: return tr("Snow grains");
    case 80: case 81: return tr("Showers");
    case 82: return tr("Heavy showers");
    case 85: case 86: return tr("Snow showers");
    case 95: return tr("Thunderstorm");
    case 96: case 99: return tr("Thunderstorm with hail");
    default: return tr("Unknown");
    }
}

QString WeatherSource::iconName(int code, bool day)
{
    if (code == 0 || code == 1)
        return day ? QStringLiteral("weather-clear") : QStringLiteral("weather-clear-night");
    if (code == 2)
        return day ? QStringLiteral("weather-few-clouds") : QStringLiteral("weather-few-clouds-night");
    if (code == 3)
        return QStringLiteral("weather-overcast");
    if (code == 45 || code == 48)
        return QStringLiteral("weather-fog");
    if ((code >= 51 && code <= 57) || code == 61 || code == 80)
        return QStringLiteral("weather-showers-scattered");
    if ((code >= 63 && code <= 67) || code == 81 || code == 82)
        return QStringLiteral("weather-showers");
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return QStringLiteral("weather-snow");
    if (code >= 95)
        return QStringLiteral("weather-storm");
    return QStringLiteral("weather-none-available");
}

std::optional<WeatherSource::Forecast> WeatherSource::parse(const QByteArray &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject current = root.value(QStringLiteral("current")).toObject();
    if (current.isEmpty())
        return std::nullopt;
    Forecast f;
    f.temperature = current.value(QStringLiteral("temperature_2m")).toDouble();
    f.feelsLike = current.value(QStringLiteral("apparent_temperature")).toDouble();
    f.wind = current.value(QStringLiteral("wind_speed_10m")).toDouble();
    f.humidity = current.value(QStringLiteral("relative_humidity_2m")).toInt();
    f.code = current.value(QStringLiteral("weather_code")).toInt();
    f.day = current.value(QStringLiteral("is_day")).toInt(1) == 1;
    const QJsonObject daily = root.value(QStringLiteral("daily")).toObject();
    const QJsonArray dates = daily.value(QStringLiteral("time")).toArray();
    const QJsonArray codes = daily.value(QStringLiteral("weather_code")).toArray();
    const QJsonArray highs = daily.value(QStringLiteral("temperature_2m_max")).toArray();
    const QJsonArray lows = daily.value(QStringLiteral("temperature_2m_min")).toArray();
    for (qsizetype i = 0; i < dates.size(); ++i)
        f.days << Day{QDate::fromString(dates.at(i).toString(), Qt::ISODate), codes.at(i).toInt(), highs.at(i).toDouble(), lows.at(i).toDouble()};
    return f;
}

void WeatherSource::query(const QString &text)
{
    m_query = text.simplified();
    const QString q = m_query.toLower();
    static const QRegularExpression before(QStringLiteral("^(?:weather|forecast)(?: (?:in|for|at))?(?: (.+))?$"));
    static const QRegularExpression after(QStringLiteral("^(.+) (?:weather|forecast)$"));
    QString place;
    bool asked = false;
    if (const auto m = before.match(q); m.hasMatch())
        place = m.captured(1), asked = true;
    else if (const auto m = after.match(q); m.hasMatch())
        place = m.captured(1), asked = true;
    if (!asked) {
        Q_EMIT resultsReady({});
        return;
    }
    m_place = place.isEmpty() ? defaultPlace() : place;
    m_error.clear();
    const auto cached = m_forecasts.constFind(m_place.toLower());
    if (cached == m_forecasts.constEnd() || cached->first.secsTo(QDateTime::currentDateTime()) > CacheMinutes * 60) {
        if (const auto known = m_places.constFind(m_place.toLower()); known != m_places.constEnd())
            fetch(*known);
        else
            lookUp(m_place);
    }
    publish();
}

void WeatherSource::lookUp(const QString &place)
{
    QUrl url(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
    url.setQuery(QUrlQuery{{QStringLiteral("name"), place}, {QStringLiteral("count"), QStringLiteral("1")},
                           {QStringLiteral("language"), QLocale().name().section(u'_', 0, 0)}});
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, place] {
        reply->deleteLater();
        const QJsonArray results = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("results")).toArray();
        if (reply->error() != QNetworkReply::NoError || results.isEmpty()) {
            m_error = reply->error() != QNetworkReply::NoError ? tr("Could not reach the weather service") : tr("No place called “%1”").arg(place);
            publish();
            return;
        }
        const QJsonObject r = results.first().toObject();
        Place p{r.value(QStringLiteral("name")).toString(), r.value(QStringLiteral("latitude")).toDouble(), r.value(QStringLiteral("longitude")).toDouble()};
        const QString country = r.value(QStringLiteral("country")).toString();
        if (!country.isEmpty())
            p.name += QStringLiteral(", ") + country;
        m_places.insert(place.toLower(), p);
        fetch(p);
    });
}

void WeatherSource::fetch(const Place &place)
{
    QUrl url(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
    QUrlQuery query{{QStringLiteral("latitude"), QString::number(place.latitude)},
                    {QStringLiteral("longitude"), QString::number(place.longitude)},
                    {QStringLiteral("current"), QStringLiteral("temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day")},
                    {QStringLiteral("daily"), QStringLiteral("weather_code,temperature_2m_max,temperature_2m_min")},
                    {QStringLiteral("forecast_days"), QStringLiteral("4")},
                    {QStringLiteral("timezone"), QStringLiteral("auto")}};
    if (imperial()) {
        query.addQueryItem(QStringLiteral("temperature_unit"), QStringLiteral("fahrenheit"));
        query.addQueryItem(QStringLiteral("wind_speed_unit"), QStringLiteral("mph"));
    }
    url.setQuery(query);
    const QString key = m_place.toLower();
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, place] {
        reply->deleteLater();
        const auto forecast = reply->error() == QNetworkReply::NoError ? parse(reply->readAll()) : std::nullopt;
        if (!forecast) {
            m_error = tr("Could not reach the weather service");
        } else {
            m_forecasts.insert(key, {QDateTime::currentDateTime(), *forecast});
            m_places.insert(key, place);
        }
        publish();
    });
}

void WeatherSource::publish()
{
    if (m_place.isEmpty())
        return;
    QList<Item> items;
    const QString key = m_place.toLower();
    const auto cached = m_forecasts.constFind(key);
    if (cached == m_forecasts.constEnd()) {
        Item wait;
        wait.id = QStringLiteral("answer:weather");
        wait.title = m_error.isEmpty() ? tr("Looking up the weather…") : m_error;
        wait.subtitle = m_place;
        wait.icon = QIcon::fromTheme(QStringLiteral("weather-none-available"));
        wait.score = 115;
        items << wait;
        Q_EMIT resultsReady(items);
        return;
    }
    const Forecast &f = cached->second;
    const QString placeName = m_places.value(key).name.isEmpty() ? m_place : m_places.value(key).name;
    const QString windUnit = imperial() ? QStringLiteral("mph") : QStringLiteral("km/h");
    Launcher *l = launcher();

    Item now;
    now.id = QStringLiteral("answer:weather");
    now.title = degrees(f.temperature) + QStringLiteral("  ") + describe(f.code);
    now.subtitle = tr("%1 · feels like %2 · wind %3 %4 · humidity %5%")
                       .arg(placeName, degrees(f.feelsLike))
                       .arg(qRound(f.wind))
                       .arg(windUnit)
                       .arg(f.humidity);
    now.kind = tr("Weather");
    now.icon = QIcon::fromTheme(iconName(f.code, f.day));
    now.score = 115;
    const QString summary = placeName + QStringLiteral(": ") + now.title.simplified();
    now.actions << Action{tr("Copy"), {}, {}, [l, summary] { l->copyText(summary); }};
    const Place place = m_places.value(key);
    const QUrl more(QStringLiteral("https://open-meteo.com/en/docs#latitude=%1&longitude=%2").arg(place.latitude).arg(place.longitude));
    now.actions << Action{tr("Open Open-Meteo"), QStringLiteral("internet-web-browser"), {}, [more] { (new KIO::OpenUrlJob(more))->start(); }};
    items << now;

    for (int i = 1; i < f.days.size(); ++i) { // today is the card above
        const Day &d = f.days.at(i);
        Item day;
        day.id = QStringLiteral("weather-day:%1").arg(i);
        day.title = i == 1 ? tr("Tomorrow") : QLocale().dayName(d.date.dayOfWeek());
        day.subtitle = describe(d.code);
        day.kind = QStringLiteral("%1 / %2").arg(degrees(d.high), degrees(d.low)); // drawn as high and low by the row
        day.icon = QIcon::fromTheme(iconName(d.code, true));
        day.score = 114 - i * 0.1;
        items << day;
    }
    Q_EMIT resultsReady(items);
}
