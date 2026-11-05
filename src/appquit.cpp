#include "appquit.h"
#include "fuzzy.h"
#include "launcher.h"
#include "plural.h"
#include "windowtiling.h"

#include <KService>

#include <QDBusConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <csignal>

namespace
{

// Shown by window class, but never offered: the desktop itself and Lumen.
const QStringList &neverQuit()
{
    static const QStringList list{QStringLiteral("lumen"), QStringLiteral("io.github.lumen"), QStringLiteral("plasmashell"),
                                  QStringLiteral("org.kde.plasmashell"), QStringLiteral("kwin"), QStringLiteral("org.kde.kwin")};
    return list;
}

// The apps' root and the view share what KWin last reported.
QList<AppQuitSource *> &instances()
{
    static QList<AppQuitSource *> list;
    return list;
}

} // namespace

AppQuitSource::AppQuitSource(Launcher *launcher, bool view)
    : Source(launcher)
    , m_view(view)
{
    // One D-Bus object answers for both (the view registers it).
    if (view)
        QDBusConnection::sessionBus().registerObject(QStringLiteral("/Apps"), this, QDBusConnection::ExportScriptableSlots);
    instances() << this;
    connect(this, &QObject::destroyed, this, [this] { instances().removeAll(this); });
}

QList<AppQuitSource::App> AppQuitSource::appsFrom(const QString &windowsJson)
{
    QList<App> apps;
    for (const QJsonValue &v : QJsonDocument::fromJson(windowsJson.toUtf8()).array()) {
        const QJsonObject w = v.toObject();
        const QString desktop = w.value(QStringLiteral("app")).toString();
        const QString cls = w.value(QStringLiteral("cls")).toString();
        const QString id = desktop.isEmpty() ? cls : desktop;
        if (id.isEmpty() || neverQuit().contains(id.toLower()) || neverQuit().contains(cls.toLower()))
            continue;
        auto it = std::find_if(apps.begin(), apps.end(), [&](const App &a) { return a.id == id; });
        if (it == apps.end()) {
            App app;
            app.id = id;
            const KService::Ptr service = KService::serviceByDesktopName(desktop);
            app.name = service ? service->name() : cls.section(u'.', -1);
            if (!app.name.isEmpty())
                app.name[0] = app.name.at(0).toUpper();
            app.icon = service ? service->icon() : cls;
            apps << app;
            it = apps.end() - 1;
        }
        const qint64 pid = w.value(QStringLiteral("pid")).toInteger();
        if (pid > 0 && !it->pids.contains(pid))
            it->pids << pid;
        it->captions << w.value(QStringLiteral("caption")).toString();
    }
    std::sort(apps.begin(), apps.end(), [](const App &a, const App &b) { return a.name.localeAwareCompare(b.name) < 0; });
    return apps;
}

void AppQuitSource::sessionStarted()
{
    if (m_view)
        request(); // one request for both
}

void AppQuitSource::request()
{
    WindowTiling::runScript(QStringLiteral("app-list"), QStringLiteral(R"js(
const out = [];
for (const w of workspace.windowList()) {
    if (!w.normalWindow || w.skipTaskbar)
        continue;
    out.push({app: w.desktopFileName, cls: w.resourceClass, caption: w.caption, pid: w.pid});
}
callDBus("io.github.lumen", "/Apps", "io.github.lumen.Apps", "report", JSON.stringify(out));
)js"));
}

void AppQuitSource::report(const QString &windowsJson)
{
    const QList<App> apps = appsFrom(windowsJson);
    for (AppQuitSource *s : std::as_const(instances())) {
        s->m_apps = apps;
        if (s->launcher()->currentSource() == s || (!s->m_view && !s->m_query.isEmpty()))
            s->publish();
    }
}

void AppQuitSource::quit(const App &app)
{
    // Ask each window to close, as its close button would: the app may ask
    // to save first.
    WindowTiling::runScript(QStringLiteral("app-quit"), QStringLiteral(R"js(
const id = %1;
for (const w of workspace.windowList())
    if (w.normalWindow && (w.desktopFileName === id || w.resourceClass === id))
        w.closeWindow();
)js")
                                                            .arg(WindowTiling::jsString(app.id)));
    launcher()->notify(tr("Quitting %1").arg(app.name));
    QTimer::singleShot(900, this, &AppQuitSource::request);
}

void AppQuitSource::forceQuit(const App &app)
{
    int stopped = 0;
    for (qint64 pid : app.pids)
        if (::kill(pid_t(pid), SIGKILL) == 0)
            ++stopped;
    launcher()->notify(stopped ? tr("%1 stopped").arg(app.name) : tr("Could not stop %1").arg(app.name));
    QTimer::singleShot(400, this, &AppQuitSource::request);
}

Item AppQuitSource::itemFor(const App &app, qreal score) const
{
    Item item;
    item.id = QStringLiteral("running-app:") + app.id;
    item.title = app.name;
    item.subtitle = app.captions.size() == 1 ? app.captions.first() : plural(app.captions.size(), tr("%1 window"), tr("%1 windows"));
    item.kind = tr("Running");
    item.icon = QIcon::fromTheme(app.icon, QIcon::fromTheme(QStringLiteral("application-x-executable")));
    item.score = score;
    auto self = const_cast<AppQuitSource *>(this);
    item.actions << Action{tr("Quit %1").arg(app.name), {}, {}, [self, app] { self->quit(app); }, true};
    item.actions << Action{tr("Force Quit %1").arg(app.name), QStringLiteral("process-stop"), QKeySequence(Qt::CTRL | Qt::Key_Delete),
                           [self, app] { self->forceQuit(app); }, true};
    return item;
}

void AppQuitSource::query(const QString &text)
{
    m_query = text.trimmed();
    if (m_view && m_apps.isEmpty())
        request();
    publish();
}

void AppQuitSource::publish()
{
    QList<Item> items;
    auto self = this;
    if (m_view) {
        for (int i = 0; i < m_apps.size(); ++i) {
            const App &app = m_apps.at(i);
            const qreal score = m_query.isEmpty() ? 1000 - i : fuzzyScore(m_query, app.name);
            if (score > 0)
                items << itemFor(app, score);
        }
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
        Q_EMIT resultsReady(items);
        return;
    }
    const QString q = m_query.toLower();
    // "quit firefox", "force quit firefox", "close kate", "kill steam".
    bool force = false;
    QString name;
    for (const char *word : {"force quit ", "force kill ", "kill ", "quit ", "close "}) {
        if (q.startsWith(QLatin1String(word))) {
            force = qstrncmp(word, "force", 5) == 0 || qstrcmp(word, "kill ") == 0;
            name = q.mid(int(qstrlen(word))).trimmed();
            break;
        }
    }
    if (!name.isEmpty()) {
        for (const App &app : std::as_const(m_apps)) {
            const qreal score = fuzzyScore(name, app.name);
            if (score <= 0)
                continue;
            Item item = itemFor(app, 100 + score / 10);
            item.title = force ? tr("Force Quit %1").arg(app.name) : tr("Quit %1").arg(app.name);
            if (force)
                std::swap(item.actions[0], item.actions[1]);
            items << item;
        }
    }
    if (fuzzyScore(m_query, QStringLiteral("Quit All Apps")) >= 60 && !m_apps.isEmpty()) {
        Item all;
        all.id = QStringLiteral("lumen:quit-all");
        all.title = tr("Quit All Apps");
        all.subtitle = plural(m_apps.size(), tr("Close the windows of the %1 running app"), tr("Close the windows of the %1 running apps"));
        all.kind = tr("Command");
        all.icon = QIcon::fromTheme(QStringLiteral("application-exit"));
        all.score = fuzzyScore(m_query, QStringLiteral("Quit All Apps"));
        const QList<App> apps = m_apps;
        all.actions << Action{tr("Quit All"), {}, {}, [self, apps] {
            for (const App &app : apps)
                self->quit(app);
        }, true};
        items << all;
    }
    Q_EMIT resultsReady(items);
}
