#include "monitorsource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QProcess>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <csignal>

namespace
{

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

QString size(quint64 kib)
{
    return QLocale().formattedDataSize(qint64(kib) * 1024, 1, QLocale::DataSizeTraditionalFormat);
}

QString bytes(qint64 b)
{
    return QLocale().formattedDataSize(b, 1, QLocale::DataSizeTraditionalFormat);
}

// A system card; `fraction` (0–1) of the resource in use shows as a bar.
Item card(const QString &id, const QString &title, const QString &subtitle, const QString &icon, qreal score, qreal fraction = -1)
{
    Item item;
    item.id = QStringLiteral("answer:") + id;
    item.title = title;
    item.subtitle = subtitle;
    item.kind = QObject::tr("System");
    item.icon = QIcon::fromTheme(icon);
    item.score = score;
    item.fraction = fraction;
    return item;
}

} // namespace

MonitorSource::MonitorSource(Launcher *launcher)
    : Source(launcher)
{
    m_sampler.setInterval(500);
    connect(&m_sampler, &QTimer::timeout, this, &MonitorSource::sample);
}

MonitorSource::CpuTimes MonitorSource::readCpu()
{
    // The first line of /proc/stat: cpu user nice system idle iowait irq softirq steal …
    const QStringList f = readText(QStringLiteral("/proc/stat")).section(u'\n', 0, 0).split(u' ', Qt::SkipEmptyParts);
    CpuTimes t;
    for (int i = 1; i < f.size() && i <= 8; ++i) {
        const quint64 v = f.at(i).toULongLong();
        t.total += v;
        if (i != 4 && i != 5) // idle, iowait
            t.busy += v;
    }
    return t;
}

std::array<quint64, 4> MonitorSource::readMemory()
{
    QHash<QString, quint64> values;
    for (const QString &line : readText(QStringLiteral("/proc/meminfo")).split(u'\n'))
        values.insert(line.section(u':', 0, 0), line.section(u':', 1).trimmed().section(u' ', 0, 0).toULongLong());
    return {values.value(QStringLiteral("MemTotal")), values.value(QStringLiteral("MemAvailable")), values.value(QStringLiteral("SwapTotal")),
            values.value(QStringLiteral("SwapFree"))};
}

std::optional<double> MonitorSource::cpuTemperature()
{
    // A CPU sensor among the hwmon devices (AMD k10temp/zenpower, Intel coretemp).
    for (const QFileInfo &hwmon : QDir(QStringLiteral("/sys/class/hwmon")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString name = readText(hwmon.absoluteFilePath() + QStringLiteral("/name"));
        if (name == QLatin1String("k10temp") || name == QLatin1String("zenpower") || name == QLatin1String("coretemp")) {
            const QString value = readText(hwmon.absoluteFilePath() + QStringLiteral("/temp1_input"));
            if (!value.isEmpty())
                return value.toDouble() / 1000;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<int, QString>> MonitorSource::battery()
{
    for (const QFileInfo &supply : QDir(QStringLiteral("/sys/class/power_supply")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (readText(supply.absoluteFilePath() + QStringLiteral("/type")) != QLatin1String("Battery"))
            continue;
        return std::pair{readText(supply.absoluteFilePath() + QStringLiteral("/capacity")).toInt(),
                         readText(supply.absoluteFilePath() + QStringLiteral("/status"))};
    }
    return std::nullopt;
}

void MonitorSource::sessionStarted()
{
    m_last = readCpu();
    m_cpu = -1;
}

void MonitorSource::sessionEnded()
{
    m_sampler.stop();
}

void MonitorSource::sample()
{
    const CpuTimes now = readCpu();
    if (now.total > m_last.total)
        m_cpu = 100.0 * double(now.busy - m_last.busy) / double(now.total - m_last.total);
    m_last = now;
    query(m_query);
}

void MonitorSource::query(const QString &text)
{
    m_query = text.trimmed();
    QList<Item> items;
    const QString q = m_query.toLower();
    auto asks = [&](std::initializer_list<const char *> words) {
        for (const char *w : words)
            if (fuzzyScore(q, QLatin1String(w)) >= 80)
                return true;
        return false;
    };
    const bool all = asks({"system", "system monitor", "stats", "resources"});
    Launcher *l = launcher();

    if (all || asks({"cpu", "processor", "load"})) {
        const int cores = QThread::idealThreadCount();
        items << card(QStringLiteral("cpu"), m_cpu < 0 ? tr("CPU · measuring…") : tr("CPU %1%").arg(qRound(m_cpu)),
                      tr("%1 cores").arg(cores), QStringLiteral("cpu"), 113, m_cpu < 0 ? 0 : m_cpu / 100);
        m_sampler.start(); // live while shown
    } else {
        m_sampler.stop();
    }
    if (all || asks({"memory", "ram", "swap"})) {
        const auto [total, available, swapTotal, swapFree] = readMemory();
        items << card(QStringLiteral("memory"), tr("Memory %1 of %2").arg(size(total - available), size(total)),
                      swapTotal ? tr("Swap %1 of %2 used").arg(size(swapTotal - swapFree), size(swapTotal)) : tr("No swap"),
                      QStringLiteral("memory"), 112, total ? double(total - available) / double(total) : -1);
    }
    if (all || asks({"disk", "storage", "space", "free space"})) {
        QStringList seen;
        for (const QString &path : {QStringLiteral("/"), QDir::homePath()}) {
            const QStorageInfo s(path);
            if (!s.isValid() || seen.contains(s.device()))
                continue; // /home on the same filesystem as /
            seen << s.device();
            items << card(QStringLiteral("disk-") + path, tr("%1 free on %2").arg(bytes(s.bytesAvailable()), path == QLatin1String("/") ? tr("system") : tr("home")),
                          tr("%1 of %2 used · %3").arg(bytes(s.bytesTotal() - s.bytesAvailable()), bytes(s.bytesTotal()), QString::fromUtf8(s.fileSystemType())),
                          QStringLiteral("drive-harddisk"), 111,
                          s.bytesTotal() > 0 ? double(s.bytesTotal() - s.bytesAvailable()) / double(s.bytesTotal()) : -1);
        }
    }
    if (all || asks({"temperature", "temp", "heat", "sensors"})) {
        if (const auto t = cpuTemperature())
            items << card(QStringLiteral("temperature"), tr("CPU %1 °C").arg(qRound(*t)), tr("Processor temperature"), QStringLiteral("temperature-normal"), 110);
    }
    if (all || asks({"battery", "power"})) {
        if (const auto b = battery())
            items << card(QStringLiteral("battery"), tr("Battery %1%").arg(b->first), b->second, QStringLiteral("battery"), 110, b->first / 100.0);
        else if (!all)
            items << card(QStringLiteral("battery"), tr("No battery"), tr("This computer runs on mains power"), QStringLiteral("battery-missing"), 110);
    }
    if (all || asks({"uptime"})) {
        const qint64 seconds = readText(QStringLiteral("/proc/uptime")).section(u'.', 0, 0).toLongLong();
        items << card(QStringLiteral("uptime"), tr("Up %1 d %2 h %3 min").arg(seconds / 86400).arg(seconds / 3600 % 24).arg(seconds / 60 % 60),
                      tr("Since %1").arg(QLocale().toString(QDateTime::currentDateTime().addSecs(-seconds), QLocale::ShortFormat)),
                      QStringLiteral("clock"), 109);
    }
    const qreal processes = std::max({fuzzyScore(q, QStringLiteral("processes")), fuzzyScore(q, QStringLiteral("task manager")),
                                      fuzzyScore(q, QStringLiteral("top")), fuzzyScore(q, QStringLiteral("kill"))});
    if (processes >= 60 || all) {
        Item open = card(QStringLiteral("processes"), tr("Processes"), tr("The busiest programs; quit or force quit them"),
                         QStringLiteral("view-process-system"), all ? 108 : processes);
        open.id = QStringLiteral("lumen:processes");
        open.kind = tr("Command");
        open.actions << Action{tr("Open"), {}, {}, [l] { l->openView(Launcher::ProcessesView); }, true};
        items << open;
    }
    Q_EMIT resultsReady(items);
}

// ------------------------------------------------------------ processes

void ProcessSource::query(const QString &text)
{
    const QString q = text.trimmed();
    const bool byMemory = q.startsWith(QLatin1String("mem"), Qt::CaseInsensitive);
    const QString filter = byMemory ? q.section(u' ', 1) : q;
    QProcess ps;
    ps.start(QStringLiteral("ps"), {QStringLiteral("-eo"), QStringLiteral("pid=,pcpu=,rss=,user=,comm="),
                                    byMemory ? QStringLiteral("--sort=-rss") : QStringLiteral("--sort=-pcpu")});
    ps.waitForFinished(2000);
    QList<Item> items;
    Launcher *l = launcher();
    const QString me = qEnvironmentVariable("USER");
    int i = 0;
    for (const QString &line : QString::fromUtf8(ps.readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts)) {
        const QStringList f = line.split(u' ', Qt::SkipEmptyParts);
        if (f.size() < 5)
            continue;
        const qint64 pid = f.at(0).toLongLong();
        const QString name = f.mid(4).join(u' ');
        if (!filter.isEmpty() && fuzzyScore(filter, name) <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("process:%1").arg(pid);
        item.title = name;
        item.subtitle = tr("CPU %1% · memory %2 · %3 · PID %4").arg(f.at(1), size(f.at(2).toULongLong()), f.at(3)).arg(pid);
        item.kind = f.at(3) == me ? QString() : f.at(3);
        item.icon = QIcon::fromTheme(name.toLower(), QIcon::fromTheme(QStringLiteral("application-x-executable")));
        item.score = 1000 - i++;
        const bool mine = f.at(3) == me;
        if (mine) {
            auto refresh = [this, text] { QTimer::singleShot(400, this, [this, text] { query(text); }); };
            item.actions << Action{tr("Quit"), {}, {}, [l, pid, name, refresh] {
                l->notify(::kill(pid_t(pid), SIGTERM) == 0 ? tr("Asked %1 to quit").arg(name) : tr("Could not quit %1").arg(name));
                refresh();
            }, true};
            item.actions << Action{tr("Force Quit"), QStringLiteral("process-stop"), QKeySequence(Qt::CTRL | Qt::Key_Delete), [l, pid, name, refresh] {
                l->notify(::kill(pid_t(pid), SIGKILL) == 0 ? tr("%1 stopped").arg(name) : tr("Could not stop %1").arg(name));
                refresh();
            }, true};
        }
        item.actions << Action{tr("Copy PID"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                               [l, pid] { l->copyText(QString::number(pid)); }};
        items << item;
        if (items.size() == 60)
            break;
    }
    Q_EMIT resultsReady(items);
}
