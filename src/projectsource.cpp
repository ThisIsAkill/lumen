#include "projectsource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KIO/OpenFileManagerWindowJob>
#include <KSharedConfig>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

namespace
{

constexpr int RescanMinutes = 10;

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Projects"));
}

QString tildePath(const QString &path)
{
    const QString home = QDir::homePath();
    return path.startsWith(home) ? u'~' + path.mid(home.size()) : path;
}

void openTerminal(const QStringList &args)
{
    QProcess::startDetached(QStringLiteral("konsole"), args);
}

} // namespace

QStringList ProjectSource::roots()
{
    QStringList configured = config().readEntry("Roots", QStringList());
    if (!configured.isEmpty())
        return configured;
    QStringList found;
    for (const char *name : {"Documents/Coding", "Projects", "projects", "src", "code", "Code", "dev", "git", "repos", "workspace"}) {
        const QString path = QDir::home().filePath(QLatin1String(name));
        if (QFileInfo(path).isDir())
            found << path;
    }
    return found;
}

QStringList ProjectSource::editorCommand()
{
    const QString configured = config().readEntry("Editor");
    if (!configured.isEmpty())
        return QProcess::splitCommand(configured);
    for (const char *editor : {"code", "codium", "zed", "kate", "kwrite"})
        if (!QStandardPaths::findExecutable(QLatin1String(editor)).isEmpty())
            return {QLatin1String(editor)};
    return {};
}

QString ProjectSource::gitBranch(const QString &repo)
{
    QFile head(repo + QStringLiteral("/.git/HEAD"));
    if (!head.open(QIODevice::ReadOnly))
        return {};
    const QString ref = QString::fromUtf8(head.readAll()).trimmed();
    if (ref.startsWith(QLatin1String("ref: refs/heads/")))
        return ref.mid(16);
    return ref.left(7); // a detached commit
}

QStringList ProjectSource::sshHosts(const QString &configFile, const QString &knownHostsFile)
{
    QStringList hosts;
    QSet<QString> seen;
    auto add = [&](const QString &host) {
        if (!host.isEmpty() && !host.contains(u'*') && !host.contains(u'?') && !host.startsWith(u'!') && !seen.contains(host)) {
            seen.insert(host);
            hosts << host;
        }
    };
    QFile config(configFile);
    if (config.open(QIODevice::ReadOnly)) {
        static const QRegularExpression hostLine(QStringLiteral("^\\s*Host\\s+(.+)$"), QRegularExpression::CaseInsensitiveOption);
        for (const QString &line : QString::fromUtf8(config.readAll()).split(u'\n')) {
            const auto m = hostLine.match(line);
            if (m.hasMatch())
                for (const QString &h : m.captured(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
                    add(h);
        }
    }
    QFile known(knownHostsFile);
    if (known.open(QIODevice::ReadOnly)) {
        for (const QString &line : QString::fromUtf8(known.readAll()).split(u'\n')) {
            if (line.startsWith(u'|') || line.startsWith(u'#') || line.startsWith(u'@'))
                continue; // hashed names cannot be read; markers are not hosts
            for (QString h : line.section(u' ', 0, 0).split(u',', Qt::SkipEmptyParts)) {
                if (h.startsWith(u'[')) // "[host]:2222"
                    h = h.mid(1, h.indexOf(u']') - 1);
                add(h);
            }
        }
    }
    return hosts;
}

void ProjectSource::sessionStarted()
{
    if (!m_scanned.isValid() || m_scanned.secsTo(QDateTime::currentDateTime()) > RescanMinutes * 60)
        scan();
}

void ProjectSource::scan()
{
    m_scanned = QDateTime::currentDateTime();
    m_projects.clear();
    QSet<QString> seen;
    for (const QString &root : roots()) {
        // The root itself, its folders, and theirs (two levels).
        QStringList candidates{root};
        for (const QFileInfo &d : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            candidates << d.absoluteFilePath();
            for (const QFileInfo &dd : QDir(d.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
                candidates << dd.absoluteFilePath();
        }
        for (const QString &c : std::as_const(candidates)) {
            if (QFileInfo::exists(c + QStringLiteral("/.git")) && !seen.contains(c)) {
                seen.insert(c);
                m_projects << Project{QFileInfo(c).fileName(), c};
            }
        }
    }
    const QString ssh = QDir::home().filePath(QStringLiteral(".ssh"));
    m_hosts = sshHosts(ssh + QStringLiteral("/config"), ssh + QStringLiteral("/known_hosts"));
}

void ProjectSource::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    if (q.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    if (!m_scanned.isValid())
        scan();
    Launcher *l = launcher();

    const bool allProjects = fuzzyScore(q, QStringLiteral("projects")) >= 80 || fuzzyScore(q, QStringLiteral("repos")) >= 80;
    const QStringList editor = editorCommand();
    for (int i = 0; i < m_projects.size(); ++i) {
        const Project &p = m_projects.at(i);
        const qreal score = allProjects ? 70 - i * 0.01 : fuzzyScore(q, p.name);
        if (score < 40)
            continue;
        Item item;
        item.id = QStringLiteral("project:") + p.path;
        item.title = p.name;
        const QString branch = gitBranch(p.path);
        item.subtitle = tildePath(p.path) + (branch.isEmpty() ? QString() : QStringLiteral(" · ") + branch);
        item.kind = tr("Project");
        item.icon = QIcon::fromTheme(QStringLiteral("folder-git"), QIcon::fromTheme(QStringLiteral("folder-development")));
        item.filePath = p.path;
        item.score = score;
        const QString path = p.path;
        if (!editor.isEmpty())
            item.actions << Action{tr("Open in %1").arg(editor.first()), {}, {}, [editor, path] {
                QProcess::startDetached(editor.first(), editor.mid(1) << path);
            }};
        item.actions << Action{tr("Open in Konsole"), QStringLiteral("utilities-terminal"), QKeySequence(Qt::CTRL | Qt::Key_T),
                               [path] { openTerminal({QStringLiteral("--workdir"), path}); }};
        item.actions << Action{tr("Open in Dolphin"), QStringLiteral("system-file-manager"), QKeySequence(Qt::CTRL | Qt::Key_Return),
                               [path] { KIO::highlightInFileManager({QUrl::fromLocalFile(path)}); }};
        item.actions << Action{tr("Copy Path"), QStringLiteral("edit-copy-path"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                               [l, path] { l->copyText(path); }};
        items << item;
    }

    const bool allHosts = q.compare(QLatin1String("ssh"), Qt::CaseInsensitive) == 0;
    const QString hostQuery = q.startsWith(QLatin1String("ssh "), Qt::CaseInsensitive) ? q.mid(4) : q;
    for (int i = 0; i < m_hosts.size(); ++i) {
        const QString host = m_hosts.at(i);
        const qreal score = allHosts ? 70 - i * 0.01 : fuzzyScore(hostQuery, host) * (hostQuery == q ? 0.9 : 1.1);
        if (score < 40)
            continue;
        Item item;
        item.id = QStringLiteral("ssh:") + host;
        item.title = tr("SSH to %1").arg(host);
        item.subtitle = tr("Opens a Konsole session");
        item.kind = tr("SSH");
        item.icon = QIcon::fromTheme(QStringLiteral("network-server"), QIcon::fromTheme(QStringLiteral("utilities-terminal")));
        item.score = score;
        item.actions << Action{tr("Connect"), {}, {}, [host] { openTerminal({QStringLiteral("-e"), QStringLiteral("ssh"), host}); }};
        item.actions << Action{tr("Copy Command"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                               [l, host] { l->copyText(QStringLiteral("ssh ") + host); }};
        items << item;
    }
    Q_EMIT resultsReady(items);
}
