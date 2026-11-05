#pragma once

#include <KService>

#include <QHash>
#include <QKeySequence>
#include <QObject>

class QAction;

// Global shortcuts that bring up an app: its window comes to the front, or
// the app starts if it is not running; pressed while the app is in front,
// the shortcut minimizes it. Kept in lumenrc [AppHotkeys] and registered with
// KDE's shortcut service, so they also show in System Settings → Shortcuts
// → Lumen.
class AppHotkeys : public QObject
{
    Q_OBJECT
public:
    explicit AppHotkeys(QObject *parent = nullptr);

    // Returns false (and sets *conflict to what uses it) when taken.
    bool set(const KService::Ptr &app, const QKeySequence &keys, QString *conflict);
    void remove(const QString &storageId);
    QKeySequence keysFor(const QString &storageId) const;
    QStringList apps() const { return m_actions.keys(); }

    // Brings up (or starts, or minimizes) the app with this desktop entry.
    static void activate(const QString &storageId);

private:
    QAction *actionFor(const KService::Ptr &app);

    QHash<QString, QAction *> m_actions; // by storage id
};
