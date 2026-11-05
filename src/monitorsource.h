#pragma once

#include "source.h"

#include <QTimer>

// "system", "cpu", "memory", "disk", "battery", "temperature", "uptime":
// cards with the machine's state, read from /proc and /sys (CPU load is
// sampled over half a second while the launcher is open). "Processes" opens
// ProcessSource.
class MonitorSource : public Source
{
    Q_OBJECT
public:
    explicit MonitorSource(Launcher *launcher);
    void query(const QString &text) override;
    void sessionStarted() override;
    void sessionEnded() override;

    struct CpuTimes {
        quint64 busy = 0, total = 0;
    };
    static CpuTimes readCpu();
    // Memory in KiB: total, available, swap total, swap free.
    static std::array<quint64, 4> readMemory();
    static std::optional<double> cpuTemperature(); // °C
    static std::optional<std::pair<int, QString>> battery(); // percent, status

private:
    void sample();

    QTimer m_sampler;
    CpuTimes m_last;
    double m_cpu = -1; // percent, -1 until measured
    QString m_query;
};

// The "Processes" view: the busiest processes (by CPU, or memory when the
// query starts with "mem"), with Quit (SIGTERM) and Force Quit (SIGKILL).
class ProcessSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Processes"); }
    QString viewPlaceholder() const override { return tr("Filter processes…"); }
};
