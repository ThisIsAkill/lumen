#include "downloadsource.h"
#include "filesource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QMimeDatabase>
#include <QStandardPaths>

namespace
{

QString relative(const QDateTime &when)
{
    const qint64 secs = when.secsTo(QDateTime::currentDateTime());
    if (secs < 3600)
        return QObject::tr("%1 min ago").arg(qMax<qint64>(1, secs / 60));
    if (when.date() == QDate::currentDate())
        return QLocale().toString(when.time(), QLocale::ShortFormat);
    return QLocale().toString(when.date(), QLocale::ShortFormat);
}

// "Move to …" actions for the usual folders.
QList<Action> moveActions(Launcher *l, const QString &path)
{
    QList<Action> actions;
    const QList<std::pair<QStandardPaths::StandardLocation, QString>> places{
        {QStandardPaths::DocumentsLocation, QObject::tr("Move to Documents")},
        {QStandardPaths::PicturesLocation, QObject::tr("Move to Pictures")},
        {QStandardPaths::DesktopLocation, QObject::tr("Move to Desktop")},
    };
    for (const auto &[location, label] : places) {
        const QString dir = QStandardPaths::writableLocation(location);
        actions << Action{label, QStringLiteral("edit-move"), {}, [l, path, dir] {
            const QString target = QDir(dir).filePath(QFileInfo(path).fileName());
            if (QFile::exists(target)) {
                l->notify(QObject::tr("%1 already has a file with that name").arg(QFileInfo(dir).fileName()));
                return;
            }
            l->notify(QFile::rename(path, target) ? QObject::tr("Moved to %1").arg(QFileInfo(dir).fileName()) : QObject::tr("Could not move it"));
        }};
    }
    return actions;
}

} // namespace

QList<Item> DownloadSource::files(const QString &filter, int limit) const
{
    const QDir dir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    QList<Item> items;
    QMimeDatabase mimes;
    Launcher *l = launcher();
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        if (!filter.isEmpty() && fuzzyScore(filter, info.fileName()) <= 0)
            continue;
        if (info.suffix() == QLatin1String("part") || info.suffix() == QLatin1String("crdownload"))
            continue; // still downloading
        Item item;
        item.id = QStringLiteral("file:") + info.absoluteFilePath();
        item.title = info.fileName();
        item.subtitle = relative(info.lastModified()) + QStringLiteral(" · ")
            + (info.isDir() ? tr("Folder") : QLocale().formattedDataSize(info.size(), 1, QLocale::DataSizeTraditionalFormat));
        const QMimeType mime = info.isDir() ? mimes.mimeTypeForName(QStringLiteral("inode/directory")) : mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
        item.icon = QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName()));
        item.filePath = info.absoluteFilePath();
        item.score = 1000 - items.size();
        item.actions = FileSource::fileActions(l, item.filePath) + moveActions(l, item.filePath);
        items << item;
        if (items.size() == limit)
            break;
    }
    return items;
}

void DownloadSource::query(const QString &text)
{
    QList<Item> items = files(text.trimmed(), 80);
    if (items.isEmpty()) {
        Item none;
        none.title = text.trimmed().isEmpty() ? tr("Downloads is empty") : tr("Nothing called “%1” in Downloads").arg(text.trimmed());
        none.icon = QIcon::fromTheme(QStringLiteral("folder-download"));
        items << none;
    }
    Q_EMIT resultsReady(items);
}

QList<Item> DownloadSource::rootItems(const QString &text) const
{
    const QString q = text.trimmed();
    QList<Item> items;
    if (q.isEmpty())
        return items;
    Launcher *l = launcher();
    const qreal open = std::max(fuzzyScore(q, QStringLiteral("Downloads")), fuzzyScore(q, QStringLiteral("recent downloads")));
    if (open >= 60) {
        Item view;
        view.id = QStringLiteral("lumen:downloads");
        view.title = tr("Downloads");
        view.subtitle = tr("Newest first; drag files out, move or preview them");
        view.kind = tr("Command");
        view.icon = QIcon::fromTheme(QStringLiteral("folder-download"));
        view.score = open;
        view.actions << Action{tr("Open"), {}, {}, [l] { l->openView(Launcher::DownloadsView); }, true};
        items << view;
    }
    if (fuzzyScore(q, QStringLiteral("latest download")) >= 60 || fuzzyScore(q, QStringLiteral("last download")) >= 60) {
        for (Item newest : files({}, 1)) {
            newest.id = QStringLiteral("answer:download");
            newest.kind = tr("Latest download");
            newest.score = 115;
            items << newest;
        }
    }
    return items;
}
