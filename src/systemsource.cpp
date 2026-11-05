#include "systemsource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <Solid/Device>
#include <Solid/OpticalDrive>
#include <Solid/StorageAccess>
#include <Solid/StorageDrive>
#include <Solid/StorageVolume>

#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusVariant>
#include <QPalette>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace
{

const QString MprisPrefix = QStringLiteral("org.mpris.MediaPlayer2.");
const QString MprisPath = QStringLiteral("/org/mpris/MediaPlayer2");

// Runs a program and hands its output to done (on success).
void readOutput(QObject *context, const QString &program, const QStringList &args, std::function<void(const QString &)> done)
{
    auto *proc = new QProcess(context);
    QObject::connect(proc, &QProcess::finished, context, [proc, done](int code) {
        if (code == 0)
            done(QString::fromUtf8(proc->readAllStandardOutput()));
        proc->deleteLater();
    });
    proc->start(program, args);
}

QVariant dbusProperty(const QString &service, const QString &path, const QString &interface, const QString &name)
{
    auto msg = QDBusMessage::createMethodCall(service, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    msg.setArguments({interface, name});
    const QDBusReply<QDBusVariant> reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 300);
    return reply.isValid() ? reply.value().variant() : QVariant();
}

bool paletteIsDark()
{
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

// Scores a command against the query by its title and keywords.
qreal match(const QString &query, const QString &title, const QStringList &keywords)
{
    qreal best = fuzzyScore(query, title);
    for (const QString &k : keywords)
        best = std::max(best, fuzzyScore(query, k) * 0.85);
    return best;
}

} // namespace

SystemSource::SystemSource(Launcher *launcher)
    : Source(launcher)
{
}

void SystemSource::sessionStarted()
{
    // After the launcher has appeared: asking players over D-Bus can be slow.
    QTimer::singleShot(0, this, &SystemSource::readState);
}

void SystemSource::readState()
{
    readOutput(this, QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("radio"), QStringLiteral("wifi")}, [this](const QString &out) {
        m_wifi = out.trimmed() == QLatin1String("enabled");
        m_wifiKnown = true;
    });
    readOutput(this, QStringLiteral("bluetoothctl"), {QStringLiteral("show")}, [this](const QString &out) {
        m_bluetoothKnown = out.contains(QLatin1String("Powered:"));
        m_bluetooth = out.contains(QLatin1String("Powered: yes"));
    });
    readOutput(this, QStringLiteral("wpctl"), {QStringLiteral("get-volume"), QStringLiteral("@DEFAULT_AUDIO_SINK@")}, [this](const QString &out) {
        static const QRegularExpression re(QStringLiteral("Volume: ([\\d.]+)"));
        const auto m = re.match(out);
        m_volume = m.hasMatch() ? qRound(m.captured(1).toDouble() * 100) : -1;
        m_muted = out.contains(QLatin1String("MUTED"));
    });
    readPlayers();
    query(m_lastQuery); // the start screen shows what is playing
}

void SystemSource::readPlayers()
{
    m_players.clear();
    const QStringList services = QDBusConnection::sessionBus().interface()->registeredServiceNames();
    for (const QString &service : services) {
        if (!service.startsWith(MprisPrefix))
            continue;
        Player p;
        p.service = service;
        p.name = dbusProperty(service, MprisPath, QStringLiteral("org.mpris.MediaPlayer2"), QStringLiteral("Identity")).toString();
        p.playing = dbusProperty(service, MprisPath, QStringLiteral("org.mpris.MediaPlayer2.Player"), QStringLiteral("PlaybackStatus")).toString()
            == QLatin1String("Playing");
        const QVariant metaVariant = dbusProperty(service, MprisPath, QStringLiteral("org.mpris.MediaPlayer2.Player"), QStringLiteral("Metadata"));
        QVariantMap meta;
        metaVariant.value<QDBusArgument>() >> meta;
        p.title = meta.value(QStringLiteral("xesam:title")).toString();
        p.artist = meta.value(QStringLiteral("xesam:artist")).toStringList().join(QStringLiteral(", "));
        if (p.name.isEmpty())
            p.name = service.mid(MprisPrefix.size()).section(u'.', 0, 0);
        m_players << p;
    }
    // Playing players first.
    std::stable_sort(m_players.begin(), m_players.end(), [](const Player &a, const Player &b) { return a.playing && !b.playing; });
}

void SystemSource::run(const QString &program, const QStringList &args, const QString &message, bool refresh)
{
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc, message, refresh](int code) {
        launcher()->notify(code == 0 ? message : tr("That did not work (%1)").arg(proc->program()));
        if (refresh) {
            readState();
            QTimer::singleShot(300, this, [this] { query(m_lastQuery); });
        }
        proc->deleteLater();
    });
    proc->start(program, args);
}

void SystemSource::mediaCall(const QString &service, const QString &method)
{
    QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(service, MprisPath, QStringLiteral("org.mpris.MediaPlayer2.Player"), method),
                                       QDBus::NoBlock);
    QTimer::singleShot(400, this, [this] {
        readPlayers();
        query(m_lastQuery);
    });
}

void SystemSource::setDoNotDisturb(bool on)
{
    auto bus = QDBusConnection::sessionBus();
    if (on) {
        auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                                                  QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Inhibit"));
        msg.setArguments({QStringLiteral("io.github.lumen"), tr("Do Not Disturb, turned on in Lumen"), QVariantMap()});
        const QDBusReply<uint> reply = bus.call(msg);
        m_dndCookie = reply.isValid() ? reply.value() : 0;
        launcher()->notify(m_dndCookie ? tr("Do Not Disturb is on") : tr("The notification server refused"));
    } else {
        auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                                                  QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("UnInhibit"));
        msg.setArguments({m_dndCookie});
        bus.call(msg);
        m_dndCookie = 0;
        launcher()->notify(tr("Do Not Disturb is off"));
    }
}

// Switches between the user's dark and light colour schemes. The scheme in
// use is remembered for its brightness, so a custom theme comes back when
// switching back; the first time, Breeze Dark or Breeze Light is used.
void SystemSource::switchColourScheme(bool dark)
{
    KConfigGroup appearance = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Appearance"));
    const QString current = KSharedConfig::openConfig(QStringLiteral("kdeglobals"))->group(QStringLiteral("General")).readEntry("ColorScheme");
    if (!current.isEmpty()) {
        appearance.writeEntry(paletteIsDark() ? "DarkScheme" : "LightScheme", current);
        appearance.sync();
    }
    const QString target = dark ? appearance.readEntry("DarkScheme", QStringLiteral("BreezeDark"))
                                : appearance.readEntry("LightScheme", QStringLiteral("BreezeLight"));
    run(QStringLiteral("plasma-apply-colorscheme"), {target}, dark ? tr("Dark colours") : tr("Light colours"));
}

void SystemSource::pickColour()
{
    // KWin lets the user click any pixel; the answer may take a while.
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/ColorPicker"),
                                              QStringLiteral("org.kde.kwin.ColorPicker"), QStringLiteral("pick"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg, 120000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            launcher()->notify(tr("No colour picked"));
            return;
        }
        // KWin sends a QColor as a struct of four ints (r, g, b, a).
        int rgba[4] = {0, 0, 0, 255};
        const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
        arg.beginStructure();
        for (int &v : rgba)
            arg >> v;
        arg.endStructure();
        launcher()->copyText(QColor(rgba[0], rgba[1], rgba[2]).name().toUpper());
    });
}

QList<Item> SystemSource::commands() const
{
    QList<Item> list;
    Launcher *l = launcher();
    auto add = [&](const QString &id, const QString &title, const QString &subtitle, const QString &icon, const QStringList &keywords,
                   const Action &action) {
        Item item;
        item.id = QStringLiteral("system:") + id;
        item.title = title;
        item.subtitle = subtitle;
        item.kind = tr("System");
        item.icon = QIcon::fromTheme(icon);
        item.actions << action;
        item.score = match(m_lastQuery, title, keywords);
        if (item.score > 0)
            list << item;
    };
    auto self = const_cast<SystemSource *>(this);

    if (m_wifiKnown) {
        const bool on = m_wifi;
        add(QStringLiteral("wifi"), on ? tr("Turn Wi-Fi Off") : tr("Turn Wi-Fi On"), on ? tr("Wi-Fi is on") : tr("Wi-Fi is off"),
            on ? QStringLiteral("network-wireless") : QStringLiteral("network-wireless-disconnected"),
            {QStringLiteral("wifi"), QStringLiteral("wireless"), QStringLiteral("wlan"), QStringLiteral("internet")},
            {tr("Toggle"), {}, {}, [self, on] {
                 self->run(QStringLiteral("nmcli"), {QStringLiteral("radio"), QStringLiteral("wifi"), on ? QStringLiteral("off") : QStringLiteral("on")},
                           on ? tr("Wi-Fi off") : tr("Wi-Fi on"), true);
             }});
    }
    if (m_bluetoothKnown) {
        const bool on = m_bluetooth;
        add(QStringLiteral("bluetooth"), on ? tr("Turn Bluetooth Off") : tr("Turn Bluetooth On"),
            on ? tr("Bluetooth is on") : tr("Bluetooth is off"), on ? QStringLiteral("network-bluetooth") : QStringLiteral("network-bluetooth-inactive"),
            {QStringLiteral("bluetooth"), QStringLiteral("bt")},
            {tr("Toggle"), {}, {}, [self, on] {
                 self->run(QStringLiteral("bluetoothctl"), {QStringLiteral("power"), on ? QStringLiteral("off") : QStringLiteral("on")},
                           on ? tr("Bluetooth off") : tr("Bluetooth on"), true);
             }});
    }
    const bool dnd = m_dndCookie != 0;
    add(QStringLiteral("dnd"), dnd ? tr("Turn Do Not Disturb Off") : tr("Turn Do Not Disturb On"), tr("Notifications"),
        dnd ? QStringLiteral("notifications") : QStringLiteral("notifications-disabled"),
        {QStringLiteral("dnd"), QStringLiteral("do not disturb"), QStringLiteral("focus"), QStringLiteral("notifications"), QStringLiteral("silence")},
        {tr("Toggle"), {}, {}, [self, dnd] { self->setDoNotDisturb(!dnd); }});
    const bool dark = paletteIsDark();
    add(QStringLiteral("theme"), dark ? tr("Switch to Light Colours") : tr("Switch to Dark Colours"), tr("Colour scheme"),
        dark ? QStringLiteral("weather-clear") : QStringLiteral("weather-clear-night"),
        {QStringLiteral("dark mode"), QStringLiteral("light mode"), QStringLiteral("theme"), QStringLiteral("appearance")},
        {tr("Switch"), {}, {}, [self, dark] { self->switchColourScheme(!dark); }});

    // Volume: "volume 40" sets it; otherwise up, down and mute.
    const QString volumeSubtitle = m_volume < 0 ? QString() : m_muted ? tr("Muted") : tr("Volume %1%").arg(m_volume);
    static const QRegularExpression setVolume(QStringLiteral("^(?:volume|vol) (\\d{1,3})%?$"));
    if (const auto m = setVolume.match(m_lastQuery.trimmed().toLower()); m.hasMatch()) {
        const int percent = std::min(m.captured(1).toInt(), 150);
        Item item;
        item.id = QStringLiteral("system:volume-set");
        item.title = tr("Set Volume to %1%").arg(percent);
        item.subtitle = volumeSubtitle;
        item.kind = tr("System");
        item.icon = QIcon::fromTheme(QStringLiteral("audio-volume-medium"));
        item.score = 110;
        item.actions << Action{tr("Set"), {}, {}, [self, percent] {
            self->run(QStringLiteral("wpctl"), {QStringLiteral("set-volume"), QStringLiteral("@DEFAULT_AUDIO_SINK@"), QStringLiteral("%1%").arg(percent)},
                      tr("Volume %1%").arg(percent), true);
        }};
        list << item;
    }
    const QStringList volumeWords{QStringLiteral("volume"), QStringLiteral("sound"), QStringLiteral("audio"), QStringLiteral("louder"), QStringLiteral("quieter")};
    add(QStringLiteral("volume-up"), tr("Volume Up"), volumeSubtitle, QStringLiteral("audio-volume-high"), volumeWords,
        {tr("Raise by 10%"), {}, {}, [self] {
             self->run(QStringLiteral("wpctl"), {QStringLiteral("set-volume"), QStringLiteral("-l"), QStringLiteral("1.0"),
                                                 QStringLiteral("@DEFAULT_AUDIO_SINK@"), QStringLiteral("10%+")}, tr("Volume up"), true);
         }, true});
    add(QStringLiteral("volume-down"), tr("Volume Down"), volumeSubtitle, QStringLiteral("audio-volume-low"), volumeWords,
        {tr("Lower by 10%"), {}, {}, [self] {
             self->run(QStringLiteral("wpctl"), {QStringLiteral("set-volume"), QStringLiteral("@DEFAULT_AUDIO_SINK@"), QStringLiteral("10%-")},
                       tr("Volume down"), true);
         }, true});
    add(QStringLiteral("mute"), m_muted ? tr("Unmute") : tr("Mute"), volumeSubtitle,
        m_muted ? QStringLiteral("audio-volume-high") : QStringLiteral("audio-volume-muted"), QStringList(volumeWords) << QStringLiteral("mute"),
        {tr("Toggle"), {}, {}, [self, muted = m_muted] {
             self->run(QStringLiteral("wpctl"), {QStringLiteral("set-mute"), QStringLiteral("@DEFAULT_AUDIO_SINK@"), QStringLiteral("toggle")},
                       muted ? tr("Unmuted") : tr("Muted"), true);
         }});

    // Removable drives that are mounted.
    for (const Solid::Device &device : Solid::Device::listFromType(Solid::DeviceInterface::StorageAccess)) {
        const auto *access = device.as<Solid::StorageAccess>();
        if (!access || !access->isAccessible() || access->filePath() == QLatin1String("/"))
            continue;
        Solid::Device driveDevice = device;
        while (driveDevice.isValid() && !driveDevice.is<Solid::StorageDrive>())
            driveDevice = driveDevice.parent();
        const auto *drive = driveDevice.as<Solid::StorageDrive>();
        if (!drive || !(drive->isHotpluggable() || drive->isRemovable()))
            continue;
        const QString udi = device.udi();
        const QString label = device.description();
        add(QStringLiteral("eject-") + udi, tr("Eject %1").arg(label), access->filePath(), QStringLiteral("media-eject"),
            {QStringLiteral("eject"), QStringLiteral("unmount"), QStringLiteral("remove drive"), label},
            {tr("Eject"), {}, {}, [l, udi, label] {
                 Solid::Device d(udi);
                 if (auto *a = d.as<Solid::StorageAccess>()) {
                     QObject::connect(a, &Solid::StorageAccess::teardownDone, l, [l, label, udi](Solid::ErrorType error) {
                         if (error != Solid::NoError) {
                             l->notify(tr("%1 is still in use").arg(label));
                             return;
                         }
                         Solid::Device parent = Solid::Device(udi).parent();
                         if (auto *optical = parent.as<Solid::OpticalDrive>())
                             optical->eject();
                         l->notify(tr("%1 can be removed").arg(label));
                     });
                     a->teardown();
                 }
             }});
    }

    // Screenshots and recordings (Lumen closes first so it is not in them).
    struct Shot {
        QString id, title, icon;
        QStringList args;
        QString done;
    };
    const QList<Shot> shots{
        {QStringLiteral("shot-region"), tr("Screenshot Region"), QStringLiteral("region"), {QStringLiteral("-b"), QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("-r")}, tr("Screenshot copied")},
        {QStringLiteral("shot-window"), tr("Screenshot Window"), QStringLiteral("window"), {QStringLiteral("-b"), QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("-a")}, tr("Screenshot copied")},
        {QStringLiteral("shot-screen"), tr("Screenshot Screen"), QStringLiteral("view-fullscreen"), {QStringLiteral("-b"), QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("-m")}, tr("Screenshot copied")},
        {QStringLiteral("record-screen"), tr("Record Screen"), QStringLiteral("media-record"), {QStringLiteral("-R"), QStringLiteral("s")}, {}},
        {QStringLiteral("record-region"), tr("Record Region"), QStringLiteral("media-record"), {QStringLiteral("-R"), QStringLiteral("r")}, {}},
        {QStringLiteral("record-window"), tr("Record Window"), QStringLiteral("media-record"), {QStringLiteral("-R"), QStringLiteral("w")}, {}},
    };
    for (const Shot &s : shots) {
        const Shot shot = s;
        add(shot.id, shot.title, shot.done.isEmpty() ? tr("Spectacle") : tr("Copied to the clipboard"), shot.icon,
            {QStringLiteral("screenshot"), QStringLiteral("capture"), QStringLiteral("record"), QStringLiteral("screen recording")},
            {tr("Capture"), {}, {}, [l, self, shot] {
                 l->runAfterHide([self, shot] {
                     if (shot.done.isEmpty())
                         QProcess::startDetached(QStringLiteral("spectacle"), shot.args);
                     else
                         self->run(QStringLiteral("spectacle"), shot.args, shot.done);
                 });
             }});
    }
    add(QStringLiteral("pick-colour"), tr("Pick Colour"), tr("Click anywhere on the screen to copy its colour"), QStringLiteral("color-picker"),
        {QStringLiteral("color picker"), QStringLiteral("colour picker"), QStringLiteral("eyedropper"), QStringLiteral("pick color")},
        {tr("Pick"), {}, {}, [l, self] { l->runAfterHide([self] { self->pickColour(); }); }});

    list << mediaItems();
    return list;
}

QList<Item> SystemSource::mediaItems() const
{
    QList<Item> list;
    auto self = const_cast<SystemSource *>(this);
    const QStringList words{QStringLiteral("music"), QStringLiteral("media"), QStringLiteral("song"), QStringLiteral("track"),
                            QStringLiteral("play"), QStringLiteral("pause")};
    for (const Player &p : m_players) {
        const QString nowPlaying = p.title.isEmpty() ? p.name : p.artist.isEmpty() ? p.title : p.title + QStringLiteral(" · ") + p.artist;
        auto add = [&](const QString &id, const QString &title, const QString &icon, const QString &method, const QStringList &keywords) {
            Item item;
            item.id = QStringLiteral("media:") + id;
            item.title = title;
            item.subtitle = nowPlaying;
            item.kind = p.name;
            item.icon = QIcon::fromTheme(icon);
            item.score = m_lastQuery.isEmpty() ? 0 : std::max(match(m_lastQuery, title, keywords), match(m_lastQuery, nowPlaying, {p.name}) * 0.8);
            const QString service = p.service;
            item.actions << Action{title, {}, {}, [self, service, method] { self->mediaCall(service, method); }, true};
            if (item.score > 0)
                list << item;
        };
        add(QStringLiteral("playpause"), p.playing ? tr("Pause") : tr("Play"), p.playing ? QStringLiteral("media-playback-pause") : QStringLiteral("media-playback-start"),
            QStringLiteral("PlayPause"), words);
        add(QStringLiteral("next"), tr("Next Track"), QStringLiteral("media-skip-forward"), QStringLiteral("Next"), QStringList(words) << QStringLiteral("skip"));
        add(QStringLiteral("previous"), tr("Previous Track"), QStringLiteral("media-skip-backward"), QStringLiteral("Previous"), words);
        break; // the first (playing) player is enough
    }
    return list;
}

QList<Item> SystemSource::startItems() const
{
    QList<Item> items;
    if (m_players.isEmpty() || !m_players.first().playing)
        return items;
    const Player &p = m_players.first();
    Item item;
    item.id = QStringLiteral("media:now-playing");
    item.title = p.title.isEmpty() ? tr("Now Playing") : p.title;
    item.subtitle = p.artist.isEmpty() ? p.name : p.artist;
    item.kind = p.name;
    item.icon = QIcon::fromTheme(QStringLiteral("media-playback-pause"));
    item.score = 100;
    auto self = const_cast<SystemSource *>(this);
    const QString service = p.service;
    item.actions << Action{tr("Pause"), {}, {}, [self, service] { self->mediaCall(service, QStringLiteral("PlayPause")); }};
    item.actions << Action{tr("Next Track"), QStringLiteral("media-skip-forward"), QKeySequence(Qt::CTRL | Qt::Key_Right),
                           [self, service] { self->mediaCall(service, QStringLiteral("Next")); }, true};
    item.actions << Action{tr("Previous Track"), QStringLiteral("media-skip-backward"), QKeySequence(Qt::CTRL | Qt::Key_Left),
                           [self, service] { self->mediaCall(service, QStringLiteral("Previous")); }, true};
    items << item;
    return items;
}

void SystemSource::query(const QString &text)
{
    m_lastQuery = text.trimmed();
    Q_EMIT resultsReady(m_lastQuery.isEmpty() ? QList<Item>() : commands());
}
