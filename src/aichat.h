#pragma once

#include "source.h"

#include <QDateTime>
#include <QPointer>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

// Chat with a local model through Ollama (lumenrc [AI] Server, Model; the
// first installed model by default). Answers stream in as they are
// written. Chats are kept in ~/.local/share/lumen/ai-chats.json.
class AiChat : public QObject
{
    Q_OBJECT
public:
    struct Message {
        QString role; // "user" or "assistant"
        QString content;
    };
    struct Chat {
        QString id;
        QString title;
        QDateTime time;
        QList<Message> messages;
    };

    explicit AiChat(QObject *parent = nullptr);

    QString server() const;
    QString model() const; // the configured one, or the first installed
    QStringList models() const { return m_models; }
    void loadModels(); // asks Ollama which models are installed

    // Starts a new chat with a question, or adds one to the current chat.
    void ask(const QString &question, bool newChat);
    void stop();
    void newChat();
    bool busy() const;
    QString error() const { return m_error; }
    const Chat &current() const { return m_current; }
    QList<Chat> history() const { return m_history; }
    void open(const QString &id); // an earlier chat becomes the current one
    void remove(const QString &id);
    // Starts loading the model while the question is still being typed.
    void warmUp();

    // Without the model's <think>…</think> notes.
    static QString visible(const QString &answer);
    // Markdown as one plain line, for a result row.
    static QString plainLine(const QString &markdown);
    // The first ```fenced``` block, without its fences.
    static QString firstCode(const QString &markdown);

Q_SIGNALS:
    void changed();

private:
    void send();
    void save();
    void load();

    QNetworkAccessManager *m_network;
    QStringList m_models;
    Chat m_current;
    QList<Chat> m_history;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_partial; // a streamed line not yet complete
    QString m_error;
    QDateTime m_warmed;
};

// The "AI Chat" view: the conversation (newest first, the latest answer
// selected and shown in full in the pane) and, while typing, "Send". The
// search field is the message box.
class AiChatSource : public Source
{
    Q_OBJECT
public:
    AiChatSource(Launcher *launcher, AiChat *chat);

    void query(const QString &text) override;
    QString viewTitle() const override { return tr("AI Chat"); }
    QString viewPlaceholder() const override { return tr("Ask a follow-up…"); }
    bool hasPreviewPane() const override { return true; }

private:
    void publish();

    AiChat *m_chat;
    QString m_query;
    QTimer m_refresh; // streaming: repaint at most this often
};

// In the main search: "ask …" / "? …" (a quick answer in the chat view),
// "AI Chat", earlier chats by title, and clipboard helpers (Summarize,
// Explain, Fix Spelling and Grammar). "Ask AI" is also a fallback command.
class AiRootSource : public Source
{
    Q_OBJECT
public:
    AiRootSource(Launcher *launcher, AiChat *chat);
    void query(const QString &text) override;
    QList<Item> fallbackItems(const QString &text) const override;

private:
    AiChat *m_chat;
};
