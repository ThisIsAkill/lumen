#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>

// Remembers how often and how recently each result was used, so that
// favourites rise to the top ("frecency"). Stored in
// ~/.local/share/lumen/usage.json.
class Usage
{
public:
    Usage();

    void record(const QString &id);

    // 0 for never used, up to 15 for something used often and recently.
    qreal boost(const QString &id) const;

    // Most used ids first.
    QStringList ranked() const;

private:
    struct Entry {
        int count = 0;
        QDateTime last;
    };
    qreal frecency(const Entry &entry) const;
    void save() const;

    QString m_file;
    QHash<QString, Entry> m_entries;
};
