#pragma once

#include "source.h"

#include <QHash>

// Everyday system commands in the main search:
//  - toggles: Wi-Fi (nmcli), Bluetooth (bluetoothctl), Do Not Disturb (the
//    notification server's inhibit API), dark/light colours
//    (plasma-apply-colorscheme), volume and mute (wpctl), ejecting drives
//    (Solid);
//  - media players (MPRIS): play/pause, next, previous, and what is playing;
//  - screenshots and screen recording (Spectacle);
//  - a colour picker (KWin).
// Their current state is read when the launcher opens, in the background.
class SystemSource : public Source
{
    Q_OBJECT
public:
    explicit SystemSource(Launcher *launcher);

    void query(const QString &text) override;
    void sessionStarted() override;

    // Shown on the start screen: what is playing, if anything.
    QList<Item> startItems() const;

private:
    struct Player {
        QString service;
        QString name;
        QString title;
        QString artist;
        bool playing = false;
    };

    QList<Item> commands() const;
    QList<Item> mediaItems() const;
    void readState();
    void readPlayers();
    void run(const QString &program, const QStringList &args, const QString &message, bool refresh = false);
    void mediaCall(const QString &service, const QString &method);
    void setDoNotDisturb(bool on);
    void switchColourScheme(bool dark);
    void pickColour();

    bool m_wifi = false, m_wifiKnown = false;
    bool m_bluetooth = false, m_bluetoothKnown = false;
    int m_volume = -1; // percent, -1 unknown
    bool m_muted = false;
    uint m_dndCookie = 0; // our notification inhibition, 0 if none
    QList<Player> m_players;
    QString m_lastQuery;
};
