#pragma once

#include "source.h"

#include <QDBusArgument>
#include <QVariantMap>

#include <memory>

// One entry of an app's menu as com.canonical.dbusmenu describes it:
// (ia{sv}av), the children being variants holding the same structure.
struct DBusMenuLayout {
    int id = 0;
    QVariantMap properties;
    QList<DBusMenuLayout> children;
};
Q_DECLARE_METATYPE(DBusMenuLayout)
QDBusArgument &operator<<(QDBusArgument &argument, const DBusMenuLayout &item);
const QDBusArgument &operator>>(const QDBusArgument &argument, DBusMenuLayout &item);

class WindowTracker;

// "Search Menu Items": every command in the menu bar of the app that was in
// front when Lumen opened ("File › Save As…"), run with Enter. Apps publish
// their menus over D-Bus (com.canonical.dbusmenu) for Plasma's global menu;
// which menu belongs to which window comes from Plasma's window-management
// protocol, which KWin offers only to programs whose desktop entry asks for
// it (install.sh sets that up).
class MenuSource : public Source
{
    Q_OBJECT
public:
    explicit MenuSource(Launcher *launcher);
    ~MenuSource() override;

    void query(const QString &text) override;
    void sessionStarted() override;
    QString viewTitle() const override;
    QString viewPlaceholder() const override { return tr("Search menu commands…"); }

    // Reads the menu at a D-Bus address (also used by the tests).
    void load(const QString &service, const QString &path, const QString &appName);

    struct Entry {
        int id = 0;
        QString label;
        QString path;     // the menus it is in: "File › Recent"
        QString shortcut; // "Ctrl+S"
        bool checked = false;
    };
    // The commands in a menu tree, without separators, hidden or disabled ones.
    static QList<Entry> flatten(const DBusMenuLayout &root);
    // "_Save &As" → "Save As" (mnemonics).
    static QString plainLabel(const QString &label);

private:
    void loadSubmenus(const DBusMenuLayout &root);
    void publish();
    void finishLoading();
    void activate(int id);

    std::unique_ptr<WindowTracker> m_tracker;
    QString m_service;
    QString m_path;
    QString m_appName;
    DBusMenuLayout m_root;
    int m_pending = 0; // submenus still loading
    bool m_loaded = false;
    QString m_query;
};
