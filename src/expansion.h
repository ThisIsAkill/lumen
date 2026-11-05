#pragma once

#include <QLocalSocket>
#include <QObject>
#include <QTimer>

class Launcher;

// Expands snippet keywords typed in any app (";addr" → the address). The
// typing itself is watched by lumen-expand-helper, a small system service
// (see helper/), which reports matches over a socket; this client tells it
// the keywords, and on a match deletes the keyword and pastes the snippet.
class Expansion : public QObject
{
    Q_OBJECT
public:
    explicit Expansion(Launcher *launcher);

    bool isConnected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    static QString socketPath();
    // The character that starts a keyword (";" by default; settings).
    static QString trigger();

    // Sends the current keywords (after snippets or settings change).
    void sendKeywords();

Q_SIGNALS:
    void connectionChanged(bool connected);

private:
    void readMatches();
    void expand(const QString &keyword);
    bool screenLocked() const;

    Launcher *m_launcher;
    QLocalSocket m_socket;
    QTimer m_retry;
    QByteArray m_incoming;
};
