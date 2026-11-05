#include "answersource.h"
#include "answers.h"
#include "launcher.h"

#include <QPainter>
#include <QPainterPath>

namespace
{

QIcon swatch(const QColor &colour)
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(4, 4, 56, 56), 14, 14);
    // A checkerboard shows through transparent colours.
    p.setClipPath(path);
    for (int y = 0; y < 64; y += 8)
        for (int x = 0; x < 64; x += 8)
            p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(200, 200, 200) : Qt::white);
    p.fillPath(path, colour);
    p.setClipping(false);
    p.setPen(QPen(QColor(0, 0, 0, 60), 2));
    p.drawPath(path);
    return QIcon(pixmap);
}

} // namespace

void AnswerSource::query(const QString &text)
{
    QList<Item> items;
    Launcher *l = launcher();
    auto add = [&](const std::optional<Answers::Answer> &answer, const QString &id, const QIcon &icon) {
        if (!answer)
            return;
        Item item;
        item.id = QStringLiteral("answer:") + id;
        item.title = answer->title;
        item.subtitle = answer->subtitle;
        item.kind = tr("Answer");
        item.icon = icon;
        item.score = 115; // an exact answer to what was typed beats everything else
        const QString copy = answer->copyText;
        item.actions << Action{tr("Copy Answer"), {}, {}, [l, copy] { l->copyText(copy); }};
        int n = 1;
        for (const auto &[label, value] : answer->alternatives) {
            const QString v = value;
            item.actions << Action{label, QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key(Qt::Key_0 + n++)),
                                   [l, v] { l->copyText(v); }};
        }
        items << item;
    };
    if (const auto c = Answers::colour(text))
        add(c, QStringLiteral("colour"), swatch(c->colour));
    add(Answers::timeZone(text), QStringLiteral("time"), QIcon::fromTheme(QStringLiteral("preferences-system-time")));
    add(Answers::dates(text), QStringLiteral("date"), QIcon::fromTheme(QStringLiteral("view-calendar")));
    add(Answers::percent(text), QStringLiteral("percent"), QIcon::fromTheme(QStringLiteral("accessories-calculator")));
    Q_EMIT resultsReady(items);
}
