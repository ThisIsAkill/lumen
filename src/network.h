#pragma once

#include "source.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;

// Network tools in the main search:
//  - "ip", "my ip": this computer's addresses, and the public one with its
//    place and provider (ipinfo.io);
//  - "speed test": latency, download and upload, measured against
//    Cloudflare's speed test servers (no program to install);
//  - "dns example.com", "whois example.com", "ping example.com": the tool's
//    output in a view; "flush dns";
//  - "vpn": NetworkManager's VPN and WireGuard connections, on and off;
//  - "shorten" (the copied address) or "shorten URL": an is.gd link, copied;
//  - "tailscale" (or a device's name): the tailnet's devices, to copy an
//    address, SSH in or use one as the exit node; connect and disconnect.
class NetworkSource : public Source
{
    Q_OBJECT
public:
    explicit NetworkSource(Launcher *launcher);

    void query(const QString &text) override;
    void sessionStarted() override;

    // "1234567" bits per second → "1.2 Mbps".
    static QString speedText(double bitsPerSecond);
    // Peers from `tailscale status --json` (tested without Tailscale).
    struct Peer {
        QString name;
        QString dnsName;
        QString ip;
        QString os;
        bool online = false;
        bool exitNodeOption = false;
        bool exitNode = false;
    };
    static QList<Peer> parseTailscale(const QJsonObject &status);

private:
    void publish();
    void loadPublicIp();
    void loadVpns();
    void loadTailscale();
    void runSpeedTest();
    void speedStep();
    void command(const QString &title, const QStringList &argv); // output in a view
    void quietly(const QStringList &argv, const QString &done, std::function<void()> then = {});
    void shorten(const QString &url);
    QHash<QString, QString> m_short; // long → short address, this session

    QNetworkAccessManager *m_network;
    QString m_query;

    QJsonObject m_publicIp;
    QDateTime m_publicIpAt;

    struct Vpn {
        QString name, uuid, type;
        bool active = false;
    };
    QList<Vpn> m_vpns;
    QJsonObject m_tailscale;
    bool m_hasTailscale = false;
    bool m_loaded = false; // VPNs and tailnet read since Lumen opened

    // The speed test, in steps: latency, download, upload.
    enum class Step { Idle, Latency, Download, Upload, Done } m_step = Step::Idle;
    QList<double> m_pings; // ms
    double m_down = 0, m_up = 0; // bits per second
    QString m_speedError;
    QElapsedTimer m_clock;
    QPointer<QNetworkReply> m_speedReply;
};
