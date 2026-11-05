#include "calendarsource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KIO/OpenUrlJob>
#include <KSharedConfig>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>

namespace
{

constexpr int RefreshMinutes = 30;
constexpr int AgendaDays = 7;   // "agenda" lists this far ahead
constexpr int SearchDays = 30;  // searching by title looks this far ahead
constexpr int SoonMinutes = 180; // the start screen shows events this close

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Calendar"));
}

QString dayText(const QDate &date)
{
    const QDate today = QDate::currentDate();
    if (date == today)
        return QObject::tr("Today");
    if (date == today.addDays(1))
        return QObject::tr("Tomorrow");
    return QLocale().toString(date, QStringLiteral("dddd, MMM d"));
}

QString timeText(const Ics::Event &e)
{
    if (e.allDay)
        return dayText(e.start.date()) + QStringLiteral(" · ") + QObject::tr("all day");
    const QLocale locale;
    const QDateTime start = e.start.toLocalTime(), end = e.end.toLocalTime();
    return dayText(start.date()) + QStringLiteral(" · ") + locale.toString(start.time(), QLocale::ShortFormat) + QStringLiteral("–")
        + locale.toString(end.time(), QLocale::ShortFormat);
}

} // namespace

CalendarSource::CalendarSource(Launcher *launcher)
    : Source(launcher)
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/lumen/calendars"));
    load();
    m_timer.setInterval(RefreshMinutes * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &CalendarSource::refresh);
    m_timer.start();
    QTimer::singleShot(5000, this, &CalendarSource::refresh); // after start-up, quietly
}

QStringList CalendarSource::feeds()
{
    return config().readEntry("Feeds", QStringList());
}

void CalendarSource::setFeeds(const QStringList &list)
{
    KConfigGroup g = config();
    g.writeEntry("Feeds", list);
    g.sync();
}

QString CalendarSource::cacheFile(const QString &feed)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/lumen/calendars/")
        + QString::fromLatin1(QCryptographicHash::hash(feed.toUtf8(), QCryptographicHash::Sha1).toHex()) + QStringLiteral(".ics");
}

void CalendarSource::sessionStarted()
{
    if (!m_lastRefresh.isValid() || m_lastRefresh.secsTo(QDateTime::currentDateTime()) > RefreshMinutes * 60)
        refresh();
}

void CalendarSource::refresh()
{
    m_lastRefresh = QDateTime::currentDateTime();
    for (const QString &feed : feeds()) {
        QString address = feed.trimmed();
        if (address.startsWith(QLatin1String("webcal"))) // webcal:// and webcals:// are https
            address = QStringLiteral("https") + address.mid(address.indexOf(QLatin1String("://")));
        const QUrl url = QUrl::fromUserInput(address);
        if (url.isLocalFile()) {
            QFile::remove(cacheFile(feed));
            QFile::copy(url.toLocalFile(), cacheFile(feed));
            continue;
        }
        QNetworkReply *reply = m_network.get(QNetworkRequest(url));
        connect(reply, &QNetworkReply::finished, this, [this, reply, feed] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return; // keep the cached copy
            QSaveFile f(cacheFile(feed));
            if (f.open(QIODevice::WriteOnly)) {
                f.write(reply->readAll());
                f.commit();
            }
            load();
        });
    }
    load();
}

void CalendarSource::load()
{
    const QDateTime now = QDateTime::currentDateTime();
    m_events.clear();
    for (const QString &feed : feeds()) {
        QFile f(cacheFile(feed));
        if (f.open(QIODevice::ReadOnly))
            m_events << Ics::parse(f.readAll(), now.addDays(-1), now.addDays(SearchDays));
    }
    std::sort(m_events.begin(), m_events.end(), [](const Ics::Event &a, const Ics::Event &b) { return a.start < b.start; });
    if (launcher()->isVisible())
        query(m_query);
}

Item CalendarSource::itemFor(const Ics::Event &e) const
{
    Item item;
    item.id = QStringLiteral("event:") + e.title + e.start.toString(Qt::ISODate);
    item.title = e.title.isEmpty() ? tr("(No title)") : e.title;
    item.subtitle = timeText(e) + (e.location.isEmpty() ? QString() : QStringLiteral(" · ") + e.location.section(u'\n', 0, 0));
    const QDateTime now = QDateTime::currentDateTime();
    item.kind = e.start <= now && e.end > now ? tr("Now") : tr("Calendar");
    item.icon = QIcon::fromTheme(QStringLiteral("view-calendar-day"));
    item.previewText = e.title + u'\n' + timeText(e) + (e.location.isEmpty() ? QString() : u'\n' + e.location)
        + (e.description.isEmpty() ? QString() : QStringLiteral("\n\n") + e.description);
    Launcher *l = launcher();
    const QString link = e.meetingLink();
    if (!link.isEmpty())
        item.actions << Action{tr("Join Meeting"), QStringLiteral("camera-video"), {}, [link] { (new KIO::OpenUrlJob(QUrl(link)))->start(); }};
    if (!e.url.isEmpty() && e.url != link) {
        const QUrl url(e.url);
        item.actions << Action{tr("Open Event Link"), QStringLiteral("internet-web-browser"), {}, [url] { (new KIO::OpenUrlJob(url))->start(); }};
    }
    const QString details = item.previewText;
    item.actions << Action{tr("Copy Details"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                           [l, details] { l->copyText(details); }};
    return item;
}

void CalendarSource::query(const QString &text)
{
    m_query = text.trimmed();
    QList<Item> items;
    const QString q = m_query.toLower();
    if (q.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    static const QStringList agendaWords{QStringLiteral("agenda"), QStringLiteral("calendar"), QStringLiteral("events"), QStringLiteral("meetings"),
                                         QStringLiteral("schedule"), QStringLiteral("today"), QStringLiteral("upcoming")};
    const bool agenda = std::any_of(agendaWords.cbegin(), agendaWords.cend(), [&](const QString &w) { return fuzzyScore(q, w) >= 80; });
    int i = 0;
    for (const Ics::Event &e : std::as_const(m_events)) {
        if (e.end <= now)
            continue;
        const bool today = q == QLatin1String("today");
        const bool inAgenda = agenda && (today ? e.start.date() <= now.date() : e.start <= now.addDays(AgendaDays));
        const qreal score = fuzzyScore(m_query, e.title);
        if (!inAgenda && score < 40)
            continue;
        Item item = itemFor(e);
        item.score = inAgenda ? 95 - i * 0.01 : score; // agenda: in time order
        items << item;
        ++i;
    }
    if (agenda && items.isEmpty()) {
        Item none;
        none.title = feeds().isEmpty() ? tr("No calendars yet") : tr("Nothing coming up");
        none.subtitle = feeds().isEmpty() ? tr("Add an .ics address in Lumen Settings → Calendar & Weather") : tr("Your next %1 days are free").arg(AgendaDays);
        none.icon = QIcon::fromTheme(QStringLiteral("view-calendar"));
        none.score = 90;
        Launcher *l = launcher();
        none.actions << Action{tr("Open Settings"), {}, {}, [l] { l->openSettings(Launcher::CalendarTab, false); }};
        items << none;
    }
    Q_EMIT resultsReady(items);
}

QList<Item> CalendarSource::startItems() const
{
    const QDateTime now = QDateTime::currentDateTime();
    for (const Ics::Event &e : m_events) {
        if (e.allDay || e.end <= now)
            continue;
        if (now.secsTo(e.start) > SoonMinutes * 60)
            break;
        Item item = itemFor(e);
        const qint64 minutes = now.secsTo(e.start) / 60;
        item.kind = e.start <= now ? tr("Now") : minutes < 60 ? tr("In %1 min").arg(minutes + 1) : tr("Soon");
        item.score = 99;
        return {item};
    }
    return {};
}
