#pragma once

#include "source.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

// Installing software the way Raycast's Homebrew extension does: search the
// Arch repositories (pacman), the AUR (its web search; installed with yay
// or paru) and Flathub together, then install, remove or update with Enter.
// The work runs in the background, one job at a time; the selected
// package's output streams into the pane beside the list, and a
// notification says when it is done. Root rights come from Plasma's
// password dialog (pkexec).
class PackageManager : public QObject
{
    Q_OBJECT
public:
    enum Origin { Repo, Aur, Flatpak };
    struct Package {
        Origin origin = Repo;
        QString name;        // pacman/AUR name, or the Flatpak application id
        QString title;       // what is shown (Flatpak: the app's name)
        QString version;
        QString repo;        // "extra", "cachyos", "aur", "flathub"
        QString description;
        QString installedVersion; // empty when not installed
        int votes = -1;      // AUR
    };
    enum class Task { Install, Remove, Update, UpgradeAll };
    struct Job {
        int id = 0;
        Task task = Task::Install;
        Package package;
        QString output;
        bool running = false;
        bool finished = false;
        bool failed = false;
    };

    explicit PackageManager(QObject *parent = nullptr);

    // Installed packages, read again when asked (after a job, or on opening).
    void refreshInstalled();
    QString installedVersion(Origin origin, const QString &name) const;

    void run(Task task, const Package &package);
    const Job *jobFor(const QString &key) const; // the latest job for a package
    const Job *upgradeJob() const;
    bool busy() const { return m_current != nullptr; }

    static QString key(const Package &p) { return QString::number(p.origin) + u':' + p.name; }
    // The command a job runs (tested without running it).
    static QStringList commandFor(Task task, const Package &package, const QString &aurHelper);
    static QString aurHelper(); // "yay", "paru" or ""
    // Parsers for the tools' output.
    static QList<Package> parsePacmanSearch(const QString &output);
    static QList<Package> parseAurSearch(const QByteArray &json);
    static QList<Package> parseFlatpakSearch(const QString &output);

Q_SIGNALS:
    void jobsChanged();

private:
    void startNext();
    void notifyDone(const Job &job);

    QHash<QString, QString> m_installed; // key → version
    QList<Job> m_jobs;
    Job *m_current = nullptr;
    QPointer<QProcess> m_process;
    int m_nextId = 1;
};

// The "Packages" view: search results from all three, with details (or a
// job's live output) in the pane. An empty search lists what can be updated.
class PackageSource : public Source
{
    Q_OBJECT
public:
    PackageSource(Launcher *launcher, PackageManager *manager);

    void query(const QString &text) override;
    void sessionStarted() override;
    QString viewTitle() const override { return tr("Packages"); }
    QString viewPlaceholder() const override { return tr("Search pacman, the AUR and Flathub…"); }
    bool hasPreviewPane() const override { return true; }

private:
    void search();
    void checkUpdates();
    void publish();
    Item itemFor(const PackageManager::Package &p) const;

    PackageManager *m_manager;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_aurReply;
    QPointer<QProcess> m_pacman;
    QPointer<QProcess> m_flatpak;
    QString m_query;
    QList<PackageManager::Package> m_repo, m_aur, m_flat, m_updates;
    QSet<QString> m_updateKeys; // packages the update lists name
    bool m_updatesChecked = false;
    bool m_fresh = false; // installed packages read since Lumen opened
    QDateTime m_updatesAt;
    QTimer m_delay;   // typing pause before searching
    QTimer m_refresh; // output arriving: repaint at most this often
};

// "install firefox" / "remove gimp" in the main search: opens the view.
class PackageRootSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
};
