#pragma once

#include "ics.h"
#include "source.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QTimer>

// Upcoming events from the user's calendar feeds (Settings → Calendar &
// Weather: .ics files or links, such as a Google Calendar "secret address in
// iCal format"). "agenda", "today", "meetings" or words from an event list
// them; the next event within three hours shows on the start screen, with
// "Join Meeting" when it has a Meet/Zoom/Teams link. Feeds are cached and
// refreshed every half hour.
class CalendarSource : public Source
{
    Q_OBJECT
public:
    explicit CalendarSource(Launcher *launcher);

    void query(const QString &text) override;
    void sessionStarted() override;
    QList<Item> startItems() const;

    static QStringList feeds();
    static void setFeeds(const QStringList &feeds);
    void refresh();

private:
    void load(); // parses the cached feeds
    Item itemFor(const Ics::Event &event) const;
    static QString cacheFile(const QString &feed);

    QNetworkAccessManager m_network;
    QList<Ics::Event> m_events;
    QDateTime m_lastRefresh;
    QTimer m_timer;
    QString m_query;
};
