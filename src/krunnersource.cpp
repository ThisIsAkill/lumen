#include "krunnersource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KRunner/Action>
#include <KRunner/QueryMatch>
#include <KRunner/RunnerManager>
#include <KSharedConfig>

#include <QLoggingCategory>

#include <algorithm>

Q_LOGGING_CATEGORY(lcKRunner, "lumen.krunner", QtWarningMsg)

namespace
{

QString primaryText(const QString &category)
{
    if (category == QLatin1String("Applications"))
        return QObject::tr("Open Application");
    if (category == QLatin1String("Windows"))
        return QObject::tr("Switch to Window");
    if (category == QLatin1String("System Settings"))
        return QObject::tr("Open Settings");
    if (category == QLatin1String("Calculator") || category == QLatin1String("Unit Conversion"))
        return QObject::tr("Copy Result");
    return QObject::tr("Open");
}

} // namespace

KRunnerSource::KRunnerSource(Launcher *launcher)
    : Source(launcher)
{
    // Same plugin selection as KRunner, but separate launch history.
    m_manager = new KRunner::RunnerManager(KSharedConfig::openConfig(QStringLiteral("krunnerrc"))->group(QStringLiteral("Plugins")),
                                           KSharedConfig::openStateConfig(QStringLiteral("lumenstaterc"))->group(QStringLiteral("Runners")),
                                           this);
    m_manager->setHistoryEnvironmentIdentifier(QStringLiteral("lumen"));
    connect(m_manager, &KRunner::RunnerManager::matchesChanged, this, &KRunnerSource::convert);
}

KRunnerSource::~KRunnerSource() = default;

void KRunnerSource::sessionStarted()
{
    m_manager->setupMatchSession();
}

void KRunnerSource::sessionEnded()
{
    m_manager->matchSessionComplete();
}

void KRunnerSource::query(const QString &text)
{
    m_query = text.trimmed();
    const QString asked = m_query.isEmpty() ? emptyQuery() : text;
    if (asked.trimmed().isEmpty()) {
        m_manager->reset();
        Q_EMIT resultsReady({});
        return;
    }
    m_manager->launchQuery(asked, m_runnerId);
    if (!m_runnerId.isEmpty())
        convert({}); // the hint, until the plugin answers
}

void KRunnerSource::convert(const QList<KRunner::QueryMatch> &matches)
{
    QList<Item> items;
    items.reserve(matches.size());
    for (const KRunner::QueryMatch &match : matches) {
        if (!match.isEnabled())
            continue;
        Item item;
        item.id = match.id();
        item.title = match.text();
        item.subtitle = match.subtext();
        item.kind = match.matchCategory();
        item.icon = match.icon().isNull() ? QIcon::fromTheme(match.iconName()) : match.icon();
        // KRunner's own scores are on several scales (applications use
        // thousands, an exact name is infinite). Rank by how well the title
        // matches instead, with KRunner's opinion as a floor (30–60) so that
        // answers without a matching title, like a calculation, still show.
        const qreal krunnerFloor = 30 + 0.3 * std::min<qreal>(match.categoryRelevance(), 100);
        // relevance() is documented as 0–1, but some plugins report far more.
        item.score = std::max(fuzzyScore(m_query, item.title), krunnerFloor) + std::clamp<qreal>(match.relevance(), 0, 1) * 5;
        const QList<QUrl> urls = match.urls();
        if (urls.size() == 1 && urls.first().isLocalFile())
            item.filePath = urls.first().toLocalFile();

        item.actions << Action{primaryText(item.kind), {}, {}, [this, match] { m_manager->run(match); }};
        int n = 1;
        for (const KRunner::Action &a : match.actions()) {
            const QKeySequence key = n < 10 ? QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key(Qt::Key_0 + n)) : QKeySequence();
            item.actions << Action{a.text(), a.iconSource(), key, [this, match, a] { m_manager->run(match, a); }};
            ++n;
        }
        qCDebug(lcKRunner) << item.id << item.kind << item.title << item.score;
        items << item;
    }
    if (!m_runnerId.isEmpty()) {
        // A view of one plugin keeps its order, best first.
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
        if (items.isEmpty() && !emptyItem().title.isEmpty())
            items << emptyItem();
    }
    Q_EMIT resultsReady(items);
}

BrowserTabsSource::BrowserTabsSource(Launcher *launcher)
    : KRunnerSource(launcher)
{
    setRunner(QStringLiteral("browsertabs"));
}

Item BrowserTabsSource::emptyItem() const
{
    Item item;
    item.title = tr("No tabs found");
    item.subtitle = tr("Tabs are listed by the Plasma Browser Integration extension: add it to Firefox or Chrome");
    item.icon = QIcon::fromTheme(QStringLiteral("plasma-browser-integration"), QIcon::fromTheme(QStringLiteral("internet-web-browser")));
    return item;
}
