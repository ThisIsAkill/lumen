#include "desktopsource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>

namespace
{

// Runs a program and hands its output to done (also on failure: "" then).
void readOutput(QObject *context, const QString &program, const QStringList &args, std::function<void(const QString &)> done)
{
    auto *proc = new QProcess(context);
    QObject::connect(proc, &QProcess::finished, context, [proc, done](int code) {
        done(code == 0 ? QString::fromUtf8(proc->readAllStandardOutput()) : QString());
        proc->deleteLater();
    });
    QObject::connect(proc, &QProcess::errorOccurred, context, [proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            proc->deleteLater(); // the tool is not installed
    });
    proc->start(program, args);
}

qreal bestOf(const QString &q, const QStringList &texts, qreal keywordWeight, const QStringList &keywords)
{
    qreal best = 0;
    for (const QString &t : texts)
        best = std::max(best, fuzzyScore(q, t));
    for (const QString &k : keywords)
        best = std::max(best, fuzzyScore(q, k) * keywordWeight);
    return best;
}

// nmcli -t escapes ':' inside fields as "\:".
QStringList splitTerse(const QString &line)
{
    QStringList fields;
    QString current;
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (line.at(i) == u'\\' && i + 1 < line.size()) {
            current += line.at(++i);
        } else if (line.at(i) == u':') {
            fields << current;
            current.clear();
        } else {
            current += line.at(i);
        }
    }
    fields << current;
    return fields;
}

} // namespace

DesktopSource::DesktopSource(Launcher *launcher)
    : Source(launcher)
{
    qDBusRegisterMetaType<NMSettings>(); // NetworkManager's a{sa{sv}}
}

void DesktopSource::sessionStarted()
{
    readAudio();
    readBluetooth();
    readWifi();
}

void DesktopSource::readAudio()
{
    auto parse = [this](bool input) {
        return [this, input](const QString &json) {
            m_audio.removeIf([input](const AudioDevice &d) { return d.input == input; });
            for (const QJsonValue &v : QJsonDocument::fromJson(json.toUtf8()).array()) {
                const QJsonObject o = v.toObject();
                const QString name = o.value(QStringLiteral("name")).toString();
                if (name.endsWith(QLatin1String(".monitor")))
                    continue; // a recording of an output, not a microphone
                m_audio << AudioDevice{name, o.value(QStringLiteral("description")).toString(), input, false};
            }
            readOutput(this, QStringLiteral("pactl"), {input ? QStringLiteral("get-default-source") : QStringLiteral("get-default-sink")},
                       [this, input](const QString &current) {
                           for (AudioDevice &d : m_audio)
                               if (d.input == input)
                                   d.current = d.name == current.trimmed();
                           if (!m_query.isEmpty())
                               query(m_query);
                       });
        };
    };
    readOutput(this, QStringLiteral("pactl"), {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sinks")}, parse(false));
    readOutput(this, QStringLiteral("pactl"), {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sources")}, parse(true));
}

void DesktopSource::readBluetooth()
{
    readOutput(this, QStringLiteral("bluetoothctl"), {QStringLiteral("devices"), QStringLiteral("Paired")}, [this](const QString &paired) {
        static const QRegularExpression line(QStringLiteral("^Device ([0-9A-F:]{17}) (.+)$"), QRegularExpression::MultilineOption);
        QList<BluetoothDevice> devices;
        for (auto it = line.globalMatch(paired); it.hasNext();) {
            const auto m = it.next();
            devices << BluetoothDevice{m.captured(1), m.captured(2).trimmed(), false};
        }
        readOutput(this, QStringLiteral("bluetoothctl"), {QStringLiteral("devices"), QStringLiteral("Connected")}, [this, devices](const QString &connected) {
            m_bluetooth = devices;
            for (BluetoothDevice &d : m_bluetooth)
                d.connected = connected.contains(d.address);
            if (!m_query.isEmpty())
                query(m_query);
        });
    });
}

void DesktopSource::readWifi()
{
    readOutput(this, QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("NAME,TYPE"), QStringLiteral("connection"), QStringLiteral("show")},
               [this](const QString &saved) {
                   QSet<QString> known;
                   for (const QString &l : saved.split(u'\n', Qt::SkipEmptyParts)) {
                       const QStringList f = splitTerse(l);
                       if (f.value(1) == QLatin1String("802-11-wireless"))
                           known.insert(f.value(0));
                   }
                   // Uses the last scan (no rescan: that takes seconds).
                   readOutput(this, QStringLiteral("nmcli"),
                              {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("IN-USE,SSID,SIGNAL,SECURITY"), QStringLiteral("device"),
                               QStringLiteral("wifi"), QStringLiteral("list"), QStringLiteral("--rescan"), QStringLiteral("no")},
                              [this, known](const QString &list) {
                                  m_networks.clear();
                                  QSet<QString> seen;
                                  for (const QString &l : list.split(u'\n', Qt::SkipEmptyParts)) {
                                      const QStringList f = splitTerse(l);
                                      const QString ssid = f.value(1);
                                      if (ssid.isEmpty() || seen.contains(ssid))
                                          continue; // hidden networks; the same network on several access points
                                      seen.insert(ssid);
                                      m_networks << Network{ssid, f.value(2).toInt(), f.value(3), f.value(0) == QLatin1String("*"), known.contains(ssid)};
                                  }
                                  if (!m_query.isEmpty())
                                      query(m_query);
                              });
               });
}

void DesktopSource::run(const QString &program, const QStringList &args, const QString &doing, const QString &done, const QString &failed)
{
    if (!doing.isEmpty())
        launcher()->notify(doing);
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc, done, failed](int code) {
        launcher()->notify(code == 0 ? done : failed);
        proc->deleteLater();
        QTimer::singleShot(500, this, &DesktopSource::sessionStarted); // show the new state
    });
    proc->start(program, args);
}

void DesktopSource::connectWifi(const Network &n)
{
    if (n.saved || n.security.isEmpty()) {
        run(QStringLiteral("nmcli"), {QStringLiteral("device"), QStringLiteral("wifi"), QStringLiteral("connect"), n.ssid},
            tr("Connecting to %1…").arg(n.ssid), tr("Connected to %1").arg(n.ssid), tr("Could not connect to %1").arg(n.ssid));
        return;
    }
    Item shown;
    shown.title = n.ssid;
    shown.subtitle = tr("%1 · signal %2%").arg(n.security).arg(n.signal);
    shown.icon = QIcon::fromTheme(QStringLiteral("network-wireless"));
    const QString ssid = n.ssid;
    launcher()->ask(tr("Password"), tr("Wi-Fi password for “%1”, then Enter").arg(ssid), shown, true, [this, n](const QString &password) {
        if (!password.isEmpty())
            joinWithPassword(n, password);
        launcher()->hideLauncher();
    });
}

// Creates and activates a connection through NetworkManager's D-Bus API, so
// the password is never on a command line (where other processes could
// read it). NetworkManager saves it like its own applet does.
void DesktopSource::joinWithPassword(const Network &n, const QString &password)
{
    readOutput(this, QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("DEVICE,TYPE"), QStringLiteral("device")},
               [this, n, password](const QString &devices) {
                   QString iface;
                   for (const QString &l : devices.split(u'\n', Qt::SkipEmptyParts))
                       if (splitTerse(l).value(1) == QLatin1String("wifi"))
                           iface = splitTerse(l).value(0);
                   auto bus = QDBusConnection::systemBus();
                   const QString service = QStringLiteral("org.freedesktop.NetworkManager");
                   const QDBusReply<QDBusObjectPath> device = bus.call(QDBusMessage::createMethodCall(
                       service, QStringLiteral("/org/freedesktop/NetworkManager"), service, QStringLiteral("GetDeviceByIpIface")) << iface);
                   if (!device.isValid()) {
                       launcher()->notify(tr("No Wi-Fi device"));
                       return;
                   }
                   NMSettings settings;
                   settings[QStringLiteral("connection")] = {{QStringLiteral("id"), n.ssid}, {QStringLiteral("type"), QStringLiteral("802-11-wireless")}};
                   settings[QStringLiteral("802-11-wireless")] = {{QStringLiteral("ssid"), n.ssid.toUtf8()},
                                                                   {QStringLiteral("mode"), QStringLiteral("infrastructure")}};
                   // WPA3-only networks use SAE; everything else here is WPA/WPA2 personal.
                   const bool sae = n.security.contains(QLatin1String("WPA3")) && !n.security.contains(QLatin1String("WPA2"));
                   settings[QStringLiteral("802-11-wireless-security")] = {{QStringLiteral("key-mgmt"), sae ? QStringLiteral("sae") : QStringLiteral("wpa-psk")},
                                                                            {QStringLiteral("psk"), password}};
                   auto msg = QDBusMessage::createMethodCall(service, QStringLiteral("/org/freedesktop/NetworkManager"), service,
                                                             QStringLiteral("AddAndActivateConnection"));
                   msg << QVariant::fromValue(settings) << QVariant::fromValue(device.value()) << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")));
                   launcher()->notify(tr("Connecting to %1…").arg(n.ssid));
                   auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
                   connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, ssid = n.ssid] {
                       watcher->deleteLater();
                       launcher()->notify(watcher->isError() ? tr("Could not connect to %1").arg(ssid) : tr("Joining %1").arg(ssid));
                       QTimer::singleShot(4000, this, &DesktopSource::sessionStarted);
                   });
               });
}

QList<Item> DesktopSource::audioItems(const QString &q) const
{
    QList<Item> items;
    auto self = const_cast<DesktopSource *>(this);
    const QStringList outputWords{QStringLiteral("audio output"), QStringLiteral("sound output"), QStringLiteral("speakers"),
                                  QStringLiteral("headphones"), QStringLiteral("output")};
    const QStringList inputWords{QStringLiteral("microphone"), QStringLiteral("mic"), QStringLiteral("audio input"), QStringLiteral("input")};
    for (const AudioDevice &d : m_audio) {
        const qreal score = bestOf(q, {d.description}, 0.85, d.input ? inputWords : outputWords);
        if (score < 40)
            continue;
        Item item;
        item.id = QStringLiteral("audio:") + d.name;
        item.title = d.input ? tr("Use Microphone: %1").arg(d.description) : tr("Play Sound on %1").arg(d.description);
        item.subtitle = d.current ? tr("In use") : d.input ? tr("Microphone") : tr("Sound output");
        item.kind = d.current ? tr("Current") : tr("Audio");
        item.icon = QIcon::fromTheme(d.input ? QStringLiteral("audio-input-microphone") : QStringLiteral("audio-speakers"));
        item.score = score - (d.current ? 5 : 0); // the others are what one wants to switch to
        const QString name = d.name, description = d.description;
        const bool input = d.input;
        if (!d.current)
            item.actions << Action{tr("Switch"), {}, {}, [self, name, description, input] {
                self->run(QStringLiteral("pactl"), {input ? QStringLiteral("set-default-source") : QStringLiteral("set-default-sink"), name}, {},
                          input ? tr("Microphone: %1").arg(description) : tr("Sound on %1").arg(description), tr("Could not switch"));
            }};
        items << item;
    }
    return items;
}

QList<Item> DesktopSource::bluetoothItems(const QString &q) const
{
    QList<Item> items;
    auto self = const_cast<DesktopSource *>(this);
    for (const BluetoothDevice &d : m_bluetooth) {
        const qreal score = bestOf(q, {d.name}, 0.85, {QStringLiteral("bluetooth"), QStringLiteral("connect"), QStringLiteral("disconnect")});
        if (score < 40)
            continue;
        Item item;
        item.id = QStringLiteral("bluetooth:") + d.address;
        item.title = d.connected ? tr("Disconnect %1").arg(d.name) : tr("Connect %1").arg(d.name);
        item.subtitle = d.connected ? tr("Connected") : tr("Paired");
        item.kind = tr("Bluetooth");
        item.icon = QIcon::fromTheme(d.connected ? QStringLiteral("network-bluetooth-activated") : QStringLiteral("network-bluetooth"));
        item.score = score;
        const QString address = d.address, name = d.name;
        const bool connected = d.connected;
        item.actions << Action{connected ? tr("Disconnect") : tr("Connect"), {}, {}, [self, address, name, connected] {
            self->run(QStringLiteral("bluetoothctl"), {connected ? QStringLiteral("disconnect") : QStringLiteral("connect"), address},
                      connected ? QString() : tr("Connecting %1…").arg(name), connected ? tr("%1 disconnected").arg(name) : tr("%1 connected").arg(name),
                      tr("Could not reach %1").arg(name));
        }};
        items << item;
    }
    return items;
}

QList<Item> DesktopSource::wifiItems(const QString &q) const
{
    QList<Item> items;
    auto self = const_cast<DesktopSource *>(this);
    const bool listAll = bestOf(q, {}, 1, {QStringLiteral("wifi"), QStringLiteral("wi-fi"), QStringLiteral("wireless"), QStringLiteral("networks")}) >= 80;
    for (int i = 0; i < m_networks.size(); ++i) {
        const Network &n = m_networks.at(i);
        const qreal score = listAll ? 80 - i * 0.01 : fuzzyScore(q, n.ssid);
        if (score < 40)
            continue;
        Item item;
        item.id = QStringLiteral("wifi:") + n.ssid;
        item.title = n.current ? tr("Connected to %1").arg(n.ssid) : tr("Join %1").arg(n.ssid);
        item.subtitle = tr("Signal %1%").arg(n.signal) + (n.security.isEmpty() ? tr(" · open") : QStringLiteral(" · ") + n.security)
            + (n.saved && !n.current ? tr(" · saved") : QString());
        item.kind = tr("Wi-Fi");
        item.icon = QIcon::fromTheme(n.signal > 75 ? QStringLiteral("network-wireless-signal-excellent")
                                     : n.signal > 50 ? QStringLiteral("network-wireless-signal-good")
                                     : n.signal > 25 ? QStringLiteral("network-wireless-signal-ok")
                                                     : QStringLiteral("network-wireless-signal-weak"));
        item.score = score - (n.current ? 10 : 0);
        const Network network = n;
        if (n.current)
            item.actions << Action{tr("Disconnect"), {}, {}, [self, network] {
                self->run(QStringLiteral("nmcli"), {QStringLiteral("connection"), QStringLiteral("down"), network.ssid}, {},
                          tr("Disconnected from %1").arg(network.ssid), tr("Could not disconnect"));
            }};
        else
            item.actions << Action{tr("Connect"), {}, {}, [self, network] { self->connectWifi(network); }, !network.saved && !network.security.isEmpty()};
        items << item;
    }
    return items;
}

void DesktopSource::query(const QString &text)
{
    m_query = text.trimmed();
    if (m_query.isEmpty()) {
        Q_EMIT resultsReady({});
        return;
    }
    Q_EMIT resultsReady(audioItems(m_query) + bluetoothItems(m_query) + wifiItems(m_query));
}
