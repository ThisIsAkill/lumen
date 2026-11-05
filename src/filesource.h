#pragma once

#include "source.h"

#include <QProcess>
#include <QTimer>

// The "Search Files" view: finds files and folders by name.
//  - In the home folder, live with fd (hidden folders, node_modules and the
//    like are skipped unless the query starts with a dot).
//  - Everywhere, when the query starts with "/", with plocate (the system's
//    file name database, refreshed daily; it does not cover /home when that
//    is a separate btrfs subvolume, the usual layout).
// Without fd, plocate is used for both.
class FileSource : public Source
{
    Q_OBJECT
public:
    explicit FileSource(Launcher *launcher);

    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Files"); }
    QString viewPlaceholder() const override { return tr("Search files by name…"); }
    bool hasPreviewPane() const override { return true; }

    // Actions shared with other sources that show local files.
    static QList<Action> fileActions(Launcher *launcher, const QString &path);

private:
    QList<Item> recentFiles() const;
    void search();
    void finished();

    QString m_query;
    QTimer m_delay;
    QProcess *m_proc = nullptr;
};
