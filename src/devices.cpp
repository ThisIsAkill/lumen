#include "devices.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace
{

constexpr int ConfirmSeconds = 15; // a new screen mode goes back unless kept
constexpr int MaxModes = 12;       // per screen
const QString BrightnessService = QStringLiteral("org.kde.ScreenBrightness");

KConfigGroup profiles()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("DisplayProfiles"));
}

bool installed(const char *program)
{
    return !QStandardPaths::findExecutable(QLatin1String(program)).isEmpty();
}

QDBusMessage nightLight(const QString &method, const QVariantList &args = {})
{
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/NightLight"),
                                              QStringLiteral("org.kde.KWin.NightLight"), method);
    msg.setArguments(args);
    return msg;
}

QString rotationName(int rotation)
{
    // KScreen's values: 1 none, 2 left, 4 inverted, 8 right.
    switch (rotation) {
    case 2:
        return QStringLiteral("left");
    case 4:
        return QStringLiteral("inverted");
    case 8:
        return QStringLiteral("right");
    default:
        return QStringLiteral("none");
    }
}

} // namespace

DisplaySource::DisplaySource(Launcher *launcher)
    : Source(launcher)
{
}

DisplaySource::~DisplaySource()
{
    if (m_nightCookie)
        QDBusConnection::sessionBus().call(nightLight(QStringLiteral("uninhibit"), {m_nightCookie}), QDBus::NoBlock);
}

QStringList DisplaySource::profileArguments(const QJsonArray &outputs)
{
    QStringList args;
    for (const QJsonValue &v : outputs) {
        const QJsonObject o = v.toObject();
        if (!o.value(QStringLiteral("connected")).toBool())
            continue;
        const QString name = QStringLiteral("output.") + o.value(QStringLiteral("name")).toString();
        if (!o.value(QStringLiteral("enabled")).toBool()) {
            args << name + QStringLiteral(".disable");
            continue;
        }
        args << name + QStringLiteral(".enable");
        // The mode by its name ("2560x1440@165"): ids change between sessions.
        const QString current = o.value(QStringLiteral("currentModeId")).toString();
        for (const QJsonValue &m : o.value(QStringLiteral("modes")).toArray())
            if (m.toObject().value(QStringLiteral("id")).toString() == current)
                args << name + QStringLiteral(".mode.") + m.toObject().value(QStringLiteral("name")).toString();
        const QJsonObject pos = o.value(QStringLiteral("pos")).toObject();
        args << name + QStringLiteral(".position.%1,%2").arg(pos.value(QStringLiteral("x")).toInt()).arg(pos.value(QStringLiteral("y")).toInt());
        args << name + QStringLiteral(".scale.") + QString::number(o.value(QStringLiteral("scale")).toDouble());
        args << name + QStringLiteral(".rotation.") + rotationName(o.value(QStringLiteral("rotation")).toInt(1));
        if (const int priority = o.value(QStringLiteral("priority")).toInt(); priority > 0)
            args << name + QStringLiteral(".priority.%1").arg(priority);
    }
    return args;
}

void DisplaySource::sessionStarted()
{
    m_loaded = false; // read again once something is typed
}

void DisplaySource::loadOutputs()
{
    if (!installed("kscreen-doctor"))
        return;
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc] {
        proc->deleteLater();
        m_outputs = QJsonDocument::fromJson(proc->readAllStandardOutput()).object().value(QStringLiteral("outputs")).toArray();
        if (!m_query.isEmpty())
            publish();
    });
    proc->start(QStringLiteral("kscreen-doctor"), {QStringLiteral("-j")});
    QTimer::singleShot(4000, proc, [proc] { proc->kill(); });
}

void DisplaySource::loadBrightness()
{
    m_screens.clear();
    QDBusInterface root(BrightnessService, QStringLiteral("/org/kde/ScreenBrightness"), BrightnessService);
    if (!root.isValid())
        return;
    for (const QString &name : root.property("DisplaysDBusNames").toStringList()) {
        const QString path = QStringLiteral("/org/kde/ScreenBrightness/") + name;
        QDBusInterface display(BrightnessService, path, BrightnessService + QStringLiteral(".Display"));
        if (!display.isValid())
            continue;
        m_screens << Screen{path, display.property("Label").toString(), display.property("Brightness").toInt(), display.property("MaxBrightness").toInt()};
    }
}

void DisplaySource::setBrightness(int percent)
{
    for (Screen &s : m_screens) {
        if (s.max <= 0)
            continue;
        int value = percent >= 0 ? s.max * percent / 100 : s.brightness + (percent == -1 ? 1 : -1) * std::max(1, s.max / 10);
        value = std::clamp(value, 0, s.max);
        auto msg = QDBusMessage::createMethodCall(BrightnessService, s.path, BrightnessService + QStringLiteral(".Display"), QStringLiteral("SetBrightness"));
        msg.setArguments({value, 0u}); // flags 0: Plasma shows its indicator
        QDBusConnection::sessionBus().asyncCall(msg);
        s.brightness = value;
    }
    publish();
}

void DisplaySource::applyMode(const QString &output, const QString &mode, const QString &previous)
{
    QProcess::startDetached(QStringLiteral("kscreen-doctor"), {QStringLiteral("output.%1.mode.%2").arg(output, mode)});
    // Like System Settings: the new mode stays only if it is confirmed.
    Launcher *l = launcher();
    Item shown;
    shown.title = tr("%1 at %2").arg(output, mode);
    shown.subtitle = tr("Goes back to %1 in %2 seconds").arg(previous).arg(ConfirmSeconds);
    shown.icon = QIcon::fromTheme(QStringLiteral("video-display"));
    auto kept = std::make_shared<bool>(false);
    l->ask(tr("Keep?"), tr("Press Enter to keep this mode"), shown, false, [kept, l](const QString &) {
        *kept = true;
        l->notify(tr("Mode kept"));
    });
    QTimer::singleShot(ConfirmSeconds * 1000, this, [this, kept, output, previous, l] {
        if (*kept)
            return;
        QProcess::startDetached(QStringLiteral("kscreen-doctor"), {QStringLiteral("output.%1.mode.%2").arg(output, previous)});
        l->notify(tr("Back to %1").arg(previous));
        loadOutputs();
    });
}

void DisplaySource::saveProfile(const QString &name)
{
    const QStringList args = profileArguments(m_outputs);
    if (args.isEmpty()) {
        launcher()->notify(tr("No screens to save"));
        return;
    }
    KConfigGroup g = profiles();
    g.writeEntry(name, args);
    g.sync();
    launcher()->notify(tr("Saved “%1”").arg(name));
}

void DisplaySource::query(const QString &text)
{
    m_query = text.trimmed();
    if (!m_loaded && !m_query.isEmpty()) {
        // Only once typing starts, not every time Lumen opens.
        m_loaded = true;
        loadOutputs();
        loadBrightness();
    }
    publish();
}

void DisplaySource::publish()
{
    QList<Item> items;
    const QString q = m_query.toLower();
    if (q.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    Launcher *l = launcher();
    auto self = this;

    // Brightness: "brightness", "brightness 40", "brightness up".
    static const QRegularExpression brightness(QStringLiteral(R"(^(?:brightness|dim|brighter|darker)(?:\s+(up|down|\d{1,3})%?)?$)"));
    const auto bm = brightness.match(q);
    if ((bm.hasMatch() || fuzzyScore(q, QStringLiteral("brightness")) >= 80) && !m_screens.isEmpty()) {
        const QString arg = bm.captured(1);
        if (!arg.isEmpty() || q == QLatin1String("brighter") || q == QLatin1String("darker")) {
            const int percent = arg == QLatin1String("up") || q == QLatin1String("brighter") ? -1
                : arg == QLatin1String("down") || q == QLatin1String("darker")              ? -2
                                                                                            : std::clamp(arg.toInt(), 0, 100);
            Item item;
            item.id = QStringLiteral("answer:brightness-set");
            item.title = percent == -1 ? tr("Brighter") : percent == -2 ? tr("Darker") : tr("Brightness %1%").arg(percent);
            item.subtitle = tr("Every screen");
            item.kind = tr("Display");
            item.icon = QIcon::fromTheme(QStringLiteral("brightness-high"), QIcon::fromTheme(QStringLiteral("video-display")));
            item.score = 115;
            item.actions << Action{tr("Set"), {}, {}, [self, percent] { self->setBrightness(percent); }, true};
            items << item;
        }
        for (const Screen &s : std::as_const(m_screens)) {
            Item item;
            item.id = QStringLiteral("answer:brightness-") + s.path.section(u'/', -1);
            const int percent = s.max > 0 ? qRound(100.0 * s.brightness / s.max) : 0;
            item.title = tr("%1 · %2%").arg(s.label.isEmpty() ? tr("Screen") : s.label).arg(percent);
            item.subtitle = tr("Brightness · Enter brighter, Ctrl+Enter darker");
            item.kind = tr("Display");
            item.icon = QIcon::fromTheme(QStringLiteral("brightness-high"), QIcon::fromTheme(QStringLiteral("video-display")));
            item.fraction = s.max > 0 ? double(s.brightness) / s.max : -1;
            item.score = 112;
            item.actions << Action{tr("Brighter"), {}, {}, [self] { self->setBrightness(-1); }, true};
            item.actions << Action{tr("Darker"), QStringLiteral("brightness-low"), QKeySequence(Qt::CTRL | Qt::Key_Return), [self] { self->setBrightness(-2); }, true};
            items << item;
        }
    }

    // Screen modes.
    const qreal modeScore = std::max({fuzzyScore(q, QStringLiteral("resolution")), fuzzyScore(q, QStringLiteral("refresh rate")),
                                      fuzzyScore(q, QStringLiteral("display mode")), fuzzyScore(q, QStringLiteral("screen resolution"))});
    if (modeScore >= 75) {
        for (const QJsonValue &v : std::as_const(m_outputs)) {
            const QJsonObject o = v.toObject();
            if (!o.value(QStringLiteral("enabled")).toBool())
                continue;
            const QString output = o.value(QStringLiteral("name")).toString();
            const QString currentId = o.value(QStringLiteral("currentModeId")).toString();
            QString current;
            struct Mode {
                QString name;
                int width, height;
                double rate;
            };
            QList<Mode> modes;
            QSet<QString> seen;
            for (const QJsonValue &m : o.value(QStringLiteral("modes")).toArray()) {
                const QJsonObject mo = m.toObject();
                const QString name = mo.value(QStringLiteral("name")).toString();
                if (mo.value(QStringLiteral("id")).toString() == currentId)
                    current = name;
                if (seen.contains(name))
                    continue;
                seen.insert(name);
                const QJsonObject size = mo.value(QStringLiteral("size")).toObject();
                modes << Mode{name, size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt(),
                              mo.value(QStringLiteral("refreshRate")).toDouble()};
            }
            std::sort(modes.begin(), modes.end(), [](const Mode &a, const Mode &b) {
                return a.width * a.height != b.width * b.height ? a.width * a.height > b.width * b.height : a.rate > b.rate;
            });
            for (int i = 0; i < modes.size() && i < MaxModes; ++i) {
                const Mode &m = modes.at(i);
                Item item;
                item.id = QStringLiteral("display-mode:%1:%2").arg(output, m.name);
                item.title = tr("%1 × %2 at %3 Hz").arg(m.width).arg(m.height).arg(qRound(m.rate));
                item.subtitle = output;
                item.kind = m.name == current ? tr("Current") : tr("Display");
                item.icon = QIcon::fromTheme(QStringLiteral("video-display"));
                item.score = 104 - i * 0.01;
                const QString mode = m.name, previous = current;
                if (m.name != current)
                    item.actions << Action{tr("Use This Mode"), {}, {}, [self, output, mode, previous] { self->applyMode(output, mode, previous); }, true};
                items << item;
            }
        }
    }

    // Saved layouts.
    const KConfigGroup saved = profiles();
    for (const QString &name : saved.keyList()) {
        const qreal score = std::max(fuzzyScore(q, name), fuzzyScore(q, name + QStringLiteral(" display")) * 0.9);
        if (score < 50)
            continue;
        Item item;
        item.id = QStringLiteral("display-profile:") + name;
        item.title = name;
        item.subtitle = tr("Display layout");
        item.kind = tr("Display");
        item.icon = QIcon::fromTheme(QStringLiteral("preferences-desktop-display"));
        item.score = score;
        const QStringList args = saved.readEntry(name, QStringList());
        item.actions << Action{tr("Apply Layout"), {}, {}, [args, l, name] {
            QProcess::startDetached(QStringLiteral("kscreen-doctor"), args);
            l->notify(tr("Display layout “%1”").arg(name));
        }};
        item.actions << Action{tr("Delete Layout"), QStringLiteral("edit-delete"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), [self, name] {
            KConfigGroup g = profiles();
            g.deleteEntry(name);
            g.sync();
            self->publish();
        }, true};
        items << item;
    }
    if (std::max(fuzzyScore(q, QStringLiteral("Save Display Layout")), fuzzyScore(q, QStringLiteral("display profile"))) >= 60 && !m_outputs.isEmpty()) {
        Item item;
        item.id = QStringLiteral("lumen:save-display-layout");
        item.title = tr("Save Display Layout");
        item.subtitle = tr("Remember the screens' resolution, places, scale and rotation");
        item.kind = tr("Display");
        item.icon = QIcon::fromTheme(QStringLiteral("document-save"));
        item.score = 103;
        const Item shown = item;
        item.actions << Action{tr("Name It"), {}, {}, [l, self, shown] {
            l->ask(tr("Layout"), tr("Name this display layout (e.g. Desk), then press Enter"), shown, false, [self](const QString &typed) {
                if (!typed.trimmed().isEmpty())
                    self->saveProfile(typed.trimmed());
            });
        }, true};
        items << item;
    }

    // Night Light.
    if (fuzzyScore(q, QStringLiteral("night light")) >= 75 || fuzzyScore(q, QStringLiteral("blue light")) >= 80) {
        QDBusInterface night(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/NightLight"), QStringLiteral("org.kde.KWin.NightLight"));
        if (night.isValid() && night.property("available").toBool()) {
            const bool enabled = night.property("enabled").toBool();
            Item item;
            item.id = QStringLiteral("lumen:night-light");
            item.kind = tr("Display");
            item.icon = QIcon::fromTheme(QStringLiteral("redshift-status-on"), QIcon::fromTheme(QStringLiteral("weather-clear-night")));
            item.score = 108;
            if (!enabled) {
                item.title = tr("Night Light is off");
                item.subtitle = tr("Turn it on in System Settings → Display & Monitor");
            } else if (m_nightCookie) {
                item.title = tr("Resume Night Light");
                item.subtitle = tr("Paused by Lumen");
                item.actions << Action{tr("Resume"), {}, {}, [self, l] {
                    QDBusConnection::sessionBus().call(nightLight(QStringLiteral("uninhibit"), {self->m_nightCookie}));
                    self->m_nightCookie = 0;
                    l->notify(tr("Night Light resumed"));
                }};
            } else {
                item.title = tr("Pause Night Light");
                item.subtitle = tr("%1 K now").arg(night.property("currentTemperature").toUInt());
                item.actions << Action{tr("Pause"), {}, {}, [self, l] {
                    const QDBusReply<uint> cookie = QDBusConnection::sessionBus().call(nightLight(QStringLiteral("inhibit")));
                    if (cookie.isValid())
                        self->m_nightCookie = cookie.value();
                    l->notify(cookie.isValid() ? tr("Night Light paused") : tr("Could not pause Night Light"));
                }};
            }
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}

// ------------------------------------------------------------- phones

void PhoneSource::sessionStarted()
{
    m_loaded = false;
}

void PhoneSource::load()
{
    if (!installed("kdeconnect-cli"))
        return;
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc] {
        proc->deleteLater();
        m_devices.clear();
        // "id name" per line.
        for (const QString &line : QString::fromUtf8(proc->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts)) {
            Device d;
            d.id = line.section(u' ', 0, 0);
            d.name = line.section(u' ', 1);
            QDBusInterface battery(QStringLiteral("org.kde.kdeconnect"), QStringLiteral("/modules/kdeconnect/devices/%1/battery").arg(d.id),
                                   QStringLiteral("org.kde.kdeconnect.device.battery"));
            if (battery.isValid()) {
                d.battery = battery.property("charge").toInt();
                d.charging = battery.property("isCharging").toBool();
            }
            m_devices << d;
        }
        if (!m_query.isEmpty())
            query(m_query);
    });
    proc->start(QStringLiteral("kdeconnect-cli"), {QStringLiteral("-a"), QStringLiteral("--id-name-only")});
    QTimer::singleShot(4000, proc, [proc] { proc->kill(); });
}

void PhoneSource::run(const QString &id, const QStringList &args, const QString &done)
{
    auto *proc = new QProcess(this);
    Launcher *l = launcher();
    connect(proc, &QProcess::finished, this, [proc, l, done](int code) {
        proc->deleteLater();
        l->notify(code == 0 ? done : QString::fromUtf8(proc->readAllStandardError()).trimmed().section(u'\n', 0, 0));
    });
    proc->start(QStringLiteral("kdeconnect-cli"), QStringList{QStringLiteral("-d"), id} + args);
}

void PhoneSource::query(const QString &text)
{
    m_query = text.trimmed();
    if (!m_loaded && !m_query.isEmpty()) {
        m_loaded = true;
        load(); // the answer calls query() again
    }
    const QString q = m_query.toLower();
    QList<Item> items;
    if (q.isEmpty() || m_devices.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    const bool ring = q.contains(QLatin1String("ring")) || q.contains(QLatin1String("find my"));
    const qreal asked = std::max({fuzzyScore(q, QStringLiteral("phone")), fuzzyScore(q, QStringLiteral("kde connect")),
                                  fuzzyScore(q, QStringLiteral("find my phone")), fuzzyScore(q, QStringLiteral("ring my phone")),
                                  fuzzyScore(q, QStringLiteral("send to phone"))});
    auto self = this;
    Launcher *l = launcher();
    for (const Device &d : std::as_const(m_devices)) {
        const qreal score = std::max(asked, fuzzyScore(q, d.name));
        if (score < 60)
            continue;
        Item item;
        item.id = QStringLiteral("phone:") + d.id;
        item.title = ring ? tr("Ring %1").arg(d.name) : d.name;
        item.subtitle = d.battery >= 0 ? (d.charging ? tr("Battery %1% · charging").arg(d.battery) : tr("Battery %1%").arg(d.battery)) : tr("Connected");
        item.kind = tr("KDE Connect");
        item.icon = QIcon::fromTheme(QStringLiteral("smartphone"));
        item.fraction = d.battery >= 0 ? d.battery / 100.0 : -1;
        item.score = 106 + score / 20;
        const QString id = d.id, name = d.name;
        Action ringAction{tr("Ring"), QStringLiteral("audio-volume-high"), QKeySequence(Qt::CTRL | Qt::Key_R),
                          [self, id, name] { self->run(id, {QStringLiteral("--ring")}, tr("Ringing %1").arg(name)); }};
        Action clipboard{tr("Send Clipboard"), QStringLiteral("edit-paste"), QKeySequence(Qt::CTRL | Qt::Key_Return),
                         [self, id, name] { self->run(id, {QStringLiteral("--send-clipboard")}, tr("Clipboard sent to %1").arg(name)); }};
        if (ring)
            item.actions << ringAction << clipboard;
        else
            item.actions << clipboard << ringAction;
        item.actions << Action{tr("Ping"), QStringLiteral("dialog-information"), {},
                               [self, id, name] { self->run(id, {QStringLiteral("--ping")}, tr("Pinged %1").arg(name)); }};
        Item shown = item;
        shown.actions.clear();
        Argument number;
        number.name = QStringLiteral("number");
        number.placeholder = tr("Phone number");
        Argument message;
        message.name = QStringLiteral("message");
        message.placeholder = tr("Message");
        Action sms{tr("Send Text Message…"), QStringLiteral("mail-message-new"), QKeySequence(Qt::CTRL | Qt::Key_M), [l, self, id, shown, number, message] {
            l->askArguments(shown, {number, message}, {}, [self, id](const QStringList &values) {
                self->run(id, {QStringLiteral("--send-sms"), values.value(1), QStringLiteral("--destination"), values.value(0)}, tr("Message sent"));
            });
        }, true};
        sms.asksArguments = true;
        item.actions << sms;
        items << item;
    }
    Q_EMIT resultsReady(items);
}
