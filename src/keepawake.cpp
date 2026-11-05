#include "keepawake.h"
#include "fuzzy.h"
#include "launcher.h"
#include "plural.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>

namespace
{

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("KeepAwake"));
}

// Inhibit(app, reason) → cookie, on the power (sleep) and screen saver
// (dimming, locking) interfaces.
uint inhibit(const QString &service, const QString &path, const QString &interface)
{
    auto msg = QDBusMessage::createMethodCall(service, path, interface, QStringLiteral("Inhibit"));
    msg.setArguments({QStringLiteral("Lumen"), QObject::tr("Keep Awake is on")});
    const QDBusReply<uint> cookie = QDBusConnection::sessionBus().call(msg, QDBus::Block, 1000);
    return cookie.isValid() ? cookie.value() : 0;
}

void uninhibit(const QString &service, const QString &path, const QString &interface, uint cookie)
{
    if (!cookie)
        return;
    auto msg = QDBusMessage::createMethodCall(service, path, interface, QStringLiteral("UnInhibit"));
    msg.setArguments({cookie});
    QDBusConnection::sessionBus().call(msg, QDBus::Block, 1000);
}

const QString PowerService = QStringLiteral("org.freedesktop.PowerManagement");
const QString PowerPath = QStringLiteral("/org/freedesktop/PowerManagement/Inhibit");
const QString PowerInterface = QStringLiteral("org.freedesktop.PowerManagement.Inhibit");
const QString ScreenService = QStringLiteral("org.freedesktop.ScreenSaver");
const QString ScreenPath = QStringLiteral("/ScreenSaver");
const QString ScreenInterface = QStringLiteral("org.freedesktop.ScreenSaver");

} // namespace

KeepAwake::KeepAwake(Launcher *launcher)
    : Source(launcher)
{
    m_end.setSingleShot(true);
    connect(&m_end, &QTimer::timeout, this, [this] {
        stop();
        this->launcher()->notify(tr("Keep Awake is off"));
    });
    // On when Lumen last stopped, and not yet over: on again.
    const KConfigGroup g = config();
    if (g.readEntry("On", false)) {
        const QDateTime until = QDateTime::fromString(g.readEntry("Until"), Qt::ISODate);
        if (!until.isValid() || until > QDateTime::currentDateTime()) {
            m_on = true;
            m_until = until;
            QTimer::singleShot(0, this, &KeepAwake::hold);
        }
    }
}

KeepAwake::~KeepAwake()
{
    release();
}

void KeepAwake::hold()
{
    if (!m_sleepCookie)
        m_sleepCookie = inhibit(PowerService, PowerPath, PowerInterface);
    if (!m_screenCookie)
        m_screenCookie = inhibit(ScreenService, ScreenPath, ScreenInterface);
    if (m_until.isValid())
        m_end.start(int(std::max<qint64>(0, QDateTime::currentDateTime().msecsTo(m_until))));
}

void KeepAwake::release()
{
    uninhibit(PowerService, PowerPath, PowerInterface, m_sleepCookie);
    uninhibit(ScreenService, ScreenPath, ScreenInterface, m_screenCookie);
    m_sleepCookie = m_screenCookie = 0;
}

void KeepAwake::start(int minutes)
{
    m_on = true;
    m_until = minutes > 0 ? QDateTime::currentDateTime().addSecs(minutes * 60) : QDateTime();
    m_end.stop();
    hold();
    KConfigGroup g = config();
    g.writeEntry("On", true);
    g.writeEntry("Until", m_until.isValid() ? m_until.toString(Qt::ISODate) : QString());
    g.sync();
    launcher()->notify(minutes > 0 ? plural(minutes >= 60 && minutes % 60 == 0 ? minutes / 60 : minutes,
                                            minutes >= 60 && minutes % 60 == 0 ? tr("Awake for %1 hour") : tr("Awake for %1 minute"),
                                            minutes >= 60 && minutes % 60 == 0 ? tr("Awake for %1 hours") : tr("Awake for %1 minutes"))
                                   : tr("Awake until you stop it"));
}

void KeepAwake::stop()
{
    m_on = false;
    m_until = {};
    m_end.stop();
    release();
    KConfigGroup g = config();
    g.writeEntry("On", false);
    g.deleteEntry("Until");
    g.sync();
}

Item KeepAwake::statusItem() const
{
    Item item;
    item.id = QStringLiteral("lumen:keep-awake");
    if (m_until.isValid()) {
        const qint64 left = std::max<qint64>(1, (QDateTime::currentDateTime().secsTo(m_until) + 59) / 60);
        item.title = left >= 60 ? tr("Keeping Awake · %1 h %2 min left").arg(left / 60).arg(left % 60) : tr("Keeping Awake · %1 min left").arg(left);
    } else {
        item.title = tr("Keeping Awake");
    }
    item.subtitle = tr("No sleep, no screen lock");
    item.kind = tr("Awake");
    item.icon = QIcon::fromTheme(QStringLiteral("system-suspend-inhibited"), QIcon::fromTheme(QStringLiteral("caffeine-cup-full")));
    auto self = const_cast<KeepAwake *>(this);
    item.actions << Action{tr("Stop Keeping Awake"), {}, {}, [self] {
        self->stop();
        self->launcher()->notify(tr("Keep Awake is off"));
    }, true};
    return item;
}

QList<Item> KeepAwake::startItems() const
{
    if (!m_on)
        return {};
    Item item = statusItem();
    item.score = 1000;
    return {item};
}

void KeepAwake::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    const qreal score = q.isEmpty() ? 0
                                    : std::max({fuzzyScore(q, QStringLiteral("Keep Awake")), fuzzyScore(q, QStringLiteral("caffeinate")),
                                                fuzzyScore(q, QStringLiteral("coffee")), fuzzyScore(q, QStringLiteral("stay awake")),
                                                fuzzyScore(q, QStringLiteral("don't sleep")), fuzzyScore(q, QStringLiteral("no sleep"))});
    if (score >= 60) {
        if (m_on) {
            Item item = statusItem();
            item.score = score + 6;
            items << item;
        }
        auto self = this;
        int n = 0;
        for (const auto &[minutes, title] : QList<std::pair<int, QString>>{{30, tr("Keep Awake for 30 Minutes")},
                                                                           {60, tr("Keep Awake for 1 Hour")},
                                                                           {120, tr("Keep Awake for 2 Hours")},
                                                                           {0, tr("Keep Awake Until I Stop It")}}) {
            Item item;
            item.id = QStringLiteral("lumen:keep-awake-%1").arg(minutes);
            item.title = title;
            item.subtitle = tr("The computer neither sleeps nor locks its screen");
            item.kind = tr("Command");
            item.icon = QIcon::fromTheme(QStringLiteral("system-suspend-inhibited"), QIcon::fromTheme(QStringLiteral("caffeine-cup-full")));
            item.score = score + 5 - n++ * 0.1;
            const int m = minutes;
            item.actions << Action{tr("Start"), {}, {}, [self, m] { self->start(m); }};
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}
