#include "menusearch.h"
#include "fuzzy.h"
#include "launcher.h"

#include "qwayland-plasma-window-management.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QHash>
#include <QWaylandClientExtensionTemplate>

namespace
{

constexpr int MaxSubmenuLoads = 60;   // apps with huge menus: load this many submenus
constexpr int MenuCallTimeout = 1500; // ms
const QString MenuInterface = QStringLiteral("com.canonical.dbusmenu");
const QStringList MenuProperties{QStringLiteral("label"), QStringLiteral("enabled"), QStringLiteral("visible"), QStringLiteral("type"),
                                 QStringLiteral("children-display"), QStringLiteral("shortcut"), QStringLiteral("toggle-type"),
                                 QStringLiteral("toggle-state")};

QDBusMessage menuCall(const QString &service, const QString &path, const QString &method, const QVariantList &args)
{
    auto msg = QDBusMessage::createMethodCall(service, path, MenuInterface, method);
    msg.setArguments(args);
    return msg;
}

// [["Control", "Shift", "s"]] → "Ctrl+Shift+S"
QString shortcutText(const QVariant &value)
{
    if (!value.canConvert<QDBusArgument>())
        return {};
    const QDBusArgument arg = value.value<QDBusArgument>();
    QStringList combos;
    arg.beginArray();
    while (!arg.atEnd()) {
        QStringList keys;
        arg >> keys;
        static const QHash<QString, QString> names{{QStringLiteral("Control"), QStringLiteral("Ctrl")}, {QStringLiteral("Super"), QStringLiteral("Meta")}};
        for (QString &k : keys)
            k = names.value(k, k.size() == 1 ? k.toUpper() : k);
        combos << keys.join(u'+');
    }
    arg.endArray();
    return combos.join(QStringLiteral(", "));
}

} // namespace

QDBusArgument &operator<<(QDBusArgument &argument, const DBusMenuLayout &item)
{
    argument.beginStructure();
    argument << item.id << item.properties;
    argument.beginArray(qMetaTypeId<QDBusVariant>());
    for (const DBusMenuLayout &child : item.children)
        argument << QDBusVariant(QVariant::fromValue(child));
    argument.endArray();
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, DBusMenuLayout &item)
{
    argument.beginStructure();
    argument >> item.id >> item.properties;
    argument.beginArray();
    item.children.clear();
    while (!argument.atEnd()) {
        QDBusVariant child;
        argument >> child;
        DBusMenuLayout layout;
        child.variant().value<QDBusArgument>() >> layout;
        item.children << layout;
    }
    argument.endArray();
    argument.endStructure();
    return argument;
}

// ------------------------------------------------------------- windows

// A window as Plasma's window-management protocol reports it: whether it is
// active, its title and where its menu is.
class PlasmaWindow : public QtWayland::org_kde_plasma_window
{
public:
    PlasmaWindow(struct ::org_kde_plasma_window *window, WindowTracker *tracker)
        : QtWayland::org_kde_plasma_window(window)
        , m_tracker(tracker)
    {
    }
    ~PlasmaWindow() override { destroy(); }

    QString title, appId, menuService, menuPath;
    bool active = false;

protected:
    void org_kde_plasma_window_title_changed(const QString &t) override { title = t; }
    void org_kde_plasma_window_app_id_changed(const QString &id) override { appId = id; }
    void org_kde_plasma_window_application_menu(const QString &service, const QString &path) override
    {
        menuService = service;
        menuPath = path;
    }
    void org_kde_plasma_window_state_changed(uint32_t flags) override;
    void org_kde_plasma_window_unmapped() override;

private:
    WindowTracker *m_tracker;
};

// Follows the windows to know the last active one that is not Lumen's.
class WindowTracker : public QWaylandClientExtensionTemplate<WindowTracker>, public QtWayland::org_kde_plasma_window_management
{
public:
    WindowTracker()
        : QWaylandClientExtensionTemplate<WindowTracker>(16) // window_with_uuid since 13
    {
        initialize();
    }
    ~WindowTracker() override
    {
        m_windows.clear();
        if (isInitialized())
            wl_proxy_destroy(reinterpret_cast<wl_proxy *>(object()));
    }

    const PlasmaWindow *lastActive() const { return m_lastActive; }
    void activated(PlasmaWindow *w)
    {
        if (w->appId != QLatin1String("io.github.lumen") && w->appId != QLatin1String("lumen"))
            m_lastActive = w;
    }
    void gone(PlasmaWindow *w)
    {
        if (m_lastActive == w)
            m_lastActive = nullptr;
        m_windows.erase(std::remove_if(m_windows.begin(), m_windows.end(), [w](const auto &p) { return p.get() == w; }), m_windows.end());
    }

protected:
    void org_kde_plasma_window_management_window_with_uuid(uint32_t, const QString &uuid) override
    {
        m_windows.push_back(std::make_unique<PlasmaWindow>(get_window_by_uuid(uuid), this));
    }

private:
    std::vector<std::unique_ptr<PlasmaWindow>> m_windows;
    PlasmaWindow *m_lastActive = nullptr;
};

void PlasmaWindow::org_kde_plasma_window_state_changed(uint32_t flags)
{
    active = flags & QtWayland::org_kde_plasma_window_management::state_active;
    if (active)
        m_tracker->activated(this);
}

void PlasmaWindow::org_kde_plasma_window_unmapped()
{
    // Deleted once this event has been handled.
    QMetaObject::invokeMethod(qApp, [tracker = m_tracker, self = this] { tracker->gone(self); }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------- menus

MenuSource::MenuSource(Launcher *launcher)
    : Source(launcher)
{
    qDBusRegisterMetaType<DBusMenuLayout>();
    if (QGuiApplication::platformName() == QLatin1String("wayland"))
        m_tracker = std::make_unique<WindowTracker>();
}

MenuSource::~MenuSource() = default;

QString MenuSource::viewTitle() const
{
    return m_appName.isEmpty() ? tr("Menu") : m_appName;
}

void MenuSource::sessionStarted()
{
    // The app that was in front: its menu is the one to search.
    const PlasmaWindow *w = m_tracker && m_tracker->isActive() ? m_tracker->lastActive() : nullptr;
    if (w && (w->menuService != m_service || w->menuPath != m_path || !m_loaded))
        load(w->menuService, w->menuPath, w->title.section(QStringLiteral(" — "), -1));
}

QString MenuSource::plainLabel(const QString &label)
{
    QString out;
    for (qsizetype i = 0; i < label.size(); ++i) {
        const QChar c = label.at(i);
        if ((c == u'_' || c == u'&') && i + 1 < label.size()) {
            if (label.at(i + 1) == c) // "__" is a literal underscore
                out += label.at(++i);
            continue;
        }
        out += c;
    }
    return out.trimmed();
}

QList<MenuSource::Entry> MenuSource::flatten(const DBusMenuLayout &root)
{
    QList<Entry> entries;
    std::function<void(const DBusMenuLayout &, const QString &)> walk = [&](const DBusMenuLayout &menu, const QString &path) {
        for (const DBusMenuLayout &item : menu.children) {
            const QVariantMap &p = item.properties;
            if (p.value(QStringLiteral("type")).toString() == QLatin1String("separator") || !p.value(QStringLiteral("visible"), true).toBool()
                || !p.value(QStringLiteral("enabled"), true).toBool())
                continue;
            const QString label = plainLabel(p.value(QStringLiteral("label")).toString());
            if (label.isEmpty())
                continue;
            if (p.value(QStringLiteral("children-display")).toString() == QLatin1String("submenu") || !item.children.isEmpty()) {
                walk(item, path.isEmpty() ? label : path + QStringLiteral(" › ") + label);
                continue;
            }
            Entry e;
            e.id = item.id;
            e.label = label;
            e.path = path;
            e.shortcut = shortcutText(p.value(QStringLiteral("shortcut")));
            e.checked = !p.value(QStringLiteral("toggle-type")).toString().isEmpty() && p.value(QStringLiteral("toggle-state")).toInt() == 1;
            entries << e;
        }
    };
    walk(root, {});
    return entries;
}

void MenuSource::load(const QString &service, const QString &path, const QString &appName)
{
    m_service = service;
    m_path = path;
    m_appName = appName;
    m_root = {};
    m_loaded = false;
    m_pending = 0;
    if (service.isEmpty() || path.isEmpty())
        return;
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(menuCall(service, path, QStringLiteral("GetLayout"), {0, -1, MenuProperties}), MenuCallTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, service, path] {
        watcher->deleteLater();
        const QDBusPendingReply<uint, DBusMenuLayout> reply = *watcher;
        if (service != m_service || path != m_path)
            return; // another app meanwhile
        if (reply.isError()) {
            finishLoading();
            return;
        }
        m_root = reply.argumentAt<1>();
        loadSubmenus(m_root);
        if (m_pending == 0)
            finishLoading();
    });
}

// Apps fill most submenus only when they are about to open: ask for each
// empty one, then read it.
void MenuSource::loadSubmenus(const DBusMenuLayout &root)
{
    std::function<void(const DBusMenuLayout &)> visit = [&](const DBusMenuLayout &menu) {
        for (const DBusMenuLayout &item : menu.children) {
            if (item.properties.value(QStringLiteral("children-display")).toString() != QLatin1String("submenu"))
                continue;
            if (!item.children.isEmpty()) {
                visit(item);
                continue;
            }
            if (m_pending >= MaxSubmenuLoads)
                return;
            ++m_pending;
            const int id = item.id;
            const QString service = m_service, path = m_path;
            auto bus = QDBusConnection::sessionBus();
            bus.asyncCall(menuCall(service, path, QStringLiteral("AboutToShow"), {id}), MenuCallTimeout);
            auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(menuCall(service, path, QStringLiteral("GetLayout"), {id, -1, MenuProperties}),
                                                                      MenuCallTimeout),
                                                        this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, id, service, path] {
                watcher->deleteLater();
                if (service != m_service || path != m_path)
                    return;
                const QDBusPendingReply<uint, DBusMenuLayout> reply = *watcher;
                if (!reply.isError()) {
                    const DBusMenuLayout sub = reply.argumentAt<1>();
                    // Put its children in place in the tree.
                    std::function<bool(DBusMenuLayout &)> attach = [&](DBusMenuLayout &menu) {
                        for (DBusMenuLayout &child : menu.children) {
                            if (child.id == id) {
                                child.children = sub.children;
                                return true;
                            }
                            if (attach(child))
                                return true;
                        }
                        return false;
                    };
                    attach(m_root);
                    loadSubmenus(sub); // its own empty submenus
                }
                if (--m_pending == 0)
                    finishLoading();
            });
        }
    };
    visit(root);
}

void MenuSource::finishLoading()
{
    m_loaded = true;
    if (launcher()->currentSource() == this)
        publish();
}

void MenuSource::activate(int id)
{
    const QString service = m_service, path = m_path;
    // The app gets focus back first: menu commands often open a dialog.
    launcher()->runAfterHide([service, path, id] {
        QDBusConnection::sessionBus().asyncCall(menuCall(service, path, QStringLiteral("Event"),
                                                         {id, QStringLiteral("clicked"), QVariant::fromValue(QDBusVariant(QString())), 0u}));
    });
}

void MenuSource::query(const QString &text)
{
    m_query = text.trimmed();
    publish();
}

void MenuSource::publish()
{
    QList<Item> items;
    if (!m_loaded || m_service.isEmpty()) {
        Item status;
        status.icon = QIcon::fromTheme(QStringLiteral("application-menu"));
        if (!m_tracker || !m_tracker->isActive()) {
            status.title = tr("Menus need Plasma on Wayland");
            status.subtitle = tr("Run install.sh again so KWin tells Lumen which window is in front");
        } else if (m_service.isEmpty()) {
            // Apps only share their menus while Plasma's global menu is on.
            status.title = tr("This app shares no menu");
            status.subtitle = tr("Turn on the global menu: the application menu titlebar button (Window Decorations → Titlebar "
                                 "Buttons) or the Global Menu widget");
        } else {
            status.title = tr("Reading the menu…");
        }
        items << status;
        Q_EMIT resultsReady(items);
        return;
    }
    const QList<Entry> entries = flatten(m_root);
    auto self = this;
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &e = entries.at(i);
        const QString full = e.path.isEmpty() ? e.label : e.path + u' ' + e.label;
        const qreal score = m_query.isEmpty() ? 1000 - i : std::max(fuzzyScore(m_query, e.label), fuzzyScore(m_query, full) * 0.9);
        if (score <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("menu:%1").arg(e.id);
        item.title = e.checked ? e.label + QStringLiteral("  ✓") : e.label;
        item.subtitle = e.path;
        item.kind = e.shortcut;
        item.keyKind = !e.shortcut.isEmpty();
        item.icon = QIcon::fromTheme(QStringLiteral("application-menu"));
        item.score = score;
        const int id = e.id;
        item.actions << Action{tr("Run"), {}, {}, [self, id] { self->activate(id); }, true};
        items << item;
    }
    // Best match first; the menu's own order otherwise.
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    if (items.isEmpty() && entries.isEmpty()) {
        Item none;
        none.title = tr("The menu is empty");
        none.icon = QIcon::fromTheme(QStringLiteral("application-menu"));
        items << none;
    }
    Q_EMIT resultsReady(items);
}
