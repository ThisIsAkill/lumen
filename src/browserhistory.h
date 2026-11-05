#pragma once

#include "source.h"

#include <QDateTime>
#include <QFutureWatcher>

// Pages visited in Firefox (and its relatives) and in Chromium-based
// browsers (Chrome, Brave, Vivaldi, Edge, Opera), read from each browser's
// own history database: works without any extension. The databases are
// copied first (browsers keep them locked) and read in the background, at
// most every few minutes; searches run over the loaded list.
//
// As a root source it offers good matches among the other results; as the
// "Browser History" view it lists recent visits.
class BrowserHistory : public Source
{
    Q_OBJECT
public:
    struct Visit {
        QString url;
        QString title;
        QString browser; // "Firefox", "Brave", …
        QString icon;    // the browser's icon name
        QDateTime last;
        int visits = 0;
    };

    BrowserHistory(Launcher *launcher, bool view);

    void query(const QString &text) override;
    void sessionStarted() override;
    QString viewTitle() const override { return tr("History"); }
    QString viewPlaceholder() const override { return tr("Search browser history…"); }

    // Readers (tested with small databases).
    static QList<Visit> readFirefox(const QString &placesFile, const QString &browser);
    static QList<Visit> readChromium(const QString &historyFile, const QString &browser);
    // Every history database found in the home folder: browser name, icon, file.
    struct Database {
        QString browser;
        QString icon;
        QString file;
        bool firefox = false;
    };
    static QList<Database> databases();
    // All of them, newest visit per address first.
    static QList<Visit> loadAll();

    // The list shared by the root source and the view.
    static QList<Visit> &visits();

private:
    void refresh();
    void publish();

    bool m_view;
    QString m_query;
    QFutureWatcher<QList<Visit>> m_loading;
};
