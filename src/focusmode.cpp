#include "focusmode.h"
#include "plural.h"
#include "fuzzy.h"
#include "launcher.h"
#include "timekeeper.h"
#include "windowtiling.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>

namespace
{

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Focus"));
}

// Browsers, whose window titles name the site being shown.
const QStringList &browsers()
{
    static const QStringList list{QStringLiteral("firefox"), QStringLiteral("librewolf"), QStringLiteral("zen"), QStringLiteral("floorp"),
                                  QStringLiteral("waterfox"), QStringLiteral("chromium"), QStringLiteral("google-chrome"),
                                  QStringLiteral("brave-browser"), QStringLiteral("vivaldi-stable"), QStringLiteral("opera"),
                                  QStringLiteral("microsoft-edge"), QStringLiteral("org.kde.falkon"), QStringLiteral("org.kde.konqueror"),
                                  QStringLiteral("org.gnome.epiphany")};
    return list;
}

QDBusMessage notifications(const QString &method, const QVariantList &args)
{
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                                              QStringLiteral("org.freedesktop.Notifications"), method);
    msg.setArguments(args);
    return msg;
}

} // namespace

FocusMode::FocusMode(Launcher *launcher, Timekeeper *timekeeper)
    : Source(launcher)
    , m_timekeeper(timekeeper)
{
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Focus"), this, QDBusConnection::ExportScriptableSlots);
    m_end.setSingleShot(true);
    connect(&m_end, &QTimer::timeout, this, [this] { stop(true); });
    // A session that was running when Lumen stopped goes on.
    const KConfigGroup g = config();
    const QDateTime until = QDateTime::fromString(g.readEntry("Until"), Qt::ISODate);
    if (until.isValid() && until > QDateTime::currentDateTime()) {
        m_until = until;
        m_goal = g.readEntry("Goal");
        m_timerId = g.readEntry("Timer", 0);
        QTimer::singleShot(0, this, &FocusMode::enforce); // once the D-Bus name is ours
    }
}

FocusMode::~FocusMode()
{
    if (m_inhibit)
        QDBusConnection::sessionBus().call(notifications(QStringLiteral("UnInhibit"), {m_inhibit}), QDBus::NoBlock);
}

QStringList FocusMode::blockedApps()
{
    return config().readEntry("Apps", QStringList{QStringLiteral("discord"), QStringLiteral("org.telegram.desktop"), QStringLiteral("steam"),
                                                   QStringLiteral("com.discordapp.Discord"), QStringLiteral("slack")});
}

QStringList FocusMode::blockedSites()
{
    return config().readEntry("Sites", QStringList{QStringLiteral("youtube.com"), QStringLiteral("reddit.com"), QStringLiteral("x.com"),
                                                    QStringLiteral("twitter.com"), QStringLiteral("instagram.com"), QStringLiteral("facebook.com"),
                                                    QStringLiteral("tiktok.com"), QStringLiteral("netflix.com"), QStringLiteral("twitch.tv")});
}

int FocusMode::defaultMinutes()
{
    return config().readEntry("Minutes", 25);
}

bool FocusMode::doNotDisturb()
{
    return config().readEntry("DoNotDisturb", true);
}

QString FocusMode::script(const QStringList &apps, const QStringList &sites)
{
    // A site is recognized by its name in a browser's title ("YouTube" for
    // youtube.com); names too short to be telling ("x") by the domain itself.
    QJsonArray siteWords;
    for (const QString &site : sites) {
        const QString domain = site.trimmed().toLower().remove(QStringLiteral("https://")).remove(QStringLiteral("www.")).section(u'/', 0, 0);
        const QString name = domain.section(u'.', 0, 0);
        if (!domain.isEmpty())
            siteWords << (name.size() >= 4 ? name : domain);
    }
    QJsonArray appIds;
    for (const QString &app : apps)
        appIds << app.toLower().remove(QStringLiteral(".desktop"));
    return QStringLiteral(R"js(
const apps = %1, sites = %2, browsers = %3;
function distraction(w) {
    if (!w || !w.normalWindow)
        return "";
    const app = (w.desktopFileName || "").toLowerCase(), cls = (w.resourceClass || "").toLowerCase();
    if (apps.includes(app) || apps.includes(cls))
        return w.caption.split(" — ").pop().split(" - ").pop() || app;
    if (browsers.includes(app) || browsers.includes(cls)) {
        const title = w.caption.toLowerCase();
        for (const s of sites)
            if (title.includes(s))
                return s;
    }
    return "";
}
function check(w) {
    const what = distraction(w);
    if (what) {
        w.minimized = true;
        callDBus("io.github.lumen", "/Focus", "io.github.lumen.Focus", "blocked", what);
    }
}
function watch(w) {
    // A browser changing tabs changes its title.
    w.captionChanged.connect(() => { if (w === workspace.activeWindow) check(w); });
}
for (const w of workspace.windowList()) {
    watch(w);
    if (distraction(w))
        w.minimized = true;
}
workspace.windowAdded.connect(w => { watch(w); check(w); });
workspace.windowActivated.connect(check);
)js")
        .arg(QString::fromUtf8(QJsonDocument(appIds).toJson(QJsonDocument::Compact)),
             QString::fromUtf8(QJsonDocument(siteWords).toJson(QJsonDocument::Compact)),
             QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(browsers())).toJson(QJsonDocument::Compact)));
}

void FocusMode::start(int minutes, const QString &goal)
{
    if (isActive())
        stop(false);
    minutes = std::clamp(minutes, 1, 12 * 60);
    m_until = QDateTime::currentDateTime().addSecs(minutes * 60);
    m_goal = goal.trimmed();
    // The countdown in the corner badge.
    m_timekeeper->startTimer(minutes * 60, m_goal.isEmpty() ? tr("Focus") : m_goal);
    m_timerId = m_timekeeper->timers().isEmpty() ? 0 : m_timekeeper->timers().last().id;
    KConfigGroup g = config();
    g.writeEntry("Until", m_until.toString(Qt::ISODate));
    g.writeEntry("Goal", m_goal);
    g.writeEntry("Timer", m_timerId);
    g.sync();
    enforce();
    launcher()->notify(plural(minutes, tr("Focusing for %1 minute"), tr("Focusing for %1 minutes")));
}

void FocusMode::extend(int minutes)
{
    if (!isActive())
        return;
    m_until = m_until.addSecs(minutes * 60);
    m_timekeeper->addTime(m_timerId, minutes * 60);
    KConfigGroup g = config();
    g.writeEntry("Until", m_until.toString(Qt::ISODate));
    g.sync();
    m_end.start(int(std::max<qint64>(0, QDateTime::currentDateTime().msecsTo(m_until))));
}

void FocusMode::enforce()
{
    m_end.start(int(std::max<qint64>(0, QDateTime::currentDateTime().msecsTo(m_until))));
    WindowTiling::loadScript(QStringLiteral("focus"), script(blockedApps(), blockedSites()));
    if (doNotDisturb() && !m_inhibit) {
        const QDBusReply<uint> cookie = QDBusConnection::sessionBus().call(
            notifications(QStringLiteral("Inhibit"), {QStringLiteral("io.github.lumen"), tr("Focus session"), QVariantMap()}), QDBus::Block, 1000);
        if (cookie.isValid())
            m_inhibit = cookie.value();
    }
}

void FocusMode::release()
{
    WindowTiling::unloadScript(QStringLiteral("focus"));
    if (m_inhibit) {
        QDBusConnection::sessionBus().call(notifications(QStringLiteral("UnInhibit"), {m_inhibit}), QDBus::Block, 1000);
        m_inhibit = 0;
    }
}

void FocusMode::stop(bool finished)
{
    if (!isActive())
        return;
    m_end.stop();
    release();
    if (!finished)
        m_timekeeper->stopTimer(m_timerId); // a finished one has rung by itself
    m_until = {};
    m_goal.clear();
    m_timerId = 0;
    KConfigGroup g = config();
    g.deleteEntry("Until");
    g.deleteEntry("Goal");
    g.deleteEntry("Timer");
    g.sync();
    launcher()->notify(finished ? tr("Focus session done") : tr("Focus session ended"));
}

void FocusMode::blocked(const QString &what)
{
    // A window can be activated several times in a row; one notice is enough.
    const QDateTime now = QDateTime::currentDateTime();
    if (m_lastNotice.isValid() && m_lastNotice.secsTo(now) < 4)
        return;
    m_lastNotice = now;
    const qint64 left = std::max<qint64>(1, (now.secsTo(m_until) + 59) / 60);
    launcher()->notify(plural(left, tr("Focus: %2 is hidden for %1 more minute"), tr("Focus: %2 is hidden for %1 more minutes")).arg(what));
}

Item FocusMode::sessionItem() const
{
    Item item;
    item.id = QStringLiteral("lumen:focus");
    const qint64 left = std::max<qint64>(0, (QDateTime::currentDateTime().secsTo(m_until) + 59) / 60);
    item.title = tr("Focus · %1 min left").arg(left);
    item.subtitle = m_goal.isEmpty() ? tr("Distractions are hidden, notifications wait") : m_goal;
    item.kind = tr("Focus");
    item.icon = QIcon::fromTheme(QStringLiteral("view-focus"), QIcon::fromTheme(QStringLiteral("chronometer")));
    auto self = const_cast<FocusMode *>(this);
    item.actions << Action{tr("End Focus Session"), {}, {}, [self] { self->stop(false); }, true};
    item.actions << Action{tr("10 More Minutes"), QStringLiteral("list-add"), QKeySequence(Qt::CTRL | Qt::Key_Plus), [self] { self->extend(10); }, true};
    return item;
}

QList<Item> FocusMode::startItems() const
{
    if (!isActive())
        return {};
    Item item = sessionItem();
    item.score = 1000;
    return {item};
}

void FocusMode::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    const qreal score = q.isEmpty() ? 0
                                    : std::max({fuzzyScore(q, QStringLiteral("Focus")), fuzzyScore(q, QStringLiteral("Start Focus Session")),
                                                fuzzyScore(q, QStringLiteral("deep work")), fuzzyScore(q, QStringLiteral("concentrate")),
                                                fuzzyScore(q, QStringLiteral("block distractions"))});
    if (score >= 50) {
        Launcher *l = launcher();
        auto self = this;
        if (isActive()) {
            Item item = sessionItem();
            item.score = score + 5;
            items << item;
        } else {
            Item item;
            item.id = QStringLiteral("lumen:focus-start");
            item.title = tr("Start Focus Session");
            item.subtitle = tr("Hide distracting apps and sites, hold notifications back");
            item.kind = tr("Focus");
            item.icon = QIcon::fromTheme(QStringLiteral("view-focus"), QIcon::fromTheme(QStringLiteral("chronometer")));
            item.score = score + 5;
            const Item shown = item;
            Argument minutes;
            minutes.name = QStringLiteral("minutes");
            minutes.placeholder = tr("Minutes");
            minutes.defaultValue = QString::number(defaultMinutes());
            minutes.optional = true;
            Argument goal;
            goal.name = QStringLiteral("goal");
            goal.placeholder = tr("Goal");
            goal.optional = true;
            Action start{tr("Start"), {}, {}, [l, self, shown, minutes, goal] {
                l->askArguments(shown, {minutes, goal}, {}, [l, self](const QStringList &values) {
                    self->start(values.value(0).toInt() > 0 ? values.value(0).toInt() : defaultMinutes(), values.value(1));
                    l->hideLauncher();
                });
            }, true};
            start.asksArguments = true;
            item.actions << start;
            item.actions << Action{tr("Focus Settings"), QStringLiteral("configure"), QKeySequence(Qt::CTRL | Qt::Key_Comma),
                                   [l] { l->openSettings(Launcher::FocusTab, false); }};
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}
