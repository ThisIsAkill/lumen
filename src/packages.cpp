#include "packages.h"
#include "fuzzy.h"
#include "launcher.h"
#include "outputview.h"

#include <KApplicationTrader>
#include <KIO/OpenUrlJob>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace
{

constexpr int MaxPerOrigin = 25;
constexpr int UpdateCheckSeconds = 600; // how old the list of updates may be
constexpr int SearchTimeout = 8000;     // ms for pacman, flatpak or the AUR to answer

QString originName(PackageManager::Origin origin)
{
    switch (origin) {
    case PackageManager::Repo:
        return QObject::tr("Repositories");
    case PackageManager::Aur:
        return QObject::tr("AUR");
    case PackageManager::Flatpak:
        return QObject::tr("Flathub");
    }
    return {};
}

QString verb(PackageManager::Task task, bool ongoing)
{
    switch (task) {
    case PackageManager::Task::Install:
        return ongoing ? QObject::tr("Installing") : QObject::tr("Installed");
    case PackageManager::Task::Remove:
        return ongoing ? QObject::tr("Removing") : QObject::tr("Removed");
    case PackageManager::Task::Update:
        return ongoing ? QObject::tr("Updating") : QObject::tr("Updated");
    case PackageManager::Task::UpgradeAll:
        return ongoing ? QObject::tr("Upgrading everything") : QObject::tr("Upgraded everything");
    }
    return {};
}

QString lastLine(const QString &text)
{
    const QStringList lines = text.split(u'\n', Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it)
        if (!it->trimmed().isEmpty())
            return it->trimmed();
    return {};
}

} // namespace

// ------------------------------------------------------------- manager

PackageManager::PackageManager(QObject *parent)
    : QObject(parent)
{
}

QString PackageManager::aurHelper()
{
    for (const char *helper : {"yay", "paru"})
        if (!QStandardPaths::findExecutable(QLatin1String(helper)).isEmpty())
            return QLatin1String(helper);
    return {};
}

void PackageManager::refreshInstalled()
{
    // Both run in the background; results replace the old lists when in.
    auto *pacman = new QProcess(this);
    connect(pacman, &QProcess::finished, this, [this, pacman] {
        pacman->deleteLater();
        QHash<QString, QString> fresh;
        for (auto it = m_installed.cbegin(); it != m_installed.cend(); ++it)
            if (it.key().startsWith(QLatin1String("f:")))
                fresh.insert(it.key(), it.value());
        for (const QString &line : QString::fromUtf8(pacman->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts))
            fresh.insert(QStringLiteral("p:") + line.section(u' ', 0, 0), line.section(u' ', 1, 1));
        m_installed = fresh;
        Q_EMIT jobsChanged();
    });
    pacman->start(QStringLiteral("pacman"), {QStringLiteral("-Q")});
    if (QStandardPaths::findExecutable(QStringLiteral("flatpak")).isEmpty())
        return;
    auto *flatpak = new QProcess(this);
    connect(flatpak, &QProcess::finished, this, [this, flatpak] {
        flatpak->deleteLater();
        m_installed.removeIf([](const auto &it) { return it.key().startsWith(QLatin1String("f:")); });
        for (const QString &line : QString::fromUtf8(flatpak->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts))
            m_installed.insert(QStringLiteral("f:") + line.section(u'\t', 0, 0), line.section(u'\t', 1, 1));
        Q_EMIT jobsChanged();
    });
    flatpak->start(QStringLiteral("flatpak"), {QStringLiteral("list"), QStringLiteral("--app"), QStringLiteral("--columns=application,version")});
}

QString PackageManager::installedVersion(Origin origin, const QString &name) const
{
    return m_installed.value((origin == Flatpak ? QStringLiteral("f:") : QStringLiteral("p:")) + name);
}

QStringList PackageManager::commandFor(Task task, const Package &p, const QString &aurHelper)
{
    // AUR helpers build as the user and ask for root (through pkexec) only
    // to install; answers are given up front so nothing waits for a terminal.
    QStringList aurFlags{QStringLiteral("--noconfirm"), QStringLiteral("--needed"), QStringLiteral("--sudo"), QStringLiteral("pkexec")};
    if (aurHelper == QLatin1String("yay"))
        aurFlags << QStringLiteral("--answerdiff") << QStringLiteral("None") << QStringLiteral("--answerclean") << QStringLiteral("None")
                 << QStringLiteral("--removemake");
    else
        aurFlags << QStringLiteral("--skipreview") << QStringLiteral("--removemake");
    switch (task) {
    case Task::Install:
    case Task::Update:
        if (p.origin == Flatpak)
            return task == Task::Install ? QStringList{QStringLiteral("flatpak"), QStringLiteral("install"), QStringLiteral("-y"),
                                                       QStringLiteral("--noninteractive"), p.repo.isEmpty() ? QStringLiteral("flathub") : p.repo, p.name}
                                         : QStringList{QStringLiteral("flatpak"), QStringLiteral("update"), QStringLiteral("-y"),
                                                       QStringLiteral("--noninteractive"), p.name};
        if (p.origin == Aur)
            return QStringList{aurHelper, QStringLiteral("-S")} + aurFlags + QStringList{p.name};
        // A repository package alone: never "-Sy", which would leave the
        // system half upgraded; updates of these go through Upgrade All.
        return {QStringLiteral("pkexec"), QStringLiteral("pacman"), QStringLiteral("-S"), QStringLiteral("--noconfirm"), QStringLiteral("--needed"), p.name};
    case Task::Remove:
        if (p.origin == Flatpak)
            return {QStringLiteral("flatpak"), QStringLiteral("uninstall"), QStringLiteral("-y"), QStringLiteral("--noninteractive"), p.name};
        return {QStringLiteral("pkexec"), QStringLiteral("pacman"), QStringLiteral("-Rns"), QStringLiteral("--noconfirm"), p.name};
    case Task::UpgradeAll: {
        QStringList steps{QStringLiteral("pkexec pacman -Syu --noconfirm")};
        if (!aurHelper.isEmpty())
            steps << aurHelper + QStringLiteral(" -Sua ") + aurFlags.join(u' ');
        if (!QStandardPaths::findExecutable(QStringLiteral("flatpak")).isEmpty())
            steps << QStringLiteral("flatpak update -y --noninteractive");
        return {QStringLiteral("sh"), QStringLiteral("-c"), steps.join(QStringLiteral(" && "))};
    }
    }
    return {};
}

void PackageManager::run(Task task, const Package &package)
{
    Job job;
    job.id = m_nextId++;
    job.task = task;
    job.package = package;
    m_jobs << job;
    startNext();
    Q_EMIT jobsChanged();
}

const PackageManager::Job *PackageManager::jobFor(const QString &k) const
{
    for (auto it = m_jobs.crbegin(); it != m_jobs.crend(); ++it)
        if (it->task != Task::UpgradeAll && key(it->package) == k)
            return &*it;
    return nullptr;
}

const PackageManager::Job *PackageManager::upgradeJob() const
{
    for (auto it = m_jobs.crbegin(); it != m_jobs.crend(); ++it)
        if (it->task == Task::UpgradeAll)
            return &*it;
    return nullptr;
}

// One job at a time: pacman allows a single transaction.
void PackageManager::startNext()
{
    if (m_current)
        return;
    const auto next = std::find_if(m_jobs.begin(), m_jobs.end(), [](const Job &j) { return !j.running && !j.finished; });
    if (next == m_jobs.end())
        return;
    m_current = &*next;
    m_current->running = true;
    const QStringList argv = commandFor(m_current->task, m_current->package, aurHelper());
    m_current->output = QStringLiteral("$ ") + argv.join(u' ') + u'\n';
    const int id = m_current->id;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, [this, id] {
        if (m_current && m_current->id == id) {
            m_current->output += OutputView::plain(QString::fromUtf8(m_process->readAll()));
            Q_EMIT jobsChanged();
        }
    });
    connect(m_process, &QProcess::finished, this, [this, id](int code, QProcess::ExitStatus status) {
        if (!m_current || m_current->id != id)
            return;
        m_current->output += OutputView::plain(QString::fromUtf8(m_process->readAll()));
        m_current->running = false;
        m_current->finished = true;
        m_current->failed = status != QProcess::NormalExit || code != 0;
        m_process->deleteLater();
        const Job done = *m_current;
        m_current = nullptr;
        notifyDone(done);
        refreshInstalled();
        Q_EMIT jobsChanged();
        startNext();
    });
    m_process->start(argv.first(), argv.mid(1));
}

void PackageManager::notifyDone(const Job &job)
{
    const QString what = job.task == Task::UpgradeAll ? QString() : (job.package.title.isEmpty() ? job.package.name : job.package.title);
    QString summary;
    if (job.failed)
        summary = job.task == Task::UpgradeAll ? tr("The upgrade did not finish") : tr("Could not change %1").arg(what);
    else
        summary = job.task == Task::UpgradeAll ? verb(job.task, false) : tr("%1 %2").arg(verb(job.task, false), what);
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                                              QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    msg.setArguments({QStringLiteral("Lumen"), 0u, job.failed ? QStringLiteral("dialog-error") : QStringLiteral("package-x-generic"), summary,
                      job.failed ? lastLine(job.output) : originName(job.package.origin), QStringList(),
                      QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral("io.github.lumen")}}, 6000});
    QDBusConnection::sessionBus().asyncCall(msg);
}

// -------------------------------------------------------------- parsing

QList<PackageManager::Package> PackageManager::parsePacmanSearch(const QString &output)
{
    // "repo/name 1.2-1 (group) [installed: 1.1-1]" then an indented description.
    QList<Package> out;
    const QStringList lines = output.split(u'\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        if (line.isEmpty() || line.startsWith(u' ') || !line.contains(u'/'))
            continue;
        Package p;
        p.origin = Repo;
        p.repo = line.section(u'/', 0, 0);
        p.name = line.section(u'/', 1).section(u' ', 0, 0);
        p.title = p.name;
        p.version = line.section(u' ', 1, 1);
        if (i + 1 < lines.size() && lines.at(i + 1).startsWith(u' '))
            p.description = lines.at(++i).trimmed();
        // The same name in several repositories: pacman installs from the
        // first one listed, so that is the one to show.
        if (std::none_of(out.cbegin(), out.cend(), [&](const Package &o) { return o.name == p.name; }))
            out << p;
    }
    return out;
}

QList<PackageManager::Package> PackageManager::parseAurSearch(const QByteArray &json)
{
    QList<Package> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).object().value(QStringLiteral("results")).toArray()) {
        const QJsonObject o = v.toObject();
        Package p;
        p.origin = Aur;
        p.repo = QStringLiteral("aur");
        p.name = o.value(QStringLiteral("Name")).toString();
        p.title = p.name;
        p.version = o.value(QStringLiteral("Version")).toString();
        p.description = o.value(QStringLiteral("Description")).toString();
        p.votes = o.value(QStringLiteral("NumVotes")).toInt();
        if (!p.name.isEmpty())
            out << p;
    }
    // The most voted first: the AUR has many near-duplicates.
    std::stable_sort(out.begin(), out.end(), [](const Package &a, const Package &b) { return a.votes > b.votes; });
    return out;
}

QList<PackageManager::Package> PackageManager::parseFlatpakSearch(const QString &output)
{
    // --columns=name,application,version,remotes,description, tab separated.
    QList<Package> out;
    for (const QString &line : output.split(u'\n', Qt::SkipEmptyParts)) {
        const QStringList f = line.split(u'\t');
        if (f.size() < 5 || !f.at(1).contains(u'.'))
            continue; // "No matches found"
        Package p;
        p.origin = Flatpak;
        p.title = f.at(0);
        p.name = f.at(1);
        p.version = f.at(2);
        p.repo = f.at(3).section(u',', 0, 0);
        p.description = f.at(4);
        out << p;
    }
    return out;
}

// ---------------------------------------------------------------- view

PackageSource::PackageSource(Launcher *launcher, PackageManager *manager)
    : Source(launcher)
    , m_manager(manager)
    , m_network(new QNetworkAccessManager(this))
{
    m_delay.setSingleShot(true);
    m_delay.setInterval(250);
    connect(&m_delay, &QTimer::timeout, this, &PackageSource::search);
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(150);
    connect(&m_refresh, &QTimer::timeout, this, &PackageSource::publish);
    connect(manager, &PackageManager::jobsChanged, this, [this] {
        if (this->launcher()->currentSource() == this && !m_refresh.isActive())
            m_refresh.start();
    });
}

void PackageSource::sessionStarted()
{
    m_fresh = false; // read the installed packages again, if the view is used
}

void PackageSource::query(const QString &text)
{
    m_query = text.trimmed();
    if (!m_fresh) {
        // Only when the view is used: what is installed, and (in the
        // background, at most every few minutes) what can be updated.
        m_fresh = true;
        m_manager->refreshInstalled();
        if (!m_updatesChecked || m_updatesAt.secsTo(QDateTime::currentDateTime()) > UpdateCheckSeconds)
            checkUpdates();
    }
    if (m_query.isEmpty()) {
        publish();
        return;
    }
    m_delay.start();
    publish();
}

void PackageSource::search()
{
    const QString term = m_query;
    if (term.isEmpty())
        return;
    for (QProcess *p : {m_pacman.data(), m_flatpak.data()})
        if (p) {
            p->disconnect(this);
            p->kill();
        }
    if (m_aurReply)
        m_aurReply->abort();
    m_repo.clear();
    m_aur.clear();
    m_flat.clear();

    m_pacman = new QProcess(this);
    connect(m_pacman, &QProcess::finished, this, [this, term, proc = m_pacman.data()] {
        proc->deleteLater();
        if (term == m_query) {
            m_repo = PackageManager::parsePacmanSearch(QString::fromUtf8(proc->readAllStandardOutput()));
            publish();
        }
    });
    m_pacman->start(QStringLiteral("pacman"), QStringList{QStringLiteral("-Ss"), QStringLiteral("--")} + term.split(u' ', Qt::SkipEmptyParts));
    QTimer::singleShot(SearchTimeout, m_pacman, [p = m_pacman] { p->kill(); });

    if (!QStandardPaths::findExecutable(QStringLiteral("flatpak")).isEmpty()) {
        m_flatpak = new QProcess(this);
        connect(m_flatpak, &QProcess::finished, this, [this, term, proc = m_flatpak.data()] {
            proc->deleteLater();
            if (term == m_query) {
                m_flat = PackageManager::parseFlatpakSearch(QString::fromUtf8(proc->readAllStandardOutput()));
                publish();
            }
        });
        m_flatpak->start(QStringLiteral("flatpak"), {QStringLiteral("search"), QStringLiteral("--columns=name,application,version,remotes,description"), term});
        QTimer::singleShot(SearchTimeout, m_flatpak, [p = m_flatpak] { p->kill(); });
    }

    // The AUR's own search (two letters at least), by name and description.
    if (term.size() >= 2 && !PackageManager::aurHelper().isEmpty()) {
        QUrl url(QStringLiteral("https://aur.archlinux.org/rpc/v5/search/") + QString::fromUtf8(QUrl::toPercentEncoding(term.section(u' ', 0, 0))));
        url.setQuery(QStringLiteral("by=name-desc"));
        QNetworkRequest request(url);
        request.setTransferTimeout(SearchTimeout);
        m_aurReply = m_network->get(request);
        connect(m_aurReply, &QNetworkReply::finished, this, [this, term, reply = m_aurReply.data()] {
            reply->deleteLater();
            if (term == m_query && reply->error() == QNetworkReply::NoError) {
                m_aur = PackageManager::parseAurSearch(reply->readAll());
                publish();
            }
        });
    }
}

void PackageSource::checkUpdates()
{
    m_updatesChecked = true;
    m_updatesAt = QDateTime::currentDateTime();
    m_updates.clear();
    m_updateKeys.clear();
    // "name 1.0-1 -> 1.1-1" from checkupdates (repositories, without touching
    // the system's package database) and the AUR helper; Flatpak's own list.
    auto parseArrows = [](const QString &out, PackageManager::Origin origin) {
        QList<PackageManager::Package> list;
        for (const QString &line : out.split(u'\n', Qt::SkipEmptyParts)) {
            const QStringList f = line.split(u' ', Qt::SkipEmptyParts);
            if (f.size() < 4 || f.at(2) != QLatin1String("->"))
                continue;
            PackageManager::Package p;
            p.origin = origin;
            p.name = p.title = f.at(0);
            p.installedVersion = f.at(1);
            p.version = f.at(3);
            p.repo = origin == PackageManager::Aur ? QStringLiteral("aur") : QString();
            list << p;
        }
        return list;
    };
    auto start = [this](const QString &program, const QStringList &args, std::function<void(const QString &)> done) {
        if (QStandardPaths::findExecutable(program).isEmpty())
            return;
        auto *proc = new QProcess(this);
        connect(proc, &QProcess::finished, this, [this, proc, done] {
            proc->deleteLater();
            done(QString::fromUtf8(proc->readAllStandardOutput()));
            for (const PackageManager::Package &p : std::as_const(m_updates))
                m_updateKeys.insert(PackageManager::key(p));
            publish();
        });
        proc->start(program, args);
        QTimer::singleShot(60000, proc, [proc] { proc->kill(); });
    };
    start(QStringLiteral("checkupdates"), {}, [this, parseArrows](const QString &out) { m_updates << parseArrows(out, PackageManager::Repo); });
    if (const QString helper = PackageManager::aurHelper(); !helper.isEmpty())
        start(helper, {QStringLiteral("-Qua")}, [this, parseArrows](const QString &out) { m_updates << parseArrows(out, PackageManager::Aur); });
    start(QStringLiteral("flatpak"), {QStringLiteral("remote-ls"), QStringLiteral("--updates"), QStringLiteral("--app"), QStringLiteral("--columns=name,application,version")},
          [this](const QString &out) {
              for (const QString &line : out.split(u'\n', Qt::SkipEmptyParts)) {
                  const QStringList f = line.split(u'\t');
                  if (f.size() < 3)
                      continue;
                  PackageManager::Package p;
                  p.origin = PackageManager::Flatpak;
                  p.title = f.at(0);
                  p.name = f.at(1);
                  p.version = f.at(2);
                  p.installedVersion = QStringLiteral("installed");
                  m_updates << p;
              }
          });
}

Item PackageSource::itemFor(const PackageManager::Package &package) const
{
    PackageManager::Package p = package;
    if (p.installedVersion.isEmpty())
        p.installedVersion = m_manager->installedVersion(p.origin, p.name);
    const bool installed = !p.installedVersion.isEmpty();
    // Only the update lists say what is newer: versions from different
    // repositories (or a rebuild) cannot be compared by their text. Search
    // results of repository packages are never "updates"; Upgrade All
    // brings those up to date.
    const bool outdated = installed && m_updateKeys.contains(PackageManager::key(p));
    const QString key = PackageManager::key(p);
    const PackageManager::Job *job = m_manager->jobFor(key);

    Item item;
    item.id = QStringLiteral("package:") + key;
    item.title = p.title.isEmpty() ? p.name : p.title;
    item.subtitle = p.description;
    item.icon = QIcon::fromTheme(p.name, QIcon::fromTheme(p.origin == PackageManager::Flatpak ? QStringLiteral("flatpak-discover")
                                                                                               : QStringLiteral("package-x-generic"),
                                                           QIcon::fromTheme(QStringLiteral("package-x-generic"))));
    // Origin and state on the right: "extra · 1.2-1", "AUR · ★ 812", "Installed".
    QString where = p.origin == PackageManager::Aur ? tr("AUR") : p.origin == PackageManager::Flatpak ? tr("Flathub") : p.repo;
    if (p.origin == PackageManager::Aur && p.votes >= 0)
        where += QStringLiteral(" · ★ %1").arg(p.votes);
    if (job && job->running)
        item.kind = tr("%1…").arg(verb(job->task, true));
    else if (job && job->finished && job->failed)
        item.kind = tr("Failed");
    else if (outdated)
        item.kind = tr("Update · %1").arg(where);
    else if (installed)
        item.kind = tr("Installed · %1").arg(where);
    else
        item.kind = where;

    // The pane: the job's output while it runs (and after), else the details.
    if (job) {
        item.previewText = job->output;
        item.monospace = true;
    } else {
        QStringList details{item.title};
        if (!p.description.isEmpty())
            details << p.description << QString();
        details << tr("Source: %1").arg(originName(p.origin) + (p.origin == PackageManager::Repo ? QStringLiteral(" (%1)").arg(p.repo) : QString()));
        details << tr("Name: %1").arg(p.name);
        if (!p.version.isEmpty())
            details << tr("Version: %1").arg(p.version);
        if (installed)
            details << tr("Installed: %1").arg(p.installedVersion == QLatin1String("installed") ? tr("yes") : p.installedVersion);
        if (p.votes >= 0)
            details << tr("Votes: %1").arg(p.votes);
        const QString helper = PackageManager::aurHelper();
        details << QString();
        if (!installed)
            details << tr("Install runs: %1").arg(PackageManager::commandFor(PackageManager::Task::Install, p, helper).join(u' '));
        if (outdated && p.origin != PackageManager::Repo)
            details << tr("Update runs: %1").arg(PackageManager::commandFor(PackageManager::Task::Update, p, helper).join(u' '));
        if (installed)
            details << tr("Remove (Ctrl+Backspace) runs: %1").arg(PackageManager::commandFor(PackageManager::Task::Remove, p, helper).join(u' '));
        item.previewText = details.join(u'\n');
    }

    Launcher *l = launcher();
    PackageManager *manager = m_manager;
    const bool working = job && job->running;
    auto task = [manager, p](PackageManager::Task t) { return [manager, p, t] { manager->run(t, p); }; };
    const bool needsHelper = p.origin == PackageManager::Aur && PackageManager::aurHelper().isEmpty();
    if (working) {
        item.actions << Action{tr("Show Output"), {}, {}, [] { }, true};
    } else if (needsHelper) {
        item.actions << Action{tr("Install yay First"), {}, {}, [l] { l->notify(tr("AUR packages need yay or paru")); }, true};
    } else if (!installed) {
        item.actions << Action{tr("Install"), {}, {}, task(PackageManager::Task::Install), true};
    } else if (outdated && p.origin != PackageManager::Repo) {
        item.actions << Action{tr("Update"), {}, {}, task(PackageManager::Task::Update), true};
    } else {
        // Installed: open it if it is an app; removing is one key away.
        const KService::List apps = KApplicationTrader::query([&](const KService::Ptr &s) {
            return s->desktopEntryName() == p.name.toLower() || s->exec().section(u' ', 0, 0) == p.name;
        });
        if (!apps.isEmpty()) {
            const QString storageId = apps.first()->storageId();
            item.actions << Action{tr("Open %1").arg(apps.first()->name()), {}, {}, [l, storageId] { l->launchApp(storageId); }};
        } else {
            item.actions << Action{tr("Reinstall"), {}, {}, task(PackageManager::Task::Install), true};
        }
    }
    if (installed && !working)
        item.actions << Action{tr("Remove"), QStringLiteral("edit-delete"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), task(PackageManager::Task::Remove), true};
    QUrl page;
    if (p.origin == PackageManager::Aur)
        page = QUrl(QStringLiteral("https://aur.archlinux.org/packages/") + p.name);
    else if (p.origin == PackageManager::Flatpak)
        page = QUrl(QStringLiteral("https://flathub.org/apps/") + p.name);
    else
        page = QUrl(QStringLiteral("https://archlinux.org/packages/?q=") + p.name);
    item.actions << Action{tr("Open Package Page"), QStringLiteral("internet-web-browser"), QKeySequence(Qt::CTRL | Qt::Key_O),
                           [page] { (new KIO::OpenUrlJob(page))->start(); }};
    const QString name = p.name;
    item.actions << Action{tr("Copy Name"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), [l, name] { l->copyText(name); }};
    return item;
}

void PackageSource::publish()
{
    QList<Item> items;
    Launcher *l = launcher();
    PackageManager *manager = m_manager;
    if (m_query.isEmpty()) {
        // What can be updated, with Upgrade All first.
        const PackageManager::Job *upgrade = manager->upgradeJob();
        Item all;
        all.id = QStringLiteral("package:upgrade-all");
        all.icon = QIcon::fromTheme(QStringLiteral("system-software-update"));
        if (upgrade && upgrade->running) {
            all.title = tr("Upgrading everything…");
            all.kind = tr("Running");
        } else if (m_updates.isEmpty()) {
            all.title = m_updatesAt.secsTo(QDateTime::currentDateTime()) < 3 ? tr("Checking for updates…") : tr("Everything is up to date");
            all.subtitle = tr("Type to search pacman, the AUR and Flathub");
        } else {
            all.title = tr("Upgrade All");
            all.subtitle = tr("%1 updates: repositories, AUR and Flatpak").arg(m_updates.size());
        }
        if (upgrade) {
            all.previewText = upgrade->output;
            all.monospace = true;
        } else {
            QStringList lines;
            for (const PackageManager::Package &p : std::as_const(m_updates))
                lines << (p.installedVersion == QLatin1String("installed") ? QStringLiteral("%1  → %2").arg(p.title, p.version)
                                                                           : QStringLiteral("%1  %2 → %3").arg(p.title, p.installedVersion, p.version));
            all.previewText = lines.join(u'\n');
            all.monospace = true;
        }
        if (!m_updates.isEmpty() && !(upgrade && upgrade->running)) {
            PackageManager::Package none;
            all.actions << Action{tr("Upgrade All"), {}, {}, [manager, none] { manager->run(PackageManager::Task::UpgradeAll, none); }, true};
        }
        items << all;
        for (const PackageManager::Package &p : std::as_const(m_updates))
            items << itemFor(p);
        Q_EMIT resultsReady(items);
        return;
    }
    // Search: the best name matches first, the repositories ahead of the
    // AUR and Flathub when equal.
    auto add = [&](const QList<PackageManager::Package> &list, qreal bias) {
        int n = 0;
        for (const PackageManager::Package &p : list) {
            if (n++ == MaxPerOrigin)
                break;
            Item item = itemFor(p);
            qreal score = std::max(fuzzyScore(m_query, p.name), fuzzyScore(m_query, p.title));
            if (p.name.compare(m_query, Qt::CaseInsensitive) == 0 || p.title.compare(m_query, Qt::CaseInsensitive) == 0)
                score += 60;
            if (p.votes > 0)
                score += std::log10(double(p.votes) + 1) * 3;
            item.score = std::max<qreal>(score, 1) + bias;
            items << item;
        }
    };
    add(m_repo, 6);
    add(m_flat, 3);
    add(m_aur, 0);
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    if (items.isEmpty()) {
        Item searching;
        searching.title = m_delay.isActive() || m_pacman || m_flatpak || m_aurReply ? tr("Searching…") : tr("No packages found");
        searching.icon = QIcon::fromTheme(QStringLiteral("package-x-generic"));
        items << searching;
    }
    Q_UNUSED(l);
    Q_EMIT resultsReady(items);
}

// ---------------------------------------------------------------- root

void PackageRootSource::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    for (const char *word : {"install ", "uninstall ", "remove "}) {
        if (q.startsWith(QLatin1String(word), Qt::CaseInsensitive) && q.size() > int(qstrlen(word))) {
            const QString term = q.mid(int(qstrlen(word))).trimmed();
            Item item;
            item.id = QStringLiteral("lumen:packages-for");
            item.title = tr("Search Packages for “%1”").arg(term);
            item.subtitle = tr("pacman, the AUR and Flathub");
            item.kind = tr("Packages");
            item.icon = QIcon::fromTheme(QStringLiteral("system-software-install"));
            item.score = 118;
            Launcher *l = launcher();
            item.actions << Action{tr("Search"), {}, {}, [l, term] { l->openView(Launcher::PackagesView, term); }, true};
            items << item;
            break;
        }
    }
    Q_EMIT resultsReady(items);
}
