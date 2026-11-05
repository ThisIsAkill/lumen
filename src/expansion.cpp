#include "expansion.h"
#include "launcher.h"
#include "library.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>

namespace
{

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Expansion"));
}

bool enabled()
{
    return config().readEntry("Enabled", true);
}

} // namespace

Expansion::Expansion(Launcher *launcher)
    : QObject(launcher)
    , m_launcher(launcher)
{
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        sendKeywords();
        Q_EMIT connectionChanged(true);
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        Q_EMIT connectionChanged(false);
        m_retry.start();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] { m_retry.start(); });
    connect(&m_socket, &QLocalSocket::readyRead, this, &Expansion::readMatches);
    connect(launcher->library(), &Library::changed, this, &Expansion::sendKeywords);

    // The helper may be installed or restarted at any time.
    m_retry.setSingleShot(true);
    m_retry.setInterval(5000);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (enabled() && m_socket.state() == QLocalSocket::UnconnectedState)
            m_socket.connectToServer(socketPath());
    });
    if (enabled())
        m_socket.connectToServer(socketPath());
}

QString Expansion::socketPath()
{
    const QString overridden = qEnvironmentVariable("LUMEN_EXPAND_SOCKET"); // for tests
    return overridden.isEmpty() ? QStringLiteral("/run/lumen-expand/socket") : overridden;
}

QString Expansion::trigger()
{
    return config().readEntry("Trigger", QStringLiteral(";"));
}

void Expansion::sendKeywords()
{
    if (!enabled()) {
        m_socket.disconnectFromServer();
        return;
    }
    if (!isConnected()) {
        m_retry.start();
        return;
    }
    QStringList keywords;
    for (const Library::Snippet &s : m_launcher->library()->snippets()) {
        const QString keyword = s.keyword.simplified().remove(u' ');
        if (!keyword.isEmpty())
            keywords << trigger() + keyword;
    }
    keywords.prepend(QStringLiteral("keywords"));
    m_socket.write(keywords.join(u'\t').toUtf8() + '\n');
}

void Expansion::readMatches()
{
    m_incoming += m_socket.readAll();
    qsizetype end;
    while ((end = m_incoming.indexOf('\n')) >= 0) {
        const QStringList parts = QString::fromUtf8(m_incoming.left(end)).split(u'\t');
        m_incoming.remove(0, end + 1);
        if (parts.size() == 2 && parts.first() == QLatin1String("match"))
            expand(parts.at(1));
    }
}

bool Expansion::screenLocked() const
{
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"),
                                              QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("GetActive"));
    const QDBusReply<bool> reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 300);
    return reply.isValid() && reply.value();
}

void Expansion::expand(const QString &typed)
{
    // Typing on the lock screen or into Lumen itself is not expanded.
    if (screenLocked() || m_launcher->isVisible())
        return;
    for (const Library::Snippet &s : m_launcher->library()->snippets()) {
        if (trigger() + s.keyword.simplified().remove(u' ') == typed) {
            m_launcher->useSnippet(s.name, s.text, Launcher::SnippetUse::ReplaceTyped, int(typed.toUcs4().size()));
            return;
        }
    }
}
