#pragma once

#include "source.h"

#include <QJsonArray>

// Screens, in the main search:
//  - "brightness", "brightness 60", "brightness up/down": every screen
//    Plasma can dim (built-in and, through DDC, external), with Plasma's
//    own on-screen indicator;
//  - "resolution" / "refresh rate": each screen's modes; a new mode is
//    kept only when confirmed within 15 seconds, otherwise it goes back;
//  - "Save Display Layout": the screens' modes, places, scale and rotation
//    under a name (this computer only); typing the name restores it;
//  - "night light": pause or resume Night Light.
// The screens' setup comes from kscreen-doctor; brightness from
// org.kde.ScreenBrightness; Night Light from KWin.
class DisplaySource : public Source
{
    Q_OBJECT
public:
    explicit DisplaySource(Launcher *launcher);
    ~DisplaySource() override;

    void query(const QString &text) override;
    void sessionStarted() override;

    // kscreen-doctor arguments that recreate a setup (`kscreen-doctor -j`'s
    // "outputs"), tested without screens.
    static QStringList profileArguments(const QJsonArray &outputs);

private:
    void loadOutputs();
    void loadBrightness();
    void setBrightness(int percent); // every screen; -1/-2: a step up/down
    void applyMode(const QString &output, const QString &mode, const QString &previous);
    void saveProfile(const QString &name);
    void publish();

    QJsonArray m_outputs;
    struct Screen {
        QString path;
        QString label;
        int brightness = 0;
        int max = 0;
    };
    QList<Screen> m_screens;
    uint m_nightCookie = 0; // Night Light paused by Lumen
    QString m_query;
    bool m_loaded = false; // screens read since Lumen opened
};

// KDE Connect, in the main search: "phone", "ring my phone", a device's name.
// Each paired, reachable device with its battery; ring it, send it the
// clipboard, ping it, or send a text message.
class PhoneSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
    void sessionStarted() override;

private:
    void load();
    void run(const QString &id, const QStringList &args, const QString &done);
    struct Device {
        QString id;
        QString name;
        int battery = -1;
        bool charging = false;
    };
    QList<Device> m_devices;
    QString m_query;
    bool m_loaded = false;
};
