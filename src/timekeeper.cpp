#include "timekeeper.h"
#include "timeparse.h"

#include <chrome.h>
#include <motion.h>

#include <LayerShellQt/Window>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QWidget>
#include <QWindow>

#include <algorithm>

namespace
{

constexpr int FocusMinutes = 25, BreakMinutes = 5, LongBreakMinutes = 15;
constexpr int BadgeShadow = 10; // room around the pill for its shadow

bool onWayland()
{
    return QGuiApplication::platformName() == QLatin1String("wayland");
}

} // namespace

// The countdown pill in the top-right corner of the screen: a glass pill in
// the family of the launcher's messages, with a ring that empties as the
// timer runs, the timer's label and the time left in equal-width digits.
// It never takes focus: the user is working in some other window.
//
// It drops in from the screen edge and its picture shrinks away when the
// last timer ends (dismiss()). The ring moves smoothly when the time left
// jumps (time added, another timer taking over); the second-by-second
// steps are too small to see and are not animated, so a running timer
// repaints once a second and no more.
class TimerBadge : public QWidget
{
public:
    struct State {
        QString label;      // "Focus", "tea", or empty
        QString time;       // "12:34"
        qreal progress = 0; // 0–1 of the timer elapsed
        bool paused = false;
        bool pomodoro = false;
        int more = 0; // other timers running besides this one
    };

    TimerBadge()
        : m_reveal(this)
        , m_ring(this)
    {
        m_reveal.onStep = [this](qreal) { update(); };
        m_ring.onStep = [this](qreal) { update(); };
        setAttribute(Qt::WA_TranslucentBackground);
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFont(Chrome::scaledFont(font(), 1.0, QFont::Medium));
        if (onWayland()) {
            create();
            auto *layer = LayerShellQt::Window::get(windowHandle());
            layer->setLayer(LayerShellQt::Window::LayerOverlay);
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
            layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorRight));
            layer->setMargins({0, 12, 12, 0});
            layer->setWantsToBeOnActiveScreen(true);
            layer->setScope(QStringLiteral("lumen-timer"));
        }
    }

    void setState(const State &state)
    {
        // The width only follows the parts that change rarely (label, count),
        // so the pill does not jitter as the digits count down.
        const bool relayout = state.label != m_state.label || state.more != m_state.more || state.paused != m_state.paused;
        if (!isVisible() || qAbs(state.progress - m_ring.value()) < 0.02)
            m_ring.jump(state.progress); // a tick of the clock
        else
            m_ring.to(state.progress, Motion::Pulse, Motion::entrance()); // a jump worth seeing
        m_state = state;
        const QFontMetrics fm(font());
        const QFontMetrics timeFm(timeFont());
        int width = PadLeft + 2 * RingRadius + Gap + timeFm.horizontalAdvance(state.time) + PadRight;
        if (!state.label.isEmpty())
            width += fm.horizontalAdvance(state.label) + Gap;
        if (state.more > 0)
            width += Gap + fm.horizontalAdvance(moreText()) + 14;
        const QSize wanted(width + 2 * BadgeShadow, qMax(fm.height() + 14, 2 * RingRadius + 12) + 2 * BadgeShadow);
        if (relayout || wanted.width() > this->width() || wanted.height() != height())
            resize(wanted);
        if (!onWayland()) {
            const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
            move(screen.right() - this->width() - 12, screen.top() + 12);
        }
        update();
    }

    // Hides the badge, leaving its picture to shrink away.
    void dismiss()
    {
        if (!isVisible())
            return;
        Ghost::dismiss(this, rect());
        hide();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        m_reveal.start(0, 1, Motion::Toast, Motion::spring()); // drops in from the top edge
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal reveal = m_reveal.value();
        p.setOpacity(reveal);
        p.translate(0, -(1 - reveal) * Drop);
        const QPalette pal = palette();
        const bool dark = Chrome::isDark(pal);
        // The badge is never the active window, and Breeze fades the
        // inactive highlight: take the accent from the active group.
        const QColor accent = pal.color(QPalette::Active, QPalette::Highlight);
        const QRectF body = QRectF(rect()).adjusted(BadgeShadow + 0.5, BadgeShadow + 0.5, -BadgeShadow - 0.5, -BadgeShadow - 0.5);
        const qreal radius = body.height() / 2;

        // The glass: the same body as the launcher's "Copied" message.
        Chrome::drawShadow(p, body, radius, BadgeShadow - 2, dark ? 120 : 80, 2);
        const QColor base = Chrome::withAlpha(Chrome::mix(pal.color(QPalette::Window), pal.color(QPalette::Base), dark ? 0.0 : 0.3), 246);
        p.setPen(Chrome::withAlpha(dark ? Qt::white : Qt::black, dark ? 36 : 40));
        p.setBrush(Chrome::mix(base, Qt::white, dark ? 0.05 : 0.2));
        p.drawRoundedRect(body, radius, radius);
        QLinearGradient beam(body.topLeft(), body.topRight());
        beam.setColorAt(0.0, Qt::transparent);
        beam.setColorAt(0.5, Chrome::withAlpha(accent, dark ? 200 : 170));
        beam.setColorAt(1.0, Qt::transparent);
        p.save();
        p.setClipRect(QRectF(body.left(), body.top() - 1, body.width(), radius + 2));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QBrush(beam), 1.5));
        p.drawRoundedRect(body.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
        p.restore();

        // The ring: a faint track with the time left drawn clockwise from the
        // top in the accent colour; paused, it dims and shows a pause mark.
        const QPointF c(body.left() + PadLeft + RingRadius, body.center().y());
        const QRectF ring(c.x() - RingRadius + 1.2, c.y() - RingRadius + 1.2, 2 * (RingRadius - 1.2), 2 * (RingRadius - 1.2));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Chrome::withAlpha(pal.color(QPalette::WindowText), dark ? 50 : 36), 2.2));
        p.drawEllipse(ring);
        const qreal left = std::clamp(1.0 - m_ring.value(), 0.0, 1.0);
        p.setPen(QPen(m_state.paused ? Chrome::withAlpha(accent, 120) : accent, 2.2, Qt::SolidLine, Qt::FlatCap));
        p.drawArc(ring, 90 * 16, -int(left * 360 * 16));
        p.setPen(Qt::NoPen);
        p.setBrush(m_state.paused ? Chrome::mutedText(pal) : accent);
        if (m_state.paused) {
            p.drawRoundedRect(QRectF(c.x() - 2.6, c.y() - 2.8, 1.8, 5.6), 0.7, 0.7);
            p.drawRoundedRect(QRectF(c.x() + 0.8, c.y() - 2.8, 1.8, 5.6), 0.7, 0.7);
        } else if (m_state.pomodoro) {
            p.drawEllipse(c, 2.4, 2.4); // the tomato
        }

        // Label, countdown, and how many more are running.
        qreal x = c.x() + RingRadius + Gap;
        const QFontMetrics fm(font());
        if (!m_state.label.isEmpty()) {
            p.setFont(font());
            p.setPen(Chrome::mutedText(pal));
            p.drawText(QRectF(x, body.top(), fm.horizontalAdvance(m_state.label), body.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       m_state.label);
            x += fm.horizontalAdvance(m_state.label) + Gap;
        }
        const QFont tf = timeFont();
        p.setFont(tf);
        p.setPen(m_state.paused ? Chrome::mutedText(pal) : pal.color(QPalette::WindowText));
        const int timeW = QFontMetrics(tf).horizontalAdvance(m_state.time);
        p.drawText(QRectF(x, body.top(), timeW, body.height()), Qt::AlignVCenter | Qt::AlignLeft, m_state.time);
        x += timeW + Gap;
        if (m_state.more > 0) {
            p.setFont(font());
            const int chipH = Chrome::chipHeight(font());
            Chrome::drawChips(p, QRect(int(x), int(body.center().y() - chipH / 2.0 + 0.5), int(body.right() - x), chipH), {moreText()}, pal);
        }
    }

private:
    static constexpr int PadLeft = 10;
    static constexpr int PadRight = 14;
    static constexpr int Gap = 7;
    static constexpr int RingRadius = 9;
    static constexpr int Drop = 8; // how far above its place the pill starts

    QFont timeFont() const
    {
        QFont f = Chrome::scaledFont(font(), 1.0, QFont::DemiBold);
        f.setFeature(QFont::Tag("tnum"), 1); // digits of equal width
        return f;
    }

    QString moreText() const { return QStringLiteral("+%1").arg(m_state.more); }

    State m_state;
    Tween m_reveal; // 0 hidden → 1 in place
    Tween m_ring;   // the progress the ring shows
};

int Timekeeper::Timer::secondsLeft() const
{
    if (pausedLeft >= 0)
        return pausedLeft;
    return int(qMax<qint64>(0, QDateTime::currentDateTime().secsTo(end)));
}

Timekeeper::Timekeeper(QObject *parent)
    : QObject(parent)
    , m_badge(new TimerBadge)
{
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &Timekeeper::tick);
    m_reminderTimer.setSingleShot(true);
    connect(&m_reminderTimer, &QTimer::timeout, this, &Timekeeper::scheduleReminders);
    load();
    update();
    scheduleReminders(); // rings any that came due while Lumen was not running
}

Timekeeper::~Timekeeper()
{
    delete m_badge;
}

QString Timekeeper::file()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/lumen/timekeeping.json");
}

void Timekeeper::startTimer(int seconds, const QString &label)
{
    Timer t;
    t.id = m_nextId++;
    t.label = label;
    t.total = seconds;
    t.end = QDateTime::currentDateTime().addSecs(seconds);
    m_timers << t;
    update();
}

void Timekeeper::startPomodoro()
{
    Timer t;
    t.id = m_nextId++;
    t.label = tr("Focus");
    t.total = FocusMinutes * 60;
    t.end = QDateTime::currentDateTime().addSecs(t.total);
    t.pomodoro = true;
    m_timers << t;
    update();
}

void Timekeeper::stopTimer(int id)
{
    m_timers.removeIf([id](const Timer &t) { return t.id == id; });
    update();
}

void Timekeeper::setPaused(int id, bool paused)
{
    for (Timer &t : m_timers) {
        if (t.id != id || paused == (t.pausedLeft >= 0))
            continue;
        if (paused) {
            t.pausedLeft = t.secondsLeft();
        } else {
            t.end = QDateTime::currentDateTime().addSecs(t.pausedLeft);
            t.pausedLeft = -1;
        }
    }
    update();
}

void Timekeeper::addTime(int id, int seconds)
{
    for (Timer &t : m_timers) {
        if (t.id != id)
            continue;
        t.total += seconds;
        if (t.pausedLeft >= 0)
            t.pausedLeft += seconds;
        else
            t.end = t.end.addSecs(seconds);
    }
    update();
}

void Timekeeper::addReminder(const QDateTime &when, const QString &text)
{
    m_reminders << Reminder{m_nextId++, when, text};
    std::sort(m_reminders.begin(), m_reminders.end(), [](const Reminder &a, const Reminder &b) { return a.when < b.when; });
    save();
    scheduleReminders();
    Q_EMIT changed();
}

void Timekeeper::removeReminder(int id)
{
    m_reminders.removeIf([id](const Reminder &r) { return r.id == id; });
    save();
    scheduleReminders();
    Q_EMIT changed();
}

void Timekeeper::tick()
{
    bool rang = false;
    for (qsizetype i = 0; i < m_timers.size();) {
        Timer &t = m_timers[i];
        if (t.pausedLeft >= 0 || t.secondsLeft() > 0) {
            ++i;
            continue;
        }
        rang = true;
        if (!t.pomodoro) {
            ring(t.label.isEmpty() ? tr("Timer done") : tr("%1: time's up").arg(t.label),
                 tr("%1 timer finished").arg(TimeParse::clock(t.total)));
            m_timers.removeAt(i);
            continue;
        }
        // Pomodoro: focus → break → focus …, a long break after every fourth round.
        if (t.onBreak) {
            ++t.round;
            t.onBreak = false;
            t.label = tr("Focus");
            t.total = FocusMinutes * 60;
            ring(tr("Break over"), tr("Round %1: focus for %2 minutes").arg(t.round).arg(FocusMinutes));
        } else {
            t.onBreak = true;
            const int minutes = t.round % 4 == 0 ? LongBreakMinutes : BreakMinutes;
            t.label = tr("Break");
            t.total = minutes * 60;
            ring(tr("Time for a break"), tr("Round %1 done: rest for %2 minutes").arg(t.round).arg(minutes));
        }
        t.end = QDateTime::currentDateTime().addSecs(t.total);
        ++i;
    }
    Q_EMIT ticked();
    if (rang) {
        update();
    } else if (!m_timers.isEmpty()) {
        // The badge shows the timer that rings next.
        const Timer &next = *std::min_element(m_timers.cbegin(), m_timers.cend(),
                                              [](const Timer &a, const Timer &b) { return a.secondsLeft() < b.secondsLeft(); });
        TimerBadge::State state;
        state.label = next.label;
        state.time = TimeParse::clock(next.secondsLeft());
        state.progress = next.total > 0 ? 1.0 - double(next.secondsLeft()) / next.total : 0;
        state.paused = next.pausedLeft >= 0;
        state.pomodoro = next.pomodoro;
        state.more = int(m_timers.size()) - 1;
        m_badge->setState(state);
    }
}

// Shows the state everywhere: badge, saved file, listeners.
void Timekeeper::update()
{
    if (m_timers.isEmpty()) {
        m_tick.stop();
        m_badge->dismiss();
    } else {
        tick();
        m_badge->show();
        m_tick.start();
    }
    save();
    Q_EMIT changed();
}

void Timekeeper::scheduleReminders()
{
    const QDateTime now = QDateTime::currentDateTime();
    bool changedAny = false;
    while (!m_reminders.isEmpty() && m_reminders.first().when <= now.addSecs(1)) {
        const Reminder r = m_reminders.takeFirst();
        const bool late = r.when.secsTo(now) > 120;
        ring(tr("Reminder"), late ? tr("%1 (due %2)").arg(r.text, QLocale().toString(r.when, QLocale::ShortFormat)) : r.text);
        changedAny = true;
    }
    if (changedAny) {
        save();
        Q_EMIT changed();
    }
    if (!m_reminders.isEmpty())
        m_reminderTimer.start(int(std::clamp<qint64>(now.msecsTo(m_reminders.first().when), 1000, 3600 * 1000)));
}

void Timekeeper::ring(const QString &title, const QString &body)
{
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                                              QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    // Urgency "critical" keeps it on screen until dismissed; the sound is the
    // standard alarm from the freedesktop sound theme.
    const QVariantMap hints{{QStringLiteral("urgency"), QVariant::fromValue(uchar(2))},
                            {QStringLiteral("sound-name"), QStringLiteral("alarm-clock-elapsed")},
                            {QStringLiteral("desktop-entry"), QStringLiteral("io.github.lumen")}};
    msg.setArguments({QStringLiteral("Lumen"), 0u, QStringLiteral("chronometer"), title, body, QStringList(), hints, 0});
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void Timekeeper::load()
{
    QFile f(file());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &v : root.value(QStringLiteral("timers")).toArray()) {
        const QJsonObject o = v.toObject();
        Timer t;
        t.id = m_nextId++;
        t.label = o.value(QStringLiteral("label")).toString();
        t.end = QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("end")).toInteger());
        t.total = o.value(QStringLiteral("total")).toInt();
        t.pausedLeft = o.value(QStringLiteral("pausedLeft")).toInt(-1);
        t.pomodoro = o.value(QStringLiteral("pomodoro")).toBool();
        t.onBreak = o.value(QStringLiteral("onBreak")).toBool();
        t.round = o.value(QStringLiteral("round")).toInt(1);
        m_timers << t;
    }
    for (const QJsonValue &v : root.value(QStringLiteral("reminders")).toArray()) {
        const QJsonObject o = v.toObject();
        m_reminders << Reminder{m_nextId++, QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("when")).toInteger()),
                                o.value(QStringLiteral("text")).toString()};
    }
}

void Timekeeper::save() const
{
    QJsonArray timers, reminders;
    for (const Timer &t : m_timers)
        timers << QJsonObject{{QStringLiteral("label"), t.label}, {QStringLiteral("end"), t.end.toSecsSinceEpoch()},
                              {QStringLiteral("total"), t.total}, {QStringLiteral("pausedLeft"), t.pausedLeft},
                              {QStringLiteral("pomodoro"), t.pomodoro}, {QStringLiteral("onBreak"), t.onBreak},
                              {QStringLiteral("round"), t.round}};
    for (const Reminder &r : m_reminders)
        reminders << QJsonObject{{QStringLiteral("when"), r.when.toSecsSinceEpoch()}, {QStringLiteral("text"), r.text}};
    QDir().mkpath(QFileInfo(file()).path());
    QSaveFile f(file());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("timers"), timers}, {QStringLiteral("reminders"), reminders}}).toJson());
        f.commit();
    }
}
