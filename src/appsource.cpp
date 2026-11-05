#include "appsource.h"
#include "fuzzy.h"

#include <KApplicationTrader>
#include <KIO/ApplicationLauncherJob>

void AppSource::sessionStarted()
{
    if (m_apps.isEmpty())
        m_apps = KApplicationTrader::query([](const KService::Ptr &service) { return !service->noDisplay(); });
}

void AppSource::query(const QString &text)
{
    QList<Item> items;
    const QString q = text.trimmed();
    if (q.size() >= 4) {
        for (const KService::Ptr &service : std::as_const(m_apps)) {
            const qreal score = fuzzyScore(q, service->name());
            if (score <= 0 || score >= 20)
                continue; // no match, or a proper match that KRunner reports
            Item item;
            item.id = QStringLiteral("app:") + service->storageId();
            item.title = service->name();
            item.subtitle = service->genericName().isEmpty() ? service->comment() : service->genericName();
            item.kind = tr("Applications");
            item.icon = QIcon::fromTheme(service->icon(), QIcon::fromTheme(QStringLiteral("application-x-executable")));
            item.score = score + 20; // a misspelt app still beats loose matches elsewhere
            const KService::Ptr app = service;
            item.actions << Action{tr("Open Application"), {}, {}, [app] { (new KIO::ApplicationLauncherJob(app))->start(); }};
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}
