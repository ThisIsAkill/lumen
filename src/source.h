#pragma once

#include "item.h"

#include <QObject>

class Launcher;

// A provider of results. Root sources answer the main search; a source can
// also be opened as a "view" of its own (clipboard history, file search),
// which then answers every query until the user leaves it.
class Source : public QObject
{
    Q_OBJECT
public:
    explicit Source(Launcher *launcher)
        : QObject(nullptr)
        , m_launcher(launcher)
    {
    }

    // Starts a search. Results arrive through resultsReady(), possibly more
    // than once (sources that run asynchronously refine them).
    virtual void query(const QString &text) = 0;

    // Shown in the search field while this source is open as a view.
    virtual QString viewTitle() const { return {}; }
    virtual QString viewPlaceholder() const { return {}; }
    // Whether this view shows a preview of the selection beside the list.
    virtual bool hasPreviewPane() const { return false; }

    // This source's commands that can take any search text, as results
    // that run with `text` (see Library::fallbacks()). Their ids are the
    // commands' own.
    virtual QList<Item> fallbackItems(const QString &text) const
    {
        Q_UNUSED(text);
        return {};
    }

    // The launcher opened (setup) or closed (teardown).
    virtual void sessionStarted() { }
    virtual void sessionEnded() { }

Q_SIGNALS:
    void resultsReady(const QList<Item> &items);

protected:
    Launcher *launcher() const { return m_launcher; }

private:
    Launcher *m_launcher;
};
