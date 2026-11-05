#include "devsource.h"
#include "clipboardhistory.h"
#include "fuzzy.h"
#include "launcher.h"
#include "transforms.h"

namespace
{

// The newest text on the clipboard history (what Ctrl+V would paste).
QString clipboardText(Launcher *launcher)
{
    for (const ClipboardHistory::Entry &e : launcher->clipboardHistory()->entries())
        if (!e.text.isEmpty())
            return e.text;
    return {};
}

QString preview(const QString &text)
{
    const QString line = text.simplified();
    return line.size() > 90 ? line.left(90) + QStringLiteral("…") : line;
}

// Paste (when possible) and copy actions for a result value.
QList<Action> deliver(Launcher *l, const QString &value)
{
    QList<Action> actions;
    if (l->canPaste())
        actions << Action{QObject::tr("Paste"), {}, {}, [l, value] { l->pasteText(value); }};
    actions << Action{QObject::tr("Copy to Clipboard"), QStringLiteral("edit-copy"),
                      l->canPaste() ? QKeySequence(Qt::CTRL | Qt::Key_Return) : QKeySequence(), [l, value] { l->copyText(value); }};
    return actions;
}

Item transformItem(Launcher *l, const Transforms::Transform &t, const QString &result, bool fromClipboard)
{
    Item item;
    item.id = QStringLiteral("transform:") + t.id;
    item.title = fromClipboard ? t.name : preview(result);
    item.subtitle = fromClipboard ? preview(result) : t.name;
    item.kind = fromClipboard ? QObject::tr("Clipboard → %1").arg(t.name) : QObject::tr("Transform");
    item.icon = QIcon::fromTheme(QStringLiteral("format-text-code"), QIcon::fromTheme(QStringLiteral("edit-select-text")));
    item.previewText = result;
    item.actions = deliver(l, result);
    return item;
}

} // namespace

void DevSource::query(const QString &text)
{
    QList<Item> items;
    const QString q = text.trimmed();
    Launcher *l = launcher();
    if (q.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }

    if (const auto g = Transforms::generate(q)) {
        Item item;
        item.id = QStringLiteral("answer:generate");
        item.title = g->title;
        item.subtitle = g->subtitle;
        item.kind = tr("Generated");
        item.icon = QIcon::fromTheme(QStringLiteral("roll"), QIcon::fromTheme(QStringLiteral("password-generate")));
        item.score = 115;
        item.previewText = g->value;
        item.actions = deliver(l, g->value);
        items << item;
    }

    // "count TEXT" / "count"
    if (q.compare(QLatin1String("count"), Qt::CaseInsensitive) == 0 || q.startsWith(QLatin1String("count "), Qt::CaseInsensitive)) {
        const QString subject = q.size() > 6 ? q.mid(6) : clipboardText(l);
        Item item;
        item.id = QStringLiteral("answer:count");
        item.title = Transforms::counts(subject);
        item.subtitle = q.size() > 6 ? tr("In the text typed") : tr("In the clipboard");
        item.kind = tr("Count");
        item.icon = QIcon::fromTheme(QStringLiteral("format-text-code"));
        item.score = 115;
        const QString value = item.title;
        item.actions << Action{tr("Copy"), {}, {}, [l, value] { l->copyText(value); }};
        items << item;
    }

    // "<keyword> TEXT" transforms TEXT; the keyword alone transforms the clipboard.
    for (const Transforms::Transform &t : Transforms::all()) {
        for (const QString &keyword : t.keywords) {
            const bool alone = q.compare(keyword, Qt::CaseInsensitive) == 0;
            const bool withText = q.startsWith(keyword + u' ', Qt::CaseInsensitive);
            if (!alone && !withText)
                continue;
            const QString input = alone ? clipboardText(l) : q.mid(keyword.size() + 1);
            if (input.isEmpty())
                break;
            if (const auto result = t.apply(input)) {
                Item item = transformItem(l, t, *result, alone);
                item.score = withText ? 113 : 105;
                items << item;
            }
            break;
        }
    }

    // The view itself, for words like "transform" and "case".
    const qreal score = std::max(fuzzyScore(q, QStringLiteral("Transform Clipboard")), fuzzyScore(q, QStringLiteral("text case")));
    if (score >= 40) {
        Item open;
        open.id = QStringLiteral("lumen:transform");
        open.title = tr("Transform Clipboard");
        open.subtitle = tr("Change case, encode, format JSON, sort lines…");
        open.kind = tr("Command");
        open.icon = QIcon::fromTheme(QStringLiteral("format-text-code"));
        open.score = score;
        open.actions << Action{tr("Open"), {}, {}, [l] { l->openView(Launcher::TransformView); }, true};
        items << open;
    }
    Q_EMIT resultsReady(items);
}

void TransformSource::query(const QString &text)
{
    Launcher *l = launcher();
    const QString input = clipboardText(l);
    QList<Item> items;
    if (input.isEmpty()) {
        Item none;
        none.title = tr("The clipboard has no text");
        none.subtitle = tr("Copy some text, then come back");
        none.icon = QIcon::fromTheme(QStringLiteral("edit-paste"));
        Q_EMIT resultsReady({none});
        return;
    }
    const QString q = text.trimmed();
    int i = 0;
    for (const Transforms::Transform &t : Transforms::all()) {
        qreal score = q.isEmpty() ? 1000 - i : fuzzyScore(q, t.name);
        for (const QString &k : t.keywords)
            score = std::max(score, fuzzyScore(q, k));
        ++i;
        const auto result = t.apply(input);
        if (score <= 0 || !result || *result == input)
            continue; // no match, does not apply, or changes nothing
        Item item = transformItem(l, t, *result, true);
        item.kind.clear();
        item.score = score;
        items << item;
    }
    Q_EMIT resultsReady(items);
}
