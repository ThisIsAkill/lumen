#include "pastestack.h"
#include "plural.h"
#include "launcher.h"

#include <KGlobalAccel>
#include <KGlobalShortcutInfo>

#include <QAction>
#include <QFile>

PasteStack::PasteStack(Launcher *launcher, ClipboardHistory *history)
    : QObject(launcher)
    , m_launcher(launcher)
    , m_history(history)
    , m_next(new QAction(tr("Paste Next from Paste Stack"), this))
{
    m_next->setObjectName(QStringLiteral("paste-stack-next"));
    connect(m_next, &QAction::triggered, this, &PasteStack::pasteNext);
    // A key left registered by an earlier run that ended mid-stack.
    KGlobalAccel::self()->removeAllShortcuts(m_next);
    connect(history, &ClipboardHistory::added, this, [this](const ClipboardHistory::Entry &entry) {
        if (m_active && !isOurOwn(entry))
            add(entry);
    });
}

PasteStack::~PasteStack()
{
    if (m_active)
        KGlobalAccel::self()->removeAllShortcuts(m_next);
}

bool PasteStack::isOurOwn(const ClipboardHistory::Entry &entry) const
{
    return entry.text == m_pasted.text && entry.imageFile == m_pasted.imageFile;
}

void PasteStack::start()
{
    if (m_active)
        return;
    m_active = true;
    m_items.clear();
    m_pasted = {};
    // The key is only taken while the stack exists, unless something else has it.
    QString takenBy;
    for (const KGlobalShortcutInfo &info : KGlobalAccel::globalShortcutsByKey(nextKey()))
        if (info.uniqueName() != m_next->objectName())
            takenBy = info.friendlyName().isEmpty() ? info.uniqueName() : info.friendlyName();
    if (takenBy.isEmpty()) {
        KGlobalAccel::self()->setShortcut(m_next, {nextKey()}, KGlobalAccel::NoAutoloading);
        m_launcher->notify(tr("Paste stack on: copy things, then paste them in order with %1").arg(nextKey().toString(QKeySequence::NativeText)));
    } else {
        m_launcher->notify(tr("Paste stack on (%1 is used by “%2”: paste from the stack view)").arg(nextKey().toString(QKeySequence::NativeText), takenBy));
    }
    Q_EMIT changed();
}

void PasteStack::stop()
{
    if (!m_active)
        return;
    m_active = false;
    m_items.clear();
    KGlobalAccel::self()->removeAllShortcuts(m_next);
    Q_EMIT changed();
}

void PasteStack::add(const ClipboardHistory::Entry &entry)
{
    m_items << entry;
    if (!m_launcher->isVisible())
        m_launcher->notify(plural(m_items.size(), tr("Paste stack: %1 item"), tr("Paste stack: %1 items")));
    Q_EMIT changed();
}

void PasteStack::remove(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    m_items.removeAt(index);
    Q_EMIT changed();
}

void PasteStack::paste(int index, bool now)
{
    if (index < 0 || index >= m_items.size())
        return;
    const ClipboardHistory::Entry entry = m_items.takeAt(index);
    m_pasted = entry;
    if (entry.imageFile.isEmpty())
        now ? m_launcher->pasteNow(entry.text) : m_launcher->pasteText(entry.text);
    else if (const QString path = m_history->imagePath(entry); QFile::exists(path))
        now ? m_launcher->pasteImageNow(path) : m_launcher->pasteImage(path);
    if (m_items.isEmpty()) {
        stop();
        m_launcher->notify(tr("Paste stack done"));
    } else {
        if (now)
            m_launcher->notify(tr("%1 left in the paste stack").arg(m_items.size()));
        Q_EMIT changed();
    }
}

void PasteStack::pasteNext()
{
    if (m_items.isEmpty()) {
        m_launcher->notify(tr("The paste stack is empty"));
        return;
    }
    paste(0, !m_launcher->isVisible());
}

void PasteStack::pasteAll()
{
    QStringList texts;
    for (const ClipboardHistory::Entry &e : std::as_const(m_items))
        if (!e.text.isEmpty())
            texts << e.text;
    if (texts.isEmpty())
        return;
    m_pasted = {texts.join(u'\n'), {}, {}, {}, false, {}, false};
    m_launcher->pasteText(m_pasted.text);
    stop();
}

// ---------------------------------------------------------------- view

PasteStackSource::PasteStackSource(Launcher *launcher, PasteStack *stack)
    : Source(launcher)
    , m_stack(stack)
{
    connect(stack, &PasteStack::changed, this, [this] {
        if (this->launcher()->currentSource() == this)
            query(m_query);
    });
}

void PasteStackSource::query(const QString &text)
{
    m_query = text;
    Launcher *l = launcher();
    PasteStack *stack = m_stack;
    QList<Item> items;
    const QList<ClipboardHistory::Entry> entries = stack->items();
    for (int i = 0; i < entries.size(); ++i) {
        const ClipboardHistory::Entry &e = entries.at(i);
        const QString shown = e.imageFile.isEmpty() ? e.text : e.imageText;
        if (!text.trimmed().isEmpty() && !shown.contains(text.trimmed(), Qt::CaseInsensitive))
            continue;
        Item item;
        item.id = QStringLiteral("stack:%1").arg(i);
        item.title = e.imageFile.isEmpty() ? e.text.simplified().left(200) : tr("Image");
        item.subtitle = i == 0 ? tr("Next · %1").arg(PasteStack::nextKey().toString(QKeySequence::NativeText)) : tr("#%1").arg(i + 1);
        item.kind = e.imageFile.isEmpty() ? QString() : tr("%1 × %2").arg(e.imageSize.width()).arg(e.imageSize.height());
        item.score = 1000 - i;
        if (e.imageFile.isEmpty()) {
            item.icon = QIcon::fromTheme(QStringLiteral("text-plain"));
            item.previewText = e.text;
        } else {
            item.filePath = l->clipboardHistory()->imagePath(e);
            item.icon = QIcon(item.filePath);
        }
        item.actions << Action{tr("Paste"), {}, {}, [stack, i] { stack->paste(i, false); }};
        item.actions << Action{tr("Remove"), QStringLiteral("edit-delete"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), [stack, i] { stack->remove(i); }, true};
        item.actions << Action{tr("Paste All as Lines"), QStringLiteral("edit-paste"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return),
                               [stack] { stack->pasteAll(); }};
        item.actions << Action{tr("End Paste Stack"), QStringLiteral("process-stop"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backspace),
                               [stack] { stack->stop(); }, true};
        items << item;
    }
    if (!stack->isActive()) {
        Item start;
        start.id = QStringLiteral("lumen:paste-stack-start");
        start.title = tr("Start Paste Stack");
        start.subtitle = tr("Collect what you copy, then paste it in order with %1").arg(PasteStack::nextKey().toString(QKeySequence::NativeText));
        start.icon = QIcon::fromTheme(QStringLiteral("view-list-text"));
        start.actions << Action{tr("Start"), {}, {}, [stack] { stack->start(); }, true};
        items << start;
    }
    Q_EMIT resultsReady(items);
}

QList<Item> PasteStackSource::startItems() const
{
    if (!m_stack->isActive())
        return {};
    Item item;
    item.id = QStringLiteral("lumen:paste-stack");
    item.title = tr("Paste Stack");
    item.subtitle = m_stack->items().isEmpty() ? tr("Copy things to add them") : plural(m_stack->items().size(), tr("%1 item waiting"), tr("%1 items waiting"));
    item.kind = PasteStack::nextKey().toString(QKeySequence::NativeText);
    item.keyKind = true;
    item.icon = QIcon::fromTheme(QStringLiteral("view-list-text"));
    item.score = 1000;
    Launcher *l = launcher();
    PasteStack *stack = m_stack;
    item.actions << Action{tr("Open"), {}, {}, [l] { l->openView(Launcher::PasteStackView); }, true};
    item.actions << Action{tr("End Paste Stack"), QStringLiteral("process-stop"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backspace),
                           [stack] { stack->stop(); }, true};
    return {item};
}
