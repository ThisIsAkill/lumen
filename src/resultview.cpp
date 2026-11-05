#include "resultview.h"

#include <chrome.h>

#include <QFontDatabase>
#include <QGuiApplication>
#include <QMimeData>
#include <QPainter>
#include <QUrl>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

void ResultModel::setItems(const QList<Item> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
}

const Item *ResultModel::itemAt(int row) const
{
    return row >= 0 && row < m_items.size() ? &m_items.at(row) : nullptr;
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

Qt::ItemFlags ResultModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractListModel::flags(index);
    if (const Item *item = itemAt(index.row()); item && !item->filePath.isEmpty())
        f |= Qt::ItemIsDragEnabled;
    return f;
}

QStringList ResultModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData *ResultModel::mimeData(const QModelIndexList &indexes) const
{
    QList<QUrl> urls;
    for (const QModelIndex &index : indexes)
        if (const Item *item = itemAt(index.row()); item && !item->filePath.isEmpty())
            urls << QUrl::fromLocalFile(item->filePath);
    if (urls.isEmpty())
        return nullptr;
    auto *data = new QMimeData;
    data->setUrls(urls);
    return data;
}

QVariant ResultModel::data(const QModelIndex &index, int role) const
{
    const Item *item = itemAt(index.row());
    if (!item)
        return {};
    switch (role) {
    case Qt::DisplayRole:
        return item->title;
    case Qt::DecorationRole:
        return item->icon;
    case SubtitleRole:
        return item->subtitle;
    case KindRole:
        return item->kind;
    case KeyKindRole:
        return item->keyKind;
    case GroupRole:
        return int(groupOf(*item));
    case FractionRole:
        return item->fraction;
    case IdRole:
        return item->id;
    case CaptionRole: {
        // The first fallback row carries the caption for all of them.
        const Item *above = itemAt(index.row() - 1);
        if (groupOf(*item) == Fallback && (!above || groupOf(*above) != Fallback))
            return tr("Use “%1” with").arg(m_searchText);
        return {};
    }
    default:
        return {};
    }
}

ResultModel::Group ResultModel::groupOf(const Item &item)
{
    if (item.id.startsWith(QLatin1String("answer:")))
        return Answer;
    if (item.id.startsWith(QLatin1String("weather-day:")))
        return Forecast;
    if (item.id.startsWith(QLatin1String("timer:")))
        return Timer;
    if (item.id.startsWith(QLatin1String("event:")))
        return Event;
    if (item.id == QLatin1String("media:now-playing"))
        return NowPlaying;
    if (item.id.startsWith(QLatin1String("favourite:")))
        return Favourite;
    if (item.id == QLatin1String("lumen:focus"))
        return Focus;
    if (item.id == QLatin1String("lumen:paste-stack"))
        return Stack;
    if (item.id.startsWith(QLatin1String("stack:")))
        return StackEntry;
    if (item.id.startsWith(QLatin1String("output-line:")))
        return OutputLine;
    if (item.id == QLatin1String("output:running"))
        return Running;
    if (item.id.startsWith(QLatin1String("calculation:")))
        return Calculation;
    if (item.id.startsWith(QLatin1String("theme:")) || item.id.startsWith(QLatin1String("accent:")))
        return Choice;
    if (item.id.startsWith(QLatin1String("fallback:")))
        return Fallback;
    return Plain;
}

int ResultModel::sectionOf(Group group)
{
    switch (group) {
    case Answer:
    case Forecast: // the forecast belongs to the weather card above it
        return 1;
    case Timer:
    case Event: // what is happening right now
    case Focus:
    case Stack:
        return 2;
    case NowPlaying:
        return 3;
    case Favourite:
        return 4;
    case Fallback:
        return 5;
    case Plain:
    case StackEntry:
    case OutputLine:
    case Running:
    case Calculation:
    case Choice:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------- key caps

namespace KeyCaps
{
namespace
{
constexpr int CapPad = 6;   // horizontal padding inside a cap
constexpr int CapGap = 3;   // between the caps of one shortcut
constexpr qreal CapRadius = 4.5;

QFont capFont(const QFont &base)
{
    return Chrome::scaledFont(base, 0.82, QFont::Medium);
}

// "Ctrl+Return" → "Ctrl", "↵". A trailing "+" is the plus key itself. Of
// several combinations ("Ctrl+S, Ctrl+Shift+S") only the first is shown.
QStringList tokens(const QString &allKeys)
{
    const QString keys = allKeys.section(QStringLiteral(", "), 0, 0);
    QStringList out = keys.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (keys.endsWith(QLatin1Char('+')))
        out << QStringLiteral("+");
    for (QString &key : out) {
        if (key == QLatin1String("Return") || key == QLatin1String("Enter"))
            key = QStringLiteral("↵");
        else if (key == QLatin1String("Backspace"))
            key = QStringLiteral("⌫");
        else if (key == QLatin1String("Escape"))
            key = QStringLiteral("Esc");
    }
    return out;
}

int capWidth(const QFontMetrics &fm, const QString &key, int height)
{
    return qMax(height + 2, fm.horizontalAdvance(key) + 2 * CapPad);
}
} // namespace

int height(const QFont &base)
{
    return QFontMetrics(capFont(base)).height() + 5;
}

int width(const QFont &base, const QString &keys)
{
    const QFontMetrics fm(capFont(base));
    const int h = height(base);
    int total = 0;
    const QStringList keyList = tokens(keys);
    for (const QString &key : keyList)
        total += capWidth(fm, key, h) + (total ? CapGap : 0);
    return total;
}

int draw(QPainter &p, const QRect &rect, const QString &keys, const QPalette &palette, Qt::Alignment align)
{
    const QFont font = capFont(p.font());
    const QFontMetrics fm(font);
    const int h = height(p.font());
    const int total = width(p.font(), keys);
    const bool dark = Chrome::isDark(palette);

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(font);
    // A cap is a light rounded tile with a slightly heavier bottom edge, so
    // it reads as a key rather than a chip.
    const QColor face = dark ? Chrome::withAlpha(Qt::white, 24) : Chrome::withAlpha(Qt::white, 235);
    const QColor edge = Chrome::withAlpha(palette.color(QPalette::WindowText), dark ? 70 : 60);
    const QColor ink = Chrome::mix(palette.color(QPalette::WindowText), palette.color(QPalette::Window), 0.1);
    qreal x = align & Qt::AlignRight ? rect.right() + 1 - total : rect.left();
    const qreal y = rect.top() + (rect.height() - h) / 2.0;
    const QStringList keyList = tokens(keys);
    for (const QString &key : keyList) {
        const int w = capWidth(fm, key, h);
        const QRectF cap(x + 0.5, y + 0.5, w - 1, h - 2);
        p.setPen(Qt::NoPen);
        p.setBrush(edge);
        p.drawRoundedRect(cap.translated(0, 1.5), CapRadius, CapRadius);
        p.setPen(QPen(edge, 1.0));
        p.setBrush(face);
        p.drawRoundedRect(cap, CapRadius, CapRadius);
        p.setPen(ink);
        p.drawText(cap, Qt::AlignCenter, key);
        x += w + CapGap;
    }
    p.restore();
    return total;
}
} // namespace KeyCaps

// ---------------------------------------------------------------- delegate

namespace
{
constexpr int RowInset = 6;      // row highlight to list edge
constexpr int RowRadius = 9;
constexpr int IconTile = 34;     // the faint tile behind the icon
constexpr int IconSize = 24;
constexpr int MarkerWidth = 3;   // accent bar on the selected row
constexpr qreal BadgeRadius = 7; // the star or playing badge on a tile's corner
constexpr int BarHeight = 3;     // the fraction bar under a card's text
constexpr int BarWidth = 140;
constexpr int ForecastIcon = 22; // a forecast row's bare icon
constexpr int CaptionInset = 14; // caption and divider inset from the row highlight

// A five-pointed star centred on c.
QPainterPath starPath(const QPointF &c, qreal outer)
{
    QPainterPath star;
    const qreal inner = outer * 0.45;
    for (int i = 0; i < 10; ++i) {
        const qreal angle = -M_PI / 2 + i * M_PI / 5;
        const qreal radius = i % 2 ? inner : outer;
        const QPointF point = c + QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
        i ? star.lineTo(point) : star.moveTo(point);
    }
    star.closeSubpath();
    return star;
}

// A small accent disc on the tile's top-right corner, rimmed with the
// well's tone so it stands clear of the icon, with a white glyph inside: a
// star (a favourite), three bars (something playing), a clock face (a
// running timer) or a calendar leaf (the next event).
void drawBadge(QPainter *p, const QRect &tile, ResultModel::Group group, const QPalette &pal)
{
    const QPointF c(tile.right() - 3, tile.top() + 4);
    const QColor ink = pal.color(QPalette::HighlightedText);
    p->setPen(QPen(pal.color(QPalette::Base), 2.0));
    p->setBrush(pal.color(QPalette::Highlight));
    p->drawEllipse(c, BadgeRadius, BadgeRadius);
    p->setPen(Qt::NoPen);
    p->setBrush(ink);
    switch (group) {
    case ResultModel::Favourite:
        p->drawPath(starPath(c + QPointF(0, 0.4), 4.2));
        break;
    case ResultModel::Timer:
        // A clock face with its hands at ten past ten.
        p->setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(c, 3.8, 3.8);
        p->drawLine(c, c + QPointF(0, -2.6));
        p->drawLine(c, c + QPointF(1.9, 0.8));
        break;
    case ResultModel::Event:
        // A calendar leaf: a page with a heavier top edge.
        p->drawRoundedRect(QRectF(c.x() - 3.6, c.y() - 3.4, 7.2, 7.0), 1.2, 1.2);
        p->setBrush(pal.color(QPalette::Highlight));
        p->drawRect(QRectF(c.x() - 2.6, c.y() - 0.6, 5.2, 3.2));
        break;
    case ResultModel::Focus:
        // A target: a ring around a dot.
        p->setPen(QPen(ink, 1.3));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(c, 3.8, 3.8);
        p->setPen(Qt::NoPen);
        p->setBrush(ink);
        p->drawEllipse(c, 1.5, 1.5);
        break;
    case ResultModel::Stack:
        // Two sheets, one behind the other.
        p->drawRoundedRect(QRectF(c.x() - 2.2, c.y() - 3.8, 6.0, 6.0), 1.0, 1.0);
        p->setPen(QPen(pal.color(QPalette::Highlight), 1.0));
        p->drawRoundedRect(QRectF(c.x() - 4.0, c.y() - 2.0, 6.0, 6.0), 1.0, 1.0);
        break;
    default:
        // Three bars of different heights: a still of an equaliser.
        for (int i = 0; i < 3; ++i) {
            const qreal h = i == 1 ? 7 : 4.5;
            p->drawRoundedRect(QRectF(c.x() - 3.6 + i * 2.6, c.y() + 3.5 - h, 1.6, h), 0.8, 0.8);
        }
        break;
    }
}

// A hairline along the last pixel row of the item, below the highlight.
void drawGroupDivider(QPainter *p, const QRect &row, const QPalette &pal)
{
    p->setPen(Chrome::withAlpha(pal.color(QPalette::WindowText), Chrome::isDark(pal) ? 48 : 34));
    const qreal y = row.bottom() + 0.5;
    p->drawLine(QPointF(row.left() + RowInset + CaptionInset, y), QPointF(row.right() - RowInset - CaptionInset + 1, y));
}

// The band over the first fallback row: a hairline like a section divider
// with the caption under it in small muted text, so the rows below read
// as "Use “text” with …" without a header row in the model.
void drawCaptionBand(QPainter *p, const QRect &band, const QString &caption, const QStyleOptionViewItem &option)
{
    const QPalette &pal = option.palette;
    p->setPen(Chrome::withAlpha(pal.color(QPalette::WindowText), Chrome::isDark(pal) ? 48 : 34));
    const qreal y = band.top() + 1.5;
    p->drawLine(QPointF(band.left() + RowInset + CaptionInset, y), QPointF(band.right() - RowInset - CaptionInset + 1, y));
    const QFont font = Chrome::scaledFont(option.font, 0.84, QFont::DemiBold);
    p->setFont(font);
    p->setPen(Chrome::mutedText(pal));
    const QRect text(band.left() + RowInset + CaptionInset, band.top() + 3, band.width() - 2 * (RowInset + CaptionInset), band.height() - 3);
    p->drawText(text, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font).elidedText(caption, Qt::ElideMiddle, text.width()));
}

// The paste stack's order on a tile's corner: an accent disc (a pill for
// two digits) for the item that is pasted next, a muted one for the rest.
void drawNumberBadge(QPainter *p, const QRect &tile, int number, const QPalette &pal)
{
    const QFont font = Chrome::scaledFont(p->font(), 0.72, QFont::Bold);
    const QFontMetrics fm(font);
    const QString text = QString::number(number);
    const qreal w = qMax(2 * BadgeRadius, fm.horizontalAdvance(text) + 7.0);
    const QRectF badge(tile.right() + 4 - w, tile.top() - 3, w, 2 * BadgeRadius);
    const bool next = number == 1;
    p->setPen(QPen(pal.color(QPalette::Base), 2.0));
    p->setBrush(next ? pal.color(QPalette::Highlight) : Chrome::mix(pal.color(QPalette::WindowText), pal.color(QPalette::Base), 0.45));
    p->drawRoundedRect(badge, BadgeRadius, BadgeRadius);
    p->setFont(font);
    p->setPen(next ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Base));
    p->drawText(badge, Qt::AlignCenter, text);
}

// An open arc in the accent colour: a still of a spinner, for the row of a
// program that is still running.
void drawSpinner(QPainter *p, const QRect &tile, const QPalette &pal)
{
    const QRectF ring(tile.center().x() - 7.5, tile.center().y() - 6.5, 16, 16);
    p->setPen(QPen(Chrome::withAlpha(pal.color(QPalette::Highlight), 70), 2.4, Qt::SolidLine, Qt::RoundCap));
    p->setBrush(Qt::NoBrush);
    p->drawEllipse(ring);
    p->setPen(QPen(pal.color(QPalette::Highlight), 2.4, Qt::SolidLine, Qt::RoundCap));
    p->drawArc(ring, 90 * 16, -250 * 16);
}

// The number of an output line where the icon tile would be, so the lines
// read as what a program printed.
void drawLineNumber(QPainter *p, const QRect &gutter, int number, const QPalette &pal)
{
    QFont font = Chrome::scaledFont(p->font(), 0.8);
    font.setFeature(QFont::Tag("tnum"), 1);
    p->setFont(font);
    p->setPen(Chrome::mix(pal.color(QPalette::Text), pal.color(QPalette::Base), 0.55));
    p->drawText(gutter, Qt::AlignVCenter | Qt::AlignRight, QString::number(number));
}

// A thin rounded bar, `fraction` of it filled with the accent colour.
void drawFractionBar(QPainter *p, const QRect &bar, qreal fraction, const QPalette &pal)
{
    const bool dark = Chrome::isDark(pal);
    p->setPen(Qt::NoPen);
    p->setBrush(Chrome::withAlpha(pal.color(QPalette::WindowText), dark ? 40 : 26));
    p->drawRoundedRect(QRectF(bar), bar.height() / 2.0, bar.height() / 2.0);
    const qreal filled = std::clamp(fraction, 0.0, 1.0) * bar.width();
    if (filled >= 1) {
        p->setBrush(pal.color(QPalette::Highlight));
        p->drawRoundedRect(QRectF(bar.left(), bar.top(), qMax<qreal>(filled, bar.height()), bar.height()), bar.height() / 2.0,
                           bar.height() / 2.0);
    }
}

// The kind of an event row ("In 5 min", "Now") as an accent-tinted chip,
// so the imminent stands out from the plain grey kinds; on the selected
// row's tint it is filled more strongly. Returns its width.
int drawAccentChip(QPainter *p, const QRect &area, const QString &text, const QPalette &pal, bool selected)
{
    const QFont font = Chrome::scaledFont(p->font(), 0.86, QFont::DemiBold);
    const QFontMetrics fm(font);
    const int h = Chrome::chipHeight(p->font());
    const int w = fm.horizontalAdvance(text) + 14;
    const QRectF chip(area.right() + 1 - w, area.center().y() - h / 2.0 + 1, w, h);
    const QColor accent = pal.color(QPalette::Highlight);
    p->setPen(Qt::NoPen);
    p->setBrush(Chrome::withAlpha(accent, selected ? (Chrome::isDark(pal) ? 120 : 80) : (Chrome::isDark(pal) ? 70 : 40)));
    p->drawRoundedRect(chip, h / 2.0, h / 2.0);
    p->setFont(font);
    p->setPen(Chrome::isDark(pal) ? accent.lighter(135) : accent.darker(selected ? 150 : 125));
    p->drawText(chip, Qt::AlignCenter, text);
    return w;
}
} // namespace

QSize ResultDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const bool captioned = !index.data(ResultModel::CaptionRole).toString().isEmpty();
    return {option.rect.width(), RowHeight + (captioned ? CaptionBand : 0)};
}

void ResultDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    // The list's style sheet makes its background transparent, which blanks
    // its palette's Window and Base; the rows draw from the application's
    // palette (the theme), as the card around them does.
    const QPalette pal = QGuiApplication::palette();
    const bool dark = Chrome::isDark(pal);
    const bool selected = option.state & QStyle::State_Selected;
    const QColor accent = pal.color(QPalette::Highlight);
    const auto group = ResultModel::Group(index.data(ResultModel::GroupRole).toInt());
    const QString id = index.data(ResultModel::IdRole).toString();

    // A captioned row keeps its band above the highlight.
    QRect itemRect = option.rect;
    if (const QString caption = index.data(ResultModel::CaptionRole).toString(); !caption.isEmpty()) {
        p->setFont(option.font);
        drawCaptionBand(p, QRect(itemRect.left(), itemRect.top(), itemRect.width(), CaptionBand), caption, option);
        itemRect.setTop(itemRect.top() + CaptionBand);
    }
    const QRect r = itemRect.adjusted(RowInset, 1, -RowInset, -1);

    if (selected) {
        // Accent-tinted fill with a hairline of the accent around it, and a
        // short solid accent bar on the left edge as the selection marker.
        const QRectF fillRect = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
        p->setPen(QPen(Chrome::withAlpha(accent, dark ? 110 : 90), 1.0));
        p->setBrush(Chrome::withAlpha(accent, dark ? 64 : 44));
        p->drawRoundedRect(fillRect, RowRadius, RowRadius);
        p->setPen(Qt::NoPen);
        p->setBrush(accent);
        const int markerHeight = r.height() - 18;
        p->drawRoundedRect(QRectF(r.left() + 5, r.center().y() - markerHeight / 2.0 + 0.5, MarkerWidth, markerHeight),
                           MarkerWidth / 2.0, MarkerWidth / 2.0);
    } else if (option.state & QStyle::State_MouseOver) {
        p->setPen(Qt::NoPen);
        p->setBrush(Chrome::withAlpha(pal.color(QPalette::WindowText), dark ? 14 : 10));
        p->drawRoundedRect(r, RowRadius, RowRadius);
    }

    // The last row of a section gets a hairline under it, so the start
    // screen's timers, now-playing and favourites read as sections of
    // their own (and a weather card keeps its forecast rows).
    const QModelIndex below = index.sibling(index.row() + 1, 0);
    const int section = ResultModel::sectionOf(group);
    if (section && below.isValid() && ResultModel::sectionOf(ResultModel::Group(below.data(ResultModel::GroupRole).toInt())) != section)
        drawGroupDivider(p, option.rect, pal);

    // Icon on a faint rounded tile, so line icons and app icons sit on the
    // same footprint. Cards (answers, timers, events, now playing, the
    // focus session, the paste stack) get the accent tint, as does the
    // spinner of a running program; a forecast row shows its weather icon
    // bare, so the days sit quietly under the weather card, and an output
    // line has its number there instead.
    const bool card = group == ResultModel::Answer || group == ResultModel::Timer || group == ResultModel::Event
        || group == ResultModel::Focus || group == ResultModel::Stack;
    const bool featured = card || group == ResultModel::NowPlaying || group == ResultModel::Running;
    const QRect tile(r.left() + 12, r.center().y() - IconTile / 2 + 1, IconTile, IconTile);
    p->setFont(option.font);
    if (group == ResultModel::Forecast) {
        const QRect iconRect(tile.center().x() - ForecastIcon / 2 + 1, tile.center().y() - ForecastIcon / 2 + 1, ForecastIcon, ForecastIcon);
        index.data(Qt::DecorationRole).value<QIcon>().paint(p, iconRect);
    } else if (group == ResultModel::OutputLine) {
        drawLineNumber(p, tile, id.section(u':', 1).toInt() + 1, pal);
    } else {
        const bool quiet = group == ResultModel::Fallback;
        p->setPen(featured ? QPen(Chrome::withAlpha(accent, dark ? 90 : 70), 1.0) : QPen(Qt::NoPen));
        p->setBrush(featured ? Chrome::withAlpha(accent, dark ? 50 : 32)
                             : Chrome::withAlpha(pal.color(QPalette::WindowText), quiet ? (dark ? 10 : 6) : (dark ? 16 : 9)));
        p->drawRoundedRect(featured ? QRectF(tile).adjusted(0.5, 0.5, -0.5, -0.5) : QRectF(tile), 9, 9);
        if (group == ResultModel::Running) {
            drawSpinner(p, tile, pal);
        } else {
            const QRect iconRect(tile.center().x() - IconSize / 2 + 1, tile.center().y() - IconSize / 2 + 1, IconSize, IconSize);
            index.data(Qt::DecorationRole).value<QIcon>().paint(p, iconRect);
        }
    }
    if (group == ResultModel::Favourite || group == ResultModel::NowPlaying || group == ResultModel::Timer || group == ResultModel::Event
        || group == ResultModel::Focus || group == ResultModel::Stack)
        drawBadge(p, tile, group, pal);
    else if (group == ResultModel::StackEntry)
        drawNumberBadge(p, tile, id.section(u':', 1).toInt() + 1, pal);

    // Kind on the right: key caps for a shortcut, otherwise a chip; an
    // accent chip for the imminent ("In 5 min") and the current theme. A
    // forecast row's kind is its high and low, drawn with the row itself;
    // a fallback row's is said by the caption above the group.
    const QString kind = index.data(ResultModel::KindRole).toString();
    const bool keyKind = index.data(ResultModel::KeyKindRole).toBool();
    int kindWidth = 0;
    const int kindRight = r.right() - 10;
    if (group == ResultModel::Forecast || group == ResultModel::Fallback) {
        // drawn below, or not at all
    } else if (!kind.isEmpty() && keyKind) {
        kindWidth = KeyCaps::draw(*p, QRect(r.left(), r.top(), kindRight - r.left() + 1, r.height()), kind, pal, Qt::AlignRight);
    } else if (!kind.isEmpty() && (group == ResultModel::Event || group == ResultModel::Choice)) {
        kindWidth = drawAccentChip(p, QRect(r.left(), r.top(), kindRight - r.left() + 1, r.height()), kind, pal, selected);
    } else if (!kind.isEmpty()) {
        const int chipH = Chrome::chipHeight(option.font);
        const QFontMetrics chipFm(Chrome::scaledFont(option.font, 0.86));
        const QRect chipArea(r.left(), r.center().y() - chipH / 2 + 1, kindRight - r.left() + 1, chipH);
        const QString shown = chipFm.elidedText(kind, Qt::ElideRight, r.width() / 3);
        Chrome::drawChips(*p, chipArea, {shown}, pal, Qt::AlignRight);
        kindWidth = chipFm.horizontalAdvance(shown) + 14;
    }

    const int x = tile.right() + 13;
    const int textRight = kindRight - kindWidth - 14;
    const QString title = index.data(Qt::DisplayRole).toString();
    const QString subtitle = index.data(ResultModel::SubtitleRole).toString();
    const QRect textArea(x, r.top(), textRight - x, r.height());
    QStyleOptionViewItem text = option;
    text.palette = pal;
    if (card || group == ResultModel::Calculation)
        paintAnswer(p, text, textArea, title, subtitle, index.data(ResultModel::FractionRole).toReal());
    else if (group == ResultModel::Forecast)
        paintForecast(p, text, textArea, title, subtitle, kind);
    else if (group == ResultModel::OutputLine)
        paintLine(p, text, textArea, title);
    else
        paintTitle(p, text, textArea, title, subtitle, group == ResultModel::Fallback);
    p->restore();
}

// Title in medium weight with the dimmer subtitle after it on one line. A
// quiet row (a fallback) sets its title in the regular weight, a little
// muted, so the results proper stay in front.
void ResultDelegate::paintTitle(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &title,
                                const QString &subtitle, bool quiet)
{
    const QFont titleFont = Chrome::scaledFont(option.font, 1.0, quiet ? QFont::Normal : QFont::Medium);
    const QFontMetrics titleFm(titleFont);
    const QString shown = titleFm.elidedText(title, Qt::ElideRight, qMax(0, area.width()));
    p->setFont(titleFont);
    p->setPen(quiet ? Chrome::mix(option.palette.color(QPalette::Text), option.palette.color(QPalette::Base), 0.22)
                    : option.palette.color(QPalette::Text));
    p->drawText(area, Qt::AlignVCenter | Qt::AlignLeft, shown);
    const int x = area.left() + titleFm.horizontalAdvance(shown) + 10;
    if (!subtitle.isEmpty() && x < area.right() - 30) {
        const QFont subFont = Chrome::scaledFont(option.font, 0.94);
        const QFontMetrics fm(subFont);
        p->setFont(subFont);
        p->setPen(Chrome::mutedText(option.palette));
        p->drawText(QRect(x, area.top(), area.right() - x, area.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    fm.elidedText(subtitle, Qt::ElideRight, area.right() - x));
    }
}

// One line a program printed, in the system's monospace font; leading
// spaces stay, so columns line up from row to row.
void ResultDelegate::paintLine(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &line)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(option.font.pointSizeF() * 0.95);
    const QFontMetrics fm(font);
    p->setFont(font);
    p->setPen(option.palette.color(QPalette::Text));
    p->drawText(area, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextForceLeftToRight, fm.elidedText(line, Qt::ElideRight, area.width()));
}

// An answer: the result large on top, its detail ("Tokyo · 15 h ahead of
// you") in a smaller muted line under it, and a thin bar beneath when the
// result is a fraction of something (memory in use, a timer's progress).
// All of it fits the standard row.
void ResultDelegate::paintAnswer(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &title,
                                 const QString &subtitle, qreal fraction)
{
    QFont titleFont = Chrome::scaledFont(option.font, 1.18, QFont::DemiBold);
    titleFont.setFeature(QFont::Tag("tnum"), 1); // a countdown keeps its width from second to second
    const QFont subFont = Chrome::scaledFont(option.font, 0.88);
    const QFontMetrics titleFm(titleFont);
    const QFontMetrics subFm(subFont);
    const bool bar = fraction >= 0;
    const int titleH = titleFm.height();
    const int subH = subtitle.isEmpty() ? 0 : subFm.height();
    const int barH = bar ? BarHeight + 3 : 0; // with its gap above
    int y = area.top() + (area.height() - titleH - subH - barH) / 2;
    p->setFont(titleFont);
    p->setPen(option.palette.color(QPalette::Text));
    p->drawText(QRect(area.left(), y, area.width(), titleH), Qt::AlignVCenter | Qt::AlignLeft,
                titleFm.elidedText(title, Qt::ElideRight, area.width()));
    y += titleH;
    if (subH) {
        p->setFont(subFont);
        p->setPen(Chrome::mutedText(option.palette));
        p->drawText(QRect(area.left(), y, area.width(), subH), Qt::AlignVCenter | Qt::AlignLeft,
                    subFm.elidedText(subtitle, Qt::ElideRight, area.width()));
        y += subH;
    }
    if (bar)
        drawFractionBar(p, QRect(area.left(), y + 3, qMin(BarWidth, area.width()), BarHeight), fraction, option.palette);
}

// A day of the forecast: the day and its weather on one line, the high
// and low ("7° / −1°") right-aligned in equal-width digits, the low muted.
void ResultDelegate::paintForecast(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &title,
                                   const QString &subtitle, const QString &temperatures)
{
    QFont tempFont = Chrome::scaledFont(option.font, 1.0, QFont::Medium);
    tempFont.setFeature(QFont::Tag("tnum"), 1);
    const QFontMetrics fm(tempFont);
    const QString high = temperatures.section(QStringLiteral(" / "), 0, 0);
    const QString low = temperatures.section(QStringLiteral(" / "), 1);
    const int lowW = low.isEmpty() ? 0 : fm.horizontalAdvance(QStringLiteral(" / ") + low);
    const int highW = fm.horizontalAdvance(high);
    int x = area.right() + 1 - lowW;
    p->setFont(tempFont);
    if (!low.isEmpty()) {
        p->setPen(Chrome::mutedText(option.palette));
        p->drawText(QRect(x, area.top(), lowW, area.height()), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral(" / ") + low);
    }
    x -= highW;
    p->setPen(option.palette.color(QPalette::Text));
    p->drawText(QRect(x, area.top(), highW, area.height()), Qt::AlignVCenter | Qt::AlignLeft, high);
    paintTitle(p, option, QRect(area.left(), area.top(), x - 14 - area.left(), area.height()), title, subtitle);
}
