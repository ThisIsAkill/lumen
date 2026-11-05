#pragma once

#include "source.h"

#include <QMap>
#include <QVariantMap>

using NMSettings = QMap<QString, QVariantMap>; // NetworkManager connection settings, a{sa{sv}}
Q_DECLARE_METATYPE(NMSettings)

// Desktop devices in the main search, read in the background when the
// launcher opens:
//  - sound outputs and microphones (pactl; PipeWire moves playing audio to
//    the new default): "headphones", "speakers", "output", "mic", names;
//  - paired Bluetooth devices (bluetoothctl): "bluetooth", "connect", names;
//  - Wi-Fi networks (nmcli): "wifi", "network", names. Saved networks connect
//    at once; for a new secured one Lumen asks for the password.
class DesktopSource : public Source
{
    Q_OBJECT
public:
    explicit DesktopSource(Launcher *launcher);

    void query(const QString &text) override;
    void sessionStarted() override;

private:
    struct AudioDevice {
        QString name;
        QString description;
        bool input = false;
        bool current = false;
    };
    struct BluetoothDevice {
        QString address;
        QString name;
        bool connected = false;
    };
    struct Network {
        QString ssid;
        int signal = 0;
        QString security;
        bool current = false;
        bool saved = false;
    };

    void readAudio();
    void readBluetooth();
    void readWifi();
    void run(const QString &program, const QStringList &args, const QString &doing, const QString &done, const QString &failed);
    void connectWifi(const Network &network);
    void joinWithPassword(const Network &network, const QString &password);
    QList<Item> audioItems(const QString &q) const;
    QList<Item> bluetoothItems(const QString &q) const;
    QList<Item> wifiItems(const QString &q) const;

    QList<AudioDevice> m_audio;
    QList<BluetoothDevice> m_bluetooth;
    QList<Network> m_networks;
    QString m_query;
};
