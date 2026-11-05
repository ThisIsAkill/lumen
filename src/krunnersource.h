#pragma once

#include "source.h"

namespace KRunner
{
class RunnerManager;
class QueryMatch;
}

// Results from KDE's KRunner plugins: applications, open windows,
// calculator and unit conversion, System Settings pages, power and session
// commands, bookmarks, web shortcuts, recent documents and more. It uses the
// plugins enabled for KRunner (System Settings → Search → Plasma Search).
class KRunnerSource : public Source
{
    Q_OBJECT
public:
    explicit KRunnerSource(Launcher *launcher);
    ~KRunnerSource() override;

    void query(const QString &text) override;
    void sessionStarted() override;
    void sessionEnded() override;

protected:
    // One plugin only (a view of its own), e.g. "browsertabs".
    void setRunner(const QString &runnerId) { m_runnerId = runnerId; }
    // What is asked when the field is empty ("" asks nothing).
    virtual QString emptyQuery() const { return {}; }
    // Shown when the plugin found nothing.
    virtual Item emptyItem() const { return {}; }

private:
    void convert(const QList<KRunner::QueryMatch> &matches);

    KRunner::RunnerManager *m_manager;
    QString m_query;
    QString m_runnerId;
};

// The "Browser Tabs" view: open tabs in Firefox, Chrome and others, through
// Plasma Browser Integration (its browser extension reports the tabs, and
// Enter switches to one).
class BrowserTabsSource : public KRunnerSource
{
    Q_OBJECT
public:
    explicit BrowserTabsSource(Launcher *launcher);
    QString viewTitle() const override { return tr("Tabs"); }
    QString viewPlaceholder() const override { return tr("Search open browser tabs…"); }

protected:
    QString emptyQuery() const override { return QStringLiteral("http"); } // every tab's address has it
    Item emptyItem() const override;
};
