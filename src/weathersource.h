#pragma once

#include "source.h"

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>

// "weather", "weather tokyo", "tokyo weather", "forecast": current conditions
// and the next days from Open-Meteo (free, no account), for the place typed
// or the one in the settings (by default the city of the system time zone).
// Answers are cached for 15 minutes.
class WeatherSource : public Source
{
    Q_OBJECT
public:
    explicit WeatherSource(Launcher *launcher);

    void query(const QString &text) override;

    struct Day {
        QDate date;
        int code = 0; // WMO weather code
        double high = 0, low = 0;
    };
    struct Forecast {
        double temperature = 0, feelsLike = 0, wind = 0;
        int humidity = 0;
        int code = 0;
        bool day = true;
        QList<Day> days;
    };
    // Reads Open-Meteo's forecast JSON (tested without the network).
    static std::optional<Forecast> parse(const QByteArray &json);
    static QString describe(int code);
    static QString iconName(int code, bool day);

    static QString defaultPlace();
    static bool imperial();

private:
    struct Place {
        QString name;
        double latitude = 0, longitude = 0;
    };
    void lookUp(const QString &place);
    void fetch(const Place &place);
    void publish();

    QNetworkAccessManager m_network;
    QString m_query;
    QString m_place;              // what the current query asks for
    QHash<QString, Place> m_places; // looked-up names
    QHash<QString, std::pair<QDateTime, Forecast>> m_forecasts; // by place name
    QString m_error;
};
