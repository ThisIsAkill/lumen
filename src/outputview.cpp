#include "outputview.h"
#include "fuzzy.h"
#include "launcher.h"

#include <QRegularExpression>

namespace
{
constexpr int MaxRows = 500;
}

OutputView::OutputView(Launcher *launcher, const QString &title, const QString &icon)
    : Source(launcher)
    , m_title(title)
    , m_icon(icon)
{
}

OutputView::~OutputView()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
    }
}

QString OutputView::plain(const QString &output)
{
    static const QRegularExpression escapes(QStringLiteral("\x1B\\[[0-9;?]*[ -/]*[@-~]|\x1B\\][^\x07]*\x07"));
    QString out = output;
    out.remove(escapes);
    out.remove(u'\r');
    return out;
}

void OutputView::setText(const QString &text, bool markdown)
{
    m_text = markdown ? text : plain(text);
    m_markdown = markdown;
    query(m_query);
}

void OutputView::follow(QProcess *process)
{
    m_process = process;
    m_running = true;
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyRead, this, [this, process] {
        m_text += plain(QString::fromUtf8(process->readAll()));
        query(m_query);
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        m_text += plain(QString::fromUtf8(process->readAll()));
        m_running = false;
        if (status != QProcess::NormalExit || code != 0)
            launcher()->notify(tr("%1 exited with code %2").arg(m_title).arg(code));
        query(m_query);
    });
}

void OutputView::query(const QString &text)
{
    m_query = text;
    Launcher *l = launcher();
    const QString all = m_text.trimmed();
    auto row = [&](const QString &title) {
        Item item;
        item.title = title;
        item.icon = QIcon::fromTheme(m_icon, QIcon::fromTheme(QStringLiteral("utilities-terminal")));
        item.previewText = all;
        item.markdown = m_markdown;
        item.monospace = !m_markdown;
        item.actions << Action{tr("Copy Output"), {}, {}, [l, all] { l->copyText(all); }};
        return item;
    };
    QList<Item> items;
    if (m_markdown) {
        Item item = row(m_title);
        item.id = QStringLiteral("output:") + m_title;
        items << item;
    } else {
        const QStringList lines = all.split(u'\n');
        for (int i = 0; i < lines.size() && items.size() < MaxRows; ++i) {
            const QString line = lines.at(i);
            if (line.trimmed().isEmpty() || (!text.trimmed().isEmpty() && fuzzyScore(text.trimmed(), line) <= 0))
                continue;
            Item item = row(line);
            item.id = QStringLiteral("output-line:%1").arg(i);
            item.actions << Action{tr("Copy Line"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                                   [l, line] { l->copyText(line); }};
            items << item;
        }
    }
    if (m_running) {
        Item running;
        running.id = QStringLiteral("output:running");
        running.title = tr("Running…");
        running.subtitle = all.isEmpty() ? tr("No output yet") : QString();
        running.icon = QIcon::fromTheme(QStringLiteral("process-working-symbolic"), QIcon::fromTheme(QStringLiteral("chronometer")));
        running.previewText = all;
        running.monospace = true;
        auto process = m_process;
        running.actions << Action{tr("Stop"), {}, {}, [process] {
            if (process)
                process->kill();
        }, true};
        items << running;
    }
    Q_EMIT resultsReady(items);
}
