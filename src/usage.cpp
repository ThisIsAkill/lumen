#include "usage.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

Usage::Usage()
    : m_file(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/lumen/usage.json"))
{
    QFile f(m_file);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject o = it->toObject();
        m_entries.insert(it.key(), {o.value(QStringLiteral("count")).toInt(),
                                    QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("last")).toInteger())});
    }
}

void Usage::record(const QString &id)
{
    if (id.isEmpty())
        return;
    Entry &e = m_entries[id];
    ++e.count;
    e.last = QDateTime::currentDateTime();
    save();
}

qreal Usage::frecency(const Entry &entry) const
{
    // Each use counts fully today and loses half its weight every two weeks.
    const qreal days = entry.last.secsTo(QDateTime::currentDateTime()) / 86400.0;
    return entry.count * std::pow(0.5, days / 14.0);
}

qreal Usage::boost(const QString &id) const
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd())
        return 0;
    return std::min(15.0, 5.0 * std::log2(1.0 + frecency(*it)));
}

QStringList Usage::ranked() const
{
    QStringList ids = m_entries.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        return frecency(m_entries.value(a)) > frecency(m_entries.value(b));
    });
    return ids;
}

void Usage::save() const
{
    QJsonObject root;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        root.insert(it.key(), QJsonObject{{QStringLiteral("count"), it->count},
                                          {QStringLiteral("last"), it->last.toSecsSinceEpoch()}});
    QDir().mkpath(QFileInfo(m_file).path());
    QSaveFile f(m_file);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
    }
}
