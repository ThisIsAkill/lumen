#pragma once

#include "source.h"

#include <QDateTime>

// Code projects and SSH hosts in the main search.
//  - Projects: git repositories found (two levels deep) in the usual code
//    folders (~/Documents/Coding, ~/Projects, ~/src, ~/code, ~/dev, …, or
//    the folders set in lumenrc [Projects] Roots). Enter opens one in the
//    editor (VS Code, VSCodium, Zed or Kate, or [Projects] Editor); actions
//    open it in Konsole or Dolphin. "projects" lists them all.
//  - SSH hosts from ~/.ssh/config and readable ~/.ssh/known_hosts entries;
//    Enter connects in Konsole. "ssh" lists them all.
class ProjectSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;

    void query(const QString &text) override;
    void sessionStarted() override;

    // Exposed for tests.
    static QStringList sshHosts(const QString &configFile, const QString &knownHostsFile);
    static QString gitBranch(const QString &repo);

private:
    struct Project {
        QString name;
        QString path;
    };
    void scan();
    static QStringList roots();
    static QStringList editorCommand();

    QList<Project> m_projects;
    QStringList m_hosts;
    QDateTime m_scanned;
};
