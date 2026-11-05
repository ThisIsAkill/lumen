#include "timesource.h"
#include "fuzzy.h"
#include "launcher.h"
#include "timekeeper.h"
#include "timeparse.h"

#include <QLocale>


namespace
{

QString whenText(const QDateTime &when)
{
    const QLocale locale;
    const QDate today = QDate::currentDate();
    const QString time = locale.toString(when.time(), QLocale::ShortFormat);
    if (when.date() == today)
        return time;
    if (when.date() == today.addDays(1))
        return QObject::tr("tomorrow %1").arg(time);
    return locale.toString(when, QLocale::ShortFormat);
}

} // namespace

TimeSource::TimeSource(Launcher *launcher, Timekeeper *keeper)
    : Source(launcher)
    , m_keeper(keeper)
{
    // Countdowns change every second; refresh a query that shows them.
    auto refresh = [this] {
        if (this->launcher()->isVisible()) // (the constructor's parameter shadows launcher())
            query(m_query); // also redraws the start screen's running timers
    };
    connect(keeper, &Timekeeper::changed, this, refresh);
    connect(keeper, &Timekeeper::ticked, this, refresh);
}

QList<Item> TimeSource::timerItems(const QString &filter) const
{
    QList<Item> items;
    Timekeeper *keeper = m_keeper;
    for (const Timekeeper::Timer &t : m_keeper->timers()) {
        Item item;
        item.id = QStringLiteral("timer:%1").arg(t.id);
        const bool paused = t.pausedLeft >= 0;
        const QString name = t.pomodoro ? tr("Pomodoro · %1").arg(t.label) : t.label.isEmpty() ? tr("Timer") : t.label;
        item.title = tr("%1 · %2 left").arg(name, TimeParse::clock(t.secondsLeft()));
        item.subtitle = paused ? tr("Paused") : t.pomodoro ? tr("Round %1").arg(t.round) : tr("%1 timer").arg(TimeParse::clock(t.total));
        item.kind = tr("Timer");
        item.icon = QIcon::fromTheme(t.pomodoro ? QStringLiteral("chronometer-start") : QStringLiteral("chronometer"));
        item.fraction = t.total > 0 ? 1.0 - double(t.secondsLeft()) / t.total : -1; // how far it has come
        item.score = 112;
        if (!filter.isEmpty() && fuzzyScore(filter, name) <= 0)
            continue;
        const int id = t.id;
        item.actions << Action{tr("Stop"), QStringLiteral("chronometer-reset"), {}, [keeper, id] { keeper->stopTimer(id); }, true};
        item.actions << Action{paused ? tr("Resume") : tr("Pause"), QStringLiteral("chronometer-pause"), QKeySequence(Qt::CTRL | Qt::Key_P),
                               [keeper, id, paused] { keeper->setPaused(id, !paused); }, true};
        item.actions << Action{tr("Add 5 Minutes"), QStringLiteral("list-add"), QKeySequence(Qt::CTRL | Qt::Key_Plus),
                               [keeper, id] { keeper->addTime(id, 300); }, true};
        items << item;
    }
    return items;
}

QList<Item> TimeSource::reminderItems(const QString &filter) const
{
    QList<Item> items;
    Timekeeper *keeper = m_keeper;
    for (const Timekeeper::Reminder &r : m_keeper->reminders()) {
        if (!filter.isEmpty() && fuzzyScore(filter, r.text) <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("reminder:%1").arg(r.id);
        item.title = r.text;
        item.subtitle = whenText(r.when);
        item.kind = tr("Reminder");
        item.icon = QIcon::fromTheme(QStringLiteral("appointment-reminder"));
        item.score = 111;
        const int id = r.id;
        item.actions << Action{tr("Delete Reminder"), QStringLiteral("edit-delete"), {}, [keeper, id] { keeper->removeReminder(id); }, true};
        items << item;
    }
    return items;
}

void TimeSource::query(const QString &text)
{
    m_query = text.trimmed();
    QList<Item> items;
    if (m_query.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    Launcher *l = launcher();
    Timekeeper *keeper = m_keeper;
    const QString q = m_query.toLower();

    if (const auto t = TimeParse::timer(m_query)) {
        Item item;
        item.id = QStringLiteral("answer:timer");
        item.title = t->label.isEmpty() ? tr("Start a %1 Timer").arg(TimeParse::clock(t->seconds))
                                        : tr("Start a %1 Timer: %2").arg(TimeParse::clock(t->seconds), t->label);
        item.subtitle = tr("Rings at %1").arg(whenText(QDateTime::currentDateTime().addSecs(t->seconds)));
        item.kind = tr("Timer");
        item.icon = QIcon::fromTheme(QStringLiteral("chronometer"));
        item.score = 115;
        const int seconds = t->seconds;
        const QString label = t->label;
        item.actions << Action{tr("Start"), {}, {}, [l, keeper, seconds, label] {
            keeper->startTimer(seconds, label);
            l->notify(tr("Timer started"));
        }};
        items << item;
    }
    if (const auto r = TimeParse::reminder(m_query, QDateTime::currentDateTime())) {
        Item item;
        item.id = QStringLiteral("answer:reminder");
        item.title = r->text;
        item.subtitle = tr("Remind me %1").arg(whenText(r->when));
        item.kind = tr("Reminder");
        item.icon = QIcon::fromTheme(QStringLiteral("appointment-reminder"));
        item.score = 115;
        const QDateTime when = r->when;
        const QString what = r->text;
        item.actions << Action{tr("Set Reminder"), {}, {}, [l, keeper, when, what] {
            keeper->addReminder(when, what);
            l->notify(tr("Reminder set for %1").arg(whenText(when)));
        }};
        items << item;
    }
    if (fuzzyScore(q, QStringLiteral("pomodoro")) >= 60) {
        Item item;
        item.id = QStringLiteral("lumen:pomodoro");
        item.title = tr("Start Pomodoro");
        item.subtitle = tr("25 minutes of focus, then a 5 minute break");
        item.kind = tr("Timer");
        item.icon = QIcon::fromTheme(QStringLiteral("chronometer-start"));
        item.score = 90;
        item.actions << Action{tr("Start"), {}, {}, [l, keeper] {
            keeper->startPomodoro();
            l->notify(tr("Focus for 25 minutes"));
        }};
        items << item;
    }
    // "timers", "reminders", or words from a running timer or reminder.
    const bool listTimers = q.startsWith(QLatin1String("timer")) || q.startsWith(QLatin1String("pomodoro"));
    const bool listReminders = q.startsWith(QLatin1String("reminder"));
    items << timerItems(listTimers ? QString() : m_query) << reminderItems(listReminders ? QString() : m_query);
    Q_EMIT resultsReady(items);
}

QList<Item> TimeSource::startItems() const
{
    return timerItems({});
}
