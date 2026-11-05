#include "network.h"
#include "fuzzy.h"
#include "launcher.h"
#include "outputview.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace
{

constexpr int PublicIpSeconds = 300; // how long the public address is trusted
constexpr int LatencyPings = 6;
constexpr qint64 DownloadBytes = 25'000'000;
constexpr qint64 UploadBytes = 8'000'000;
const QString SpeedServer = QStringLiteral("https://speed.cloudflare.com");

bool installed(const char *program)
{
    return !QStandardPaths::findExecutable(QLatin1String(program)).isEmpty();
}

// A host name or address after a word: "dns example.com" → "example.com".
QString target(const QString &query, const QStringList &words)
{
    static const QRegularExpression host(QStringLiteral(R"(^[A-Za-z0-9.:_-]+$)"));
    for (const QString &w : words) {
        if (query.startsWith(w + u' ', Qt::CaseInsensitive)) {
            const QString t = query.mid(w.size() + 1).trimmed();
            return host.match(t).hasMatch() ? t : QString();
        }
    }
    return {};
}

Item card(const QString &id, const QString &title, const QString &subtitle, const QString &icon, qreal score)
{
    Item item;
    item.id = QStringLiteral("answer:") + id;
    item.title = title;
    item.subtitle = subtitle;
    item.kind = QObject::tr("Network");
    item.icon = QIcon::fromTheme(icon);
    item.score = score;
    return item;
}

} // namespace

NetworkSource::NetworkSource(Launcher *launcher)
    : Source(launcher)
    , m_network(new QNetworkAccessManager(this))
{
    m_hasTailscale = installed("tailscale");
}

QString NetworkSource::speedText(double bps)
{
    if (bps >= 1e9)
        return QStringLiteral("%1 Gbps").arg(bps / 1e9, 0, 'f', 2);
    if (bps >= 1e6)
        return QStringLiteral("%1 Mbps").arg(bps / 1e6, 0, 'f', bps >= 1e8 ? 0 : 1);
    return QStringLiteral("%1 kbps").arg(bps / 1e3, 0, 'f', 0);
}

QList<NetworkSource::Peer> NetworkSource::parseTailscale(const QJsonObject &status)
{
    QList<Peer> peers;
    const QJsonObject all = status.value(QStringLiteral("Peer")).toObject();
    for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
        const QJsonObject p = it->toObject();
        Peer peer;
        peer.name = p.value(QStringLiteral("HostName")).toString();
        peer.dnsName = p.value(QStringLiteral("DNSName")).toString().chopped(p.value(QStringLiteral("DNSName")).toString().endsWith(u'.') ? 1 : 0);
        peer.ip = p.value(QStringLiteral("TailscaleIPs")).toArray().at(0).toString();
        peer.os = p.value(QStringLiteral("OS")).toString();
        peer.online = p.value(QStringLiteral("Online")).toBool();
        peer.exitNodeOption = p.value(QStringLiteral("ExitNodeOption")).toBool();
        peer.exitNode = p.value(QStringLiteral("ExitNode")).toBool();
        peers << peer;
    }
    // Online first, then by name.
    std::sort(peers.begin(), peers.end(), [](const Peer &a, const Peer &b) {
        return a.online != b.online ? a.online : a.name.localeAwareCompare(b.name) < 0;
    });
    return peers;
}

void NetworkSource::sessionStarted()
{
    m_loaded = false; // read again once something is typed
}

void NetworkSource::loadPublicIp()
{
    if (m_publicIpAt.isValid() && m_publicIpAt.secsTo(QDateTime::currentDateTime()) < PublicIpSeconds)
        return;
    m_publicIpAt = QDateTime::currentDateTime();
    QNetworkRequest request(QUrl(QStringLiteral("https://ipinfo.io/json")));
    request.setTransferTimeout(6000);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_publicIp = QJsonDocument::fromJson(reply->readAll()).object();
        if (m_publicIp.isEmpty())
            m_publicIpAt = {}; // try again next time
        publish();
    });
}

void NetworkSource::loadVpns()
{
    if (!installed("nmcli"))
        return;
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc] {
        proc->deleteLater();
        m_vpns.clear();
        // NAME:UUID:TYPE:ACTIVE, with ":" in names written as "\:".
        for (const QString &line : QString::fromUtf8(proc->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts)) {
            QStringList f = line.split(QRegularExpression(QStringLiteral(R"((?<!\\):)")));
            if (f.size() < 4)
                continue;
            const QString type = f.at(2);
            if (type != QLatin1String("vpn") && type != QLatin1String("wireguard"))
                continue;
            m_vpns << Vpn{QString(f.at(0)).replace(QLatin1String("\\:"), QLatin1String(":")), f.at(1), type, f.at(3) == QLatin1String("yes")};
        }
        if (!m_query.isEmpty())
            publish();
    });
    proc->start(QStringLiteral("nmcli"), {QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("NAME,UUID,TYPE,ACTIVE"), QStringLiteral("connection"), QStringLiteral("show")});
}

void NetworkSource::loadTailscale()
{
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc] {
        proc->deleteLater();
        m_tailscale = QJsonDocument::fromJson(proc->readAllStandardOutput()).object();
        if (!m_query.isEmpty())
            publish();
    });
    proc->start(QStringLiteral("tailscale"), {QStringLiteral("status"), QStringLiteral("--json")});
    QTimer::singleShot(4000, proc, [proc] { proc->kill(); });
}

void NetworkSource::command(const QString &title, const QStringList &argv)
{
    Launcher *l = launcher();
    auto *view = new OutputView(l, title, QStringLiteral("network-wired"));
    auto *proc = new QProcess(view);
    view->follow(proc);
    l->pushView(view);
    proc->start(argv.first(), argv.mid(1));
}

void NetworkSource::quietly(const QStringList &argv, const QString &done, std::function<void()> then)
{
    auto *proc = new QProcess(this);
    Launcher *l = launcher();
    connect(proc, &QProcess::finished, this, [proc, l, done, then](int code) {
        proc->deleteLater();
        if (code == 0) {
            l->notify(done);
        } else {
            const QStringList lines = QString::fromUtf8(proc->readAllStandardError() + proc->readAllStandardOutput()).trimmed().split(u'\n');
            l->notify(lines.value(0).isEmpty() ? QObject::tr("That did not work") : lines.value(0));
        }
        if (then)
            then();
    });
    proc->start(argv.first(), argv.mid(1));
}

// ---------------------------------------------------------- speed test

void NetworkSource::runSpeedTest()
{
    if (m_step != Step::Idle && m_step != Step::Done)
        return;
    m_pings.clear();
    m_down = m_up = 0;
    m_speedError.clear();
    m_step = Step::Latency;
    speedStep();
    publish();
}

void NetworkSource::speedStep()
{
    QNetworkRequest request;
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    m_clock.start();
    if (m_step == Step::Latency) {
        // Round trips of an empty download; the first also opens the connection.
        request.setUrl(QUrl(SpeedServer + QStringLiteral("/__down?bytes=0")));
        m_speedReply = m_network->get(request);
    } else if (m_step == Step::Download) {
        request.setUrl(QUrl(SpeedServer + QStringLiteral("/__down?bytes=%1").arg(DownloadBytes)));
        m_speedReply = m_network->get(request);
        // Progress while it runs: the speed so far.
        connect(m_speedReply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
            if (m_clock.elapsed() > 200)
                m_down = got * 8.0 / (m_clock.elapsed() / 1000.0);
            publish();
        });
    } else if (m_step == Step::Upload) {
        request.setUrl(QUrl(SpeedServer + QStringLiteral("/__up")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        m_speedReply = m_network->post(request, QByteArray(UploadBytes, '0'));
        connect(m_speedReply, &QNetworkReply::uploadProgress, this, [this](qint64 sent, qint64) {
            if (m_clock.elapsed() > 200)
                m_up = sent * 8.0 / (m_clock.elapsed() / 1000.0);
            publish();
        });
    } else {
        return;
    }
    connect(m_speedReply, &QNetworkReply::finished, this, [this, reply = m_speedReply.data()] {
        reply->deleteLater();
        const double seconds = m_clock.elapsed() / 1000.0;
        if (reply->error() != QNetworkReply::NoError) {
            m_speedError = reply->errorString();
            m_step = Step::Done;
            publish();
            return;
        }
        if (m_step == Step::Latency) {
            m_pings << seconds * 1000;
            if (m_pings.size() < LatencyPings + 1) {
                speedStep();
                return;
            }
            m_pings.removeFirst(); // the one that set up the connection
            m_step = Step::Download;
        } else if (m_step == Step::Download) {
            m_down = DownloadBytes * 8.0 / seconds;
            m_step = Step::Upload;
        } else {
            m_up = UploadBytes * 8.0 / seconds;
            m_step = Step::Done;
        }
        publish();
        speedStep();
    });
}

// --------------------------------------------------------------- query

void NetworkSource::query(const QString &text)
{
    m_query = text.trimmed();
    const QString q = m_query.toLower();
    if (!m_loaded && !q.isEmpty()) {
        // VPNs and the tailnet: local and quick, read once typing starts.
        m_loaded = true;
        loadVpns();
        if (m_hasTailscale)
            loadTailscale();
    }
    if (q == QLatin1String("ip") || q.contains(QLatin1String("my ip")) || q.startsWith(QLatin1String("ip addr")) || q == QLatin1String("public ip"))
        loadPublicIp();
    publish();
}

void NetworkSource::publish()
{
    QList<Item> items;
    const QString q = m_query.toLower();
    if (q.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    Launcher *l = launcher();
    auto self = this;
    auto asks = [&](std::initializer_list<const char *> words, qreal at = 80) {
        for (const char *w : words)
            if (fuzzyScore(q, QLatin1String(w)) >= at)
                return true;
        return false;
    };

    // Addresses.
    if (q == QLatin1String("ip") || asks({"my ip", "ip address", "public ip", "local ip"}, 85)) {
        for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
            if (!(iface.flags() & QNetworkInterface::IsUp) || iface.flags() & QNetworkInterface::IsLoopBack)
                continue;
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                    continue;
                const QString ip = entry.ip().toString();
                Item item = card(QStringLiteral("ip-") + iface.name(), ip, tr("Local address · %1").arg(iface.humanReadableName()),
                                 QStringLiteral("network-wired"), 113);
                item.actions << Action{tr("Copy Address"), {}, {}, [l, ip] { l->copyText(ip); }};
                items << item;
            }
        }
        const QString ip = m_publicIp.value(QStringLiteral("ip")).toString();
        Item pub = card(QStringLiteral("ip-public"), ip.isEmpty() ? tr("Public address…") : ip, {}, QStringLiteral("network-workgroup"), 114);
        if (!ip.isEmpty()) {
            QStringList where;
            for (const char *key : {"city", "region", "country"})
                if (const QString v = m_publicIp.value(QLatin1String(key)).toString(); !v.isEmpty() && !where.contains(v))
                    where << v;
            pub.subtitle = tr("Public · %1 · %2").arg(where.join(QStringLiteral(", ")), m_publicIp.value(QStringLiteral("org")).toString());
            pub.actions << Action{tr("Copy Address"), {}, {}, [l, ip] { l->copyText(ip); }};
        } else {
            pub.subtitle = tr("Asking ipinfo.io");
        }
        items << pub;
    }

    // Speed test.
    if (asks({"speed test", "speedtest", "internet speed", "network speed", "bandwidth"}, 70)) {
        Item item = card(QStringLiteral("speed"), {}, {}, QStringLiteral("speedometer"), 114);
        auto ms = [this] {
            QList<double> sorted = m_pings;
            std::sort(sorted.begin(), sorted.end());
            return sorted.isEmpty() ? 0.0 : sorted.at(sorted.size() / 2);
        };
        switch (m_step) {
        case Step::Idle:
            item.title = tr("Run Speed Test");
            item.subtitle = tr("Latency, download and upload, against Cloudflare (uses about 35 MB)");
            break;
        case Step::Latency:
            item.title = tr("Measuring latency…");
            item.fraction = 0.05;
            break;
        case Step::Download:
            item.title = tr("Download %1…").arg(speedText(m_down));
            item.subtitle = tr("Latency %1 ms").arg(qRound(ms()));
            item.fraction = 0.1 + 0.5 * std::min(1.0, m_clock.elapsed() / 8000.0);
            break;
        case Step::Upload:
            item.title = tr("↓ %1 · Upload %2…").arg(speedText(m_down), speedText(m_up));
            item.subtitle = tr("Latency %1 ms").arg(qRound(ms()));
            item.fraction = 0.6 + 0.4 * std::min(1.0, m_clock.elapsed() / 8000.0);
            break;
        case Step::Done:
            item.title = m_speedError.isEmpty() ? tr("↓ %1   ↑ %2").arg(speedText(m_down), speedText(m_up)) : tr("Speed test failed");
            item.subtitle = m_speedError.isEmpty() ? tr("Latency %1 ms · Cloudflare").arg(qRound(ms())) : m_speedError;
            break;
        }
        if (m_step == Step::Idle || m_step == Step::Done) {
            item.actions << Action{m_step == Step::Done ? tr("Run Again") : tr("Run Speed Test"), {}, {}, [self] { self->runSpeedTest(); }, true};
            if (m_step == Step::Done && m_speedError.isEmpty()) {
                const QString result = item.title + QStringLiteral(" · ") + item.subtitle;
                item.actions << Action{tr("Copy Result"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::Key_Return), [l, result] { l->copyText(result); }};
            }
        }
        items << item;
    }

    // Lookups: the tool's own output.
    struct Tool {
        QStringList words;
        QString title;
        QString icon;
        std::function<QStringList(const QString &)> argv;
        const char *program;
    };
    const QList<Tool> tools{
        {{QStringLiteral("dns"), QStringLiteral("dig"), QStringLiteral("nslookup")}, tr("DNS records for %1"), QStringLiteral("network-server"),
         [](const QString &h) {
             QStringList a{QStringLiteral("dig"), QStringLiteral("+noall"), QStringLiteral("+answer")};
             for (const char *type : {"A", "AAAA", "CNAME", "MX", "NS", "TXT"})
                 a << h << QLatin1String(type);
             return a;
         },
         "dig"},
        {{QStringLiteral("whois")}, tr("Whois %1"), QStringLiteral("dialog-information"), [](const QString &h) { return QStringList{QStringLiteral("whois"), h}; }, "whois"},
        {{QStringLiteral("ping")}, tr("Ping %1"), QStringLiteral("network-transmit-receive"),
         [](const QString &h) { return QStringList{QStringLiteral("ping"), QStringLiteral("-c"), QStringLiteral("5"), h}; }, "ping"},
    };
    for (const Tool &t : tools) {
        const QString host = target(m_query, t.words);
        if (host.isEmpty() || !installed(t.program))
            continue;
        Item item = card(QStringLiteral("tool-") + t.words.first(), t.title.arg(host), tr("Runs %1").arg(t.argv(host).join(u' ')), t.icon, 115);
        item.id = QStringLiteral("lumen:net-") + t.words.first();
        const QString title = t.title.arg(host);
        const QStringList argv = t.argv(host);
        item.actions << Action{tr("Run"), {}, {}, [self, title, argv] { self->command(title, argv); }, true};
        items << item;
    }
    if (asks({"flush dns", "clear dns cache", "dns cache"}, 75) && installed("resolvectl")) {
        Item item = card(QStringLiteral("flush-dns"), tr("Flush DNS Cache"), tr("Forget looked-up addresses (systemd-resolved)"), QStringLiteral("edit-clear"), 105);
        item.id = QStringLiteral("lumen:flush-dns");
        item.actions << Action{tr("Flush"), {}, {}, [self] {
            self->quietly({QStringLiteral("resolvectl"), QStringLiteral("flush-caches")}, tr("DNS cache flushed"));
        }, true};
        items << item;
    }

    // VPN connections.
    const bool vpnAsked = asks({"vpn", "wireguard"}, 80);
    for (const Vpn &v : std::as_const(m_vpns)) {
        const qreal score = vpnAsked ? 104 : fuzzyScore(m_query, v.name) * 0.85;
        if (score < 40)
            continue;
        Item item;
        item.id = QStringLiteral("vpn:") + v.uuid;
        item.title = v.name;
        item.subtitle = v.active ? tr("Connected") : tr("Not connected");
        item.kind = v.type == QLatin1String("wireguard") ? tr("WireGuard") : tr("VPN");
        item.icon = QIcon::fromTheme(v.active ? QStringLiteral("network-vpn") : QStringLiteral("network-vpn-disconnected"),
                                     QIcon::fromTheme(QStringLiteral("network-vpn")));
        item.score = score;
        const QString uuid = v.uuid, name = v.name;
        const bool active = v.active;
        item.actions << Action{active ? tr("Disconnect") : tr("Connect"), {}, {}, [self, uuid, name, active] {
            self->quietly({QStringLiteral("nmcli"), QStringLiteral("connection"), active ? QStringLiteral("down") : QStringLiteral("up"), uuid},
                          active ? tr("%1 disconnected").arg(name) : tr("Connected to %1").arg(name), [self] { self->loadVpns(); });
        }, true};
        items << item;
    }

    // Tailscale.
    if (m_hasTailscale && !m_tailscale.isEmpty()) {
        const bool asked = asks({"tailscale", "tailnet", "exit node"}, 80) || q == QLatin1String("ts");
        const QString state = m_tailscale.value(QStringLiteral("BackendState")).toString();
        const bool running = state == QLatin1String("Running");
        const QJsonObject me = m_tailscale.value(QStringLiteral("Self")).toObject();
        const QList<Peer> peers = parseTailscale(m_tailscale);
        if (asked) {
            Item status = card(QStringLiteral("tailscale"), running ? tr("Tailscale: connected") : tr("Tailscale: %1").arg(state.isEmpty() ? tr("off") : state),
                               {}, QStringLiteral("network-vpn"), 112);
            const int online = int(std::count_if(peers.cbegin(), peers.cend(), [](const Peer &p) { return p.online; }));
            status.subtitle = running ? tr("%1 · %2 · %3 of %4 devices online")
                                            .arg(me.value(QStringLiteral("HostName")).toString(),
                                                 me.value(QStringLiteral("TailscaleIPs")).toArray().at(0).toString())
                                            .arg(online)
                                            .arg(peers.size())
                                      : tr("Enter connects");
            status.actions << Action{running ? tr("Disconnect") : tr("Connect"), {}, {}, [self, running] {
                self->quietly({QStringLiteral("tailscale"), running ? QStringLiteral("down") : QStringLiteral("up")},
                              running ? tr("Tailscale disconnected") : tr("Tailscale connected"), [self] { self->loadTailscale(); });
            }, true};
            if (std::any_of(peers.cbegin(), peers.cend(), [](const Peer &p) { return p.exitNode; }))
                status.actions << Action{tr("Stop Using Exit Node"), QStringLiteral("process-stop"), {}, [self] {
                    self->quietly({QStringLiteral("tailscale"), QStringLiteral("set"), QStringLiteral("--exit-node=")}, tr("Exit node off"),
                                  [self] { self->loadTailscale(); });
                }, true};
            items << status;
        }
        for (int i = 0; i < peers.size(); ++i) {
            const Peer &p = peers.at(i);
            const qreal score = asked ? 100 - i * 0.01 : (q.size() >= 3 ? fuzzyScore(m_query, p.name) * 0.8 : 0);
            if (score < 40)
                continue;
            Item item;
            item.id = QStringLiteral("tailscale:") + p.name;
            item.title = p.name;
            item.subtitle = QStringList{p.ip, p.os, p.online ? tr("online") : tr("offline")}.join(QStringLiteral(" · "));
            item.kind = p.exitNode ? tr("Exit Node") : tr("Tailscale");
            item.icon = QIcon::fromTheme(p.os == QLatin1String("android") || p.os == QLatin1String("iOS") ? QStringLiteral("smartphone")
                                                                                                         : QStringLiteral("computer"));
            item.score = score;
            const QString ip = p.ip, dns = p.dnsName, name = p.name;
            item.actions << Action{tr("Copy Address"), {}, {}, [l, ip] { l->copyText(ip); }};
            item.actions << Action{tr("SSH in Konsole"), QStringLiteral("utilities-terminal"), QKeySequence(Qt::CTRL | Qt::Key_T), [dns, ip] {
                QProcess::startDetached(QStringLiteral("konsole"), {QStringLiteral("-e"), QStringLiteral("ssh"), dns.isEmpty() ? ip : dns});
            }};
            if (!dns.isEmpty())
                item.actions << Action{tr("Copy MagicDNS Name"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                                       [l, dns] { l->copyText(dns); }};
            if (p.exitNodeOption && !p.exitNode)
                item.actions << Action{tr("Use as Exit Node"), QStringLiteral("network-vpn"), {}, [self, ip, name] {
                    self->quietly({QStringLiteral("tailscale"), QStringLiteral("set"), QStringLiteral("--exit-node=") + ip},
                                  tr("Traffic goes through %1").arg(name), [self] { self->loadTailscale(); });
                }, true};
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}
