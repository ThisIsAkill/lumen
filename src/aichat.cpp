#include "aichat.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace
{

constexpr int MaxChats = 30;
constexpr int WarmUpSeconds = 120; // ask Ollama to load the model at most this often
// Short, direct answers suit a launcher; Markdown shows in the pane.
const char *SystemPrompt =
    "You are a helpful assistant inside a desktop launcher on Linux (KDE Plasma). "
    "Answer briefly and directly: the answer first, then only the detail that helps. "
    "Use Markdown; put code and commands in fenced code blocks.";

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("AI"));
}

QString chatsFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/lumen/ai-chats.json");
}

QIcon aiIcon()
{
    return QIcon::fromTheme(QStringLiteral("im-user"), QIcon::fromTheme(QStringLiteral("dialog-messages"), QIcon::fromTheme(QStringLiteral("help-about"))));
}

} // namespace

// --------------------------------------------------------------- client

AiChat::AiChat(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    load();
    newChat();
}

bool AiChat::busy() const
{
    return !m_reply.isNull();
}

QString AiChat::server() const
{
    return config().readEntry("Server", QStringLiteral("http://localhost:11434"));
}

QString AiChat::model() const
{
    const QString configured = config().readEntry("Model");
    if (!configured.isEmpty())
        return configured;
    // The first installed model that can chat (embedding models cannot).
    for (const QString &m : m_models)
        if (!m.contains(QLatin1String("embed")))
            return m;
    return {};
}

void AiChat::loadModels()
{
    QNetworkRequest request(QUrl(server() + QStringLiteral("/api/tags")));
    request.setTransferTimeout(3000);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_models.clear();
        for (const QJsonValue &m : QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("models")).toArray())
            m_models << m.toObject().value(QStringLiteral("name")).toString();
        Q_EMIT changed();
    });
}

void AiChat::warmUp()
{
    if (m_models.isEmpty()) {
        loadModels();
        return;
    }
    if (m_warmed.isValid() && m_warmed.secsTo(QDateTime::currentDateTime()) < WarmUpSeconds)
        return;
    m_warmed = QDateTime::currentDateTime();
    // A request with no prompt only loads the model (and keeps it a while).
    QNetworkRequest request(QUrl(server() + QStringLiteral("/api/generate")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_network->post(request, QJsonDocument(QJsonObject{{QStringLiteral("model"), model()},
                                                                               {QStringLiteral("keep_alive"), QStringLiteral("15m")}})
                                                        .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

QString AiChat::visible(const QString &answer)
{
    static const QRegularExpression think(QStringLiteral(R"(<think>.*?(</think>|$))"), QRegularExpression::DotMatchesEverythingOption);
    QString out = answer;
    out.remove(think);
    return out.trimmed();
}

QString AiChat::plainLine(const QString &markdown)
{
    // For a one-line row: no fences, emphasis, code ticks, headings or quotes.
    static const QRegularExpression marks(QStringLiteral(R"(```\w*|\*\*|__|`|^#+\s*|^>\s*)"), QRegularExpression::MultilineOption);
    QString out = markdown;
    out.remove(marks);
    return out.simplified();
}

QString AiChat::firstCode(const QString &markdown)
{
    static const QRegularExpression fence(QStringLiteral(R"(```[^\n]*\n(.*?)```)"), QRegularExpression::DotMatchesEverythingOption);
    const auto m = fence.match(markdown);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

void AiChat::newChat()
{
    stop();
    m_current = Chat{QUuid::createUuid().toString(QUuid::WithoutBraces), {}, QDateTime::currentDateTime(), {}};
    m_error.clear();
    Q_EMIT changed();
}

void AiChat::open(const QString &id)
{
    stop();
    for (const Chat &c : std::as_const(m_history))
        if (c.id == id)
            m_current = c;
    m_error.clear();
    Q_EMIT changed();
}

void AiChat::remove(const QString &id)
{
    m_history.removeIf([&](const Chat &c) { return c.id == id; });
    save();
    Q_EMIT changed();
}

void AiChat::ask(const QString &question, bool fresh)
{
    if (question.trimmed().isEmpty())
        return;
    if (fresh)
        newChat();
    stop();
    if (m_current.title.isEmpty())
        m_current.title = question.simplified().left(70);
    m_current.time = QDateTime::currentDateTime();
    m_current.messages << Message{QStringLiteral("user"), question.trimmed()} << Message{QStringLiteral("assistant"), {}};
    if (m_models.isEmpty() && config().readEntry("Model").isEmpty()) {
        // Which model? Ask Ollama first, then send.
        QNetworkRequest request(QUrl(server() + QStringLiteral("/api/tags")));
        request.setTransferTimeout(3000);
        QNetworkReply *reply = m_network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            for (const QJsonValue &m : QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("models")).toArray())
                m_models << m.toObject().value(QStringLiteral("name")).toString();
            send();
        });
    } else {
        send();
    }
    Q_EMIT changed();
}

void AiChat::send()
{
    m_error.clear();
    if (model().isEmpty()) {
        m_error = tr("No model found. Is Ollama running? (systemctl start ollama; ollama pull qwen3)");
        m_current.messages.removeLast();
        Q_EMIT changed();
        return;
    }
    QJsonArray messages{QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), QString::fromLatin1(SystemPrompt)}}};
    for (int i = 0; i < m_current.messages.size() - 1; ++i) // the last is the answer being written
        messages << QJsonObject{{QStringLiteral("role"), m_current.messages.at(i).role}, {QStringLiteral("content"), m_current.messages.at(i).content}};
    QNetworkRequest request(QUrl(server() + QStringLiteral("/api/chat")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Thinking models answer faster for quick questions without the notes.
    const QJsonObject body{{QStringLiteral("model"), model()}, {QStringLiteral("messages"), messages}, {QStringLiteral("stream"), true},
                           {QStringLiteral("think"), false}, {QStringLiteral("keep_alive"), QStringLiteral("15m")}};
    m_partial.clear();
    m_reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    // One JSON object per line, each with the next piece of the answer.
    connect(m_reply, &QNetworkReply::readyRead, this, [this, reply = m_reply.data()] {
        m_partial += reply->readAll();
        qsizetype end;
        bool grew = false;
        while ((end = m_partial.indexOf('\n')) >= 0) {
            const QJsonObject piece = QJsonDocument::fromJson(m_partial.left(end)).object();
            m_partial.remove(0, end + 1);
            if (piece.contains(QStringLiteral("error")))
                m_error = piece.value(QStringLiteral("error")).toString();
            const QString text = piece.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
            if (!text.isEmpty() && !m_current.messages.isEmpty()) {
                m_current.messages.last().content += text;
                grew = true;
            }
        }
        if (grew || !m_error.isEmpty())
            Q_EMIT changed();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply.data()] {
        reply->deleteLater();
        m_reply = nullptr; // done: busy() is false from here on
        if (reply->error() != QNetworkReply::NoError && reply->error() != QNetworkReply::OperationCanceledError && m_error.isEmpty())
            m_error = reply->error() == QNetworkReply::ConnectionRefusedError
                ? tr("Ollama is not running at %1 (systemctl start ollama)").arg(server())
                : reply->errorString();
        if (!m_current.messages.isEmpty() && m_current.messages.last().content.isEmpty())
            m_current.messages.removeLast(); // nothing came: no empty answer
        // Keep the chat (newest first) once it has an answer.
        m_history.removeIf([&](const Chat &c) { return c.id == m_current.id; });
        if (!m_current.messages.isEmpty())
            m_history.prepend(m_current);
        if (m_history.size() > MaxChats)
            m_history.resize(MaxChats);
        save();
        Q_EMIT changed();
    });
}

void AiChat::stop()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void AiChat::save()
{
    QJsonArray chats;
    for (const Chat &c : std::as_const(m_history)) {
        QJsonArray messages;
        for (const Message &m : c.messages)
            messages << QJsonObject{{QStringLiteral("role"), m.role}, {QStringLiteral("content"), m.content}};
        chats << QJsonObject{{QStringLiteral("id"), c.id}, {QStringLiteral("title"), c.title}, {QStringLiteral("time"), c.time.toSecsSinceEpoch()},
                             {QStringLiteral("messages"), messages}};
    }
    QDir().mkpath(QFileInfo(chatsFile()).path());
    QSaveFile f(chatsFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); // they may be private
        f.write(QJsonDocument(chats).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void AiChat::load()
{
    QFile f(chatsFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        const QJsonObject o = v.toObject();
        Chat c{o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("title")).toString(),
               QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("time")).toInteger()), {}};
        for (const QJsonValue &m : o.value(QStringLiteral("messages")).toArray())
            c.messages << Message{m.toObject().value(QStringLiteral("role")).toString(), m.toObject().value(QStringLiteral("content")).toString()};
        m_history << c;
    }
}

// ----------------------------------------------------------------- view

AiChatSource::AiChatSource(Launcher *launcher, AiChat *chat)
    : Source(launcher)
    , m_chat(chat)
{
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(90);
    connect(&m_refresh, &QTimer::timeout, this, &AiChatSource::publish);
    connect(chat, &AiChat::changed, this, [this] {
        if (this->launcher()->currentSource() == this && !m_refresh.isActive())
            m_refresh.start();
    });
}

void AiChatSource::query(const QString &text)
{
    m_query = text;
    publish();
}

void AiChatSource::publish()
{
    QList<Item> items;
    Launcher *l = launcher();
    AiChat *chat = m_chat;
    const QString typed = m_query.trimmed();
    if (!typed.isEmpty()) {
        Item send;
        send.id = QStringLiteral("ai:send");
        send.title = tr("Send “%1”").arg(typed.simplified().left(90));
        send.subtitle = chat->current().messages.isEmpty() ? tr("New chat · %1").arg(chat->model()) : tr("Follow-up · %1").arg(chat->model());
        send.icon = QIcon::fromTheme(QStringLiteral("document-send"));
        send.previewText = typed;
        send.score = 2000;
        send.actions << Action{tr("Send"), {}, {}, [l, chat, typed] {
            chat->ask(typed, false);
            l->setQuery({}); // the field is free for the next message
        }, true};
        send.actions << Action{tr("Send as New Chat"), QStringLiteral("document-new"), QKeySequence(Qt::CTRL | Qt::Key_Return), [l, chat, typed] {
            chat->ask(typed, true);
            l->setQuery({});
        }, true};
        items << send;
    }
    if (!chat->error().isEmpty()) {
        Item error;
        error.id = QStringLiteral("ai:error");
        error.title = chat->error();
        error.icon = QIcon::fromTheme(QStringLiteral("dialog-error"));
        error.score = 1999;
        items << error;
    }
    // The conversation, newest first: the latest answer is selected.
    const QList<AiChat::Message> messages = chat->current().messages;
    for (int i = int(messages.size()) - 1; i >= 0; --i) {
        const AiChat::Message &m = messages.at(i);
        const bool answer = m.role == QLatin1String("assistant");
        const bool writing = answer && i == messages.size() - 1 && chat->busy();
        const QString text = answer ? AiChat::visible(m.content) : m.content;
        Item item;
        item.id = QStringLiteral("ai:message:%1").arg(i);
        item.title = text.isEmpty() ? (writing ? tr("Thinking…") : QString()) : AiChat::plainLine(text).left(140);
        item.subtitle = answer ? (writing ? tr("Writing… · %1").arg(chat->model()) : chat->model()) : tr("You");
        item.kind = answer ? tr("Answer") : tr("Question");
        item.icon = answer ? aiIcon() : QIcon::fromTheme(QStringLiteral("user-identity"));
        item.previewText = text.isEmpty() ? tr("*Thinking…*") : text;
        item.markdown = true;
        item.score = 1000 - (messages.size() - 1 - i);
        if (writing)
            item.actions << Action{tr("Stop"), QStringLiteral("process-stop"), {}, [chat] { chat->stop(); }, true};
        if (!text.isEmpty()) {
            if (l->canPaste())
                item.actions << Action{answer ? tr("Paste Answer") : tr("Paste"), {}, {}, [l, text] { l->pasteText(text); }};
            item.actions << Action{answer ? tr("Copy Answer") : tr("Copy"), QStringLiteral("edit-copy"),
                                   l->canPaste() ? QKeySequence(Qt::CTRL | Qt::Key_Return) : QKeySequence(), [l, text] { l->copyText(text); }};
            if (const QString code = AiChat::firstCode(text); answer && !code.isEmpty())
                item.actions << Action{tr("Copy Code"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                                       [l, code] { l->copyText(code); }};
        }
        item.actions << Action{tr("New Chat"), QStringLiteral("document-new"), QKeySequence(Qt::CTRL | Qt::Key_N), [chat] { chat->newChat(); }, true};
        items << item;
    }
    if (items.isEmpty()) {
        Item hint;
        hint.id = QStringLiteral("ai:hint");
        hint.title = tr("Ask anything");
        hint.subtitle = chat->model().isEmpty() ? tr("Answers come from Ollama on this computer") : tr("Answers come from %1, on this computer").arg(chat->model());
        hint.icon = aiIcon();
        items << hint;
    }
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    Q_EMIT resultsReady(items);
}

// ----------------------------------------------------------------- root

AiRootSource::AiRootSource(Launcher *launcher, AiChat *chat)
    : Source(launcher)
    , m_chat(chat)
{
}

QList<Item> AiRootSource::fallbackItems(const QString &text) const
{
    // "ask …" has its own result already.
    static const QRegularExpression asking(QStringLiteral(R"(^(?:ask|ai|\?)\s)"), QRegularExpression::CaseInsensitiveOption);
    if (asking.match(text).hasMatch())
        return {};
    Item item;
    item.id = QStringLiteral("lumen:ask-ai");
    item.title = tr("Ask AI “%1”").arg(text);
    item.icon = aiIcon();
    Launcher *l = launcher();
    AiChat *chat = m_chat;
    item.actions << Action{tr("Ask"), {}, {}, [l, chat, text] {
        chat->ask(text, true);
        l->openView(Launcher::AiChatView);
    }, true};
    return {item};
}

void AiRootSource::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    Launcher *l = launcher();
    AiChat *chat = m_chat;
    // "ask …", "? …", "ai …": a question for the model.
    static const QRegularExpression askPattern(QStringLiteral(R"(^(?:ask|ai|\?)\s+(.+)$)"), QRegularExpression::CaseInsensitiveOption);
    if (const auto m = askPattern.match(q); m.hasMatch()) {
        chat->warmUp(); // the model loads while the question is typed
        const QString question = m.captured(1);
        Item item;
        item.id = QStringLiteral("lumen:ask");
        item.title = tr("Ask AI: %1").arg(question);
        item.subtitle = chat->model().isEmpty() ? tr("Ollama, on this computer") : chat->model();
        item.kind = tr("AI");
        item.icon = aiIcon();
        item.score = 125;
        item.actions << Action{tr("Ask"), {}, {}, [l, chat, question] {
            chat->ask(question, true);
            l->openView(Launcher::AiChatView);
        }, true};
        items << item;
    } else if (q.compare(QLatin1String("ask"), Qt::CaseInsensitive) == 0 || q == QLatin1String("?")) {
        chat->warmUp();
    }
    if (!q.isEmpty()) {
        const qreal chatScore = std::max({fuzzyScore(q, QStringLiteral("AI Chat")), fuzzyScore(q, QStringLiteral("chat")), fuzzyScore(q, QStringLiteral("ollama"))});
        if (chatScore >= 60) {
            Item item;
            item.id = QStringLiteral("lumen:ai-chat");
            item.title = chat->current().messages.isEmpty() ? tr("AI Chat") : tr("AI Chat · %1").arg(chat->current().title);
            item.subtitle = tr("Ask a local model; Enter opens the chat");
            item.kind = tr("AI");
            item.icon = aiIcon();
            item.score = chatScore;
            item.actions << Action{tr("Open"), {}, {}, [l, chat] {
                chat->warmUp();
                l->openView(Launcher::AiChatView);
            }, true};
            item.actions << Action{tr("New Chat"), QStringLiteral("document-new"), QKeySequence(Qt::CTRL | Qt::Key_N), [l, chat] {
                chat->newChat();
                l->openView(Launcher::AiChatView);
            }, true};
            items << item;
            // Earlier chats.
            int n = 0;
            for (const AiChat::Chat &c : chat->history()) {
                if (n++ == 8)
                    break;
                Item old;
                old.id = QStringLiteral("ai-chat:") + c.id;
                old.title = c.title;
                old.subtitle = QLocale().toString(c.time, QLocale::ShortFormat);
                old.kind = tr("AI Chat");
                old.icon = aiIcon();
                old.score = chatScore - 1 - n * 0.01;
                const QString id = c.id;
                old.actions << Action{tr("Continue Chat"), {}, {}, [l, chat, id] {
                    chat->open(id);
                    l->openView(Launcher::AiChatView);
                }, true};
                old.actions << Action{tr("Delete Chat"), QStringLiteral("edit-delete"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), [l, chat, id, text] {
                    chat->remove(id);
                    l->setQuery(text);
                }, true};
                items << old;
            }
        } else {
            // Earlier chats by their title.
            for (const AiChat::Chat &c : chat->history()) {
                const qreal score = fuzzyScore(q, c.title) * 0.7;
                if (score < 45 || q.size() < 3)
                    continue;
                Item old;
                old.id = QStringLiteral("ai-chat:") + c.id;
                old.title = c.title;
                old.subtitle = tr("AI chat · %1").arg(QLocale().toString(c.time, QLocale::ShortFormat));
                old.kind = tr("AI Chat");
                old.icon = aiIcon();
                old.score = score;
                const QString id = c.id;
                old.actions << Action{tr("Continue Chat"), {}, {}, [l, chat, id] {
                    chat->open(id);
                    l->openView(Launcher::AiChatView);
                }, true};
                items << old;
            }
        }
        // The copied text, worked on.
        struct Helper {
            const char *id;
            QString title;
            QString subtitle;
            QString prompt;
        };
        const QList<Helper> helpers{
            {"ai-summarize", tr("Summarize Clipboard"), tr("A short summary of the copied text"), QStringLiteral("Summarize this in a few bullet points:\n\n%1")},
            {"ai-explain", tr("Explain Clipboard"), tr("What the copied text, code or error means"), QStringLiteral("Explain this simply. If it is an error, say how to fix it:\n\n%1")},
            {"ai-fix", tr("Fix Spelling and Grammar"), tr("The copied text, corrected"),
             QStringLiteral("Correct the spelling and grammar. Reply with the corrected text only, nothing else:\n\n%1")},
        };
        for (const Helper &h : helpers) {
            const qreal score = fuzzyScore(q, h.title);
            if (score < 60)
                continue;
            Item item;
            item.id = QStringLiteral("lumen:") + QLatin1String(h.id);
            item.title = h.title;
            item.subtitle = h.subtitle;
            item.kind = tr("AI");
            item.icon = aiIcon();
            item.score = score;
            const QString prompt = h.prompt;
            item.actions << Action{tr("Run"), {}, {}, [l, chat, prompt] {
                const QString copied = l->recentClipboardText(0);
                if (copied.trimmed().isEmpty()) {
                    l->notify(tr("Copy some text first"));
                    return;
                }
                chat->warmUp();
                chat->ask(prompt.arg(copied), true);
                l->openView(Launcher::AiChatView);
            }, true};
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}
