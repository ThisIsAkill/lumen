#pragma once

#include "source.h"

// Downloads: "downloads" opens a view of the Downloads folder, newest first,
// with a preview; "latest download" (or "last download") answers with the
// newest file. Files can be dragged out of the list into other apps, and
// moved to Documents, Pictures or the Desktop.
class DownloadSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Downloads"); }
    QString viewPlaceholder() const override { return tr("Filter downloads…"); }
    bool hasPreviewPane() const override { return true; }

    // Root results ("downloads", "latest download"), from DownloadRootSource.
    QList<Item> rootItems(const QString &text) const;

private:
    QList<Item> files(const QString &filter, int limit) const;
};

// Puts DownloadSource's root results in the main search.
class DownloadRootSource : public Source
{
    Q_OBJECT
public:
    DownloadRootSource(Launcher *launcher, DownloadSource *view)
        : Source(launcher)
        , m_view(view)
    {
    }
    void query(const QString &text) override { Q_EMIT resultsReady(m_view->rootItems(text)); }

private:
    DownloadSource *m_view;
};
