#pragma once

#include "item.h"

#include <QAbstractListModel>
#include <QStyledItemDelegate>

// The rows currently shown: results, or the actions of one result.
class ResultModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role {
        SubtitleRole = Qt::UserRole + 1,
        KindRole,
        KeyKindRole, // the kind is a keyboard shortcut, drawn as key caps
        GroupRole,   // a Group: rows of a kind that get their own treatment
        FractionRole, // Item::fraction: a bar under the text, or -1
        IdRole,       // Item::id, for rows whose look depends on it (line numbers, stack positions)
        CaptionRole,  // a caption drawn in a band above the row ("Use “…” with" over the first fallback), or empty
    };

    // Rows the delegate draws differently from a plain result (see the
    // delegate). Told apart by the result's id.
    enum Group {
        Plain,
        Answer,     // an instant answer ("answer:*"): a larger title over its detail
        Forecast,   // the days after a weather answer ("weather-day:*"): compact, under the card
        Timer,      // a running timer ("timer:*"): a card with its progress
        Event,      // the next calendar event ("event:*"): a card with its "In 5 min"
        NowPlaying, // the start screen's media row ("media:now-playing")
        Favourite,  // a kept result ("favourite:*"): a star badge on its icon
        Focus,      // the running focus session ("lumen:focus"): a card like a timer's
        Stack,      // the paste stack while it collects ("lumen:paste-stack"): a card too
        StackEntry, // an item waiting in the paste stack ("stack:*"): its position as a badge
        OutputLine, // a line a program printed ("output-line:*"): monospace, numbered in the gutter
        Running,    // the row while a program still runs ("output:running")
        Calculation, // a past calculation ("calculation:*"): the result over its expression
        Choice,     // a theme or accent ("theme:*", "accent:*"): "Current" as an accent chip
        Fallback,   // a command offered for the text typed ("fallback:*"): quiet rows closing the list
    };
    static Group groupOf(const Item &item);
    // Groups that sit together on the start screen share a section; a
    // hairline separates sections. Plain rows have none.
    static int sectionOf(Group group);

    using QAbstractListModel::QAbstractListModel;

    void setItems(const QList<Item> &items);
    const QList<Item> &items() const { return m_items; }
    // The text the fallback rows are offered for: the first of them gets
    // the "Use “text” with" caption (CaptionRole).
    void setSearchText(const QString &text) { m_searchText = text; }
    const Item *itemAt(int row) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    // Rows for local files can be dragged out as files.
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
    QList<Item> m_items;
    QString m_searchText;
};

// Draws a row: the icon on a faint tile, title, a dimmer subtitle after it,
// and the kind on the right as a chip (or key caps for an action's
// shortcut). The selected row gets an accent-tinted highlight with a short
// accent bar on its left edge.
//
// Answers, running timers, the next event and the now-playing row sit on
// an accent-tinted tile; the first three also set their title large over
// the detail line, so they read as the cards they are, with a thin bar
// under the text when the result carries a fraction (memory in use, how
// far a timer has come). Favourites, timers, events and media carry a
// badge on the tile's corner. The days after a weather answer are compact
// rows under its card. A hairline under the last row of a section
// separates it from the plain results, which gives the start screen its
// sections without header rows.
//
// The running focus session and the paste stack are cards of that family.
// A program's output lines are set in monospace with a muted line number
// where the icon would be; a past calculation shows its result over the
// expression. The fallback commands close the list as quieter rows under a
// caption band ("Use “text” with"), the only row that is taller than the
// rest.
class ResultDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static constexpr int RowHeight = 48;
    static constexpr int CaptionBand = 24; // the band over a row with a caption

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    static void paintTitle(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &title,
                           const QString &subtitle, bool quiet = false);
    static void paintLine(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &line);
    static void paintAnswer(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &title,
                            const QString &subtitle, qreal fraction);
    static void paintForecast(QPainter *p, const QStyleOptionViewItem &option, const QRect &area, const QString &title,
                              const QString &subtitle, const QString &temperatures);
};

// Keyboard shortcuts drawn as small key caps: "Ctrl+K" becomes two caps,
// "Return" a single "↵" cap. Shared by the result rows and the footer.
namespace KeyCaps
{
int height(const QFont &base);
int width(const QFont &base, const QString &keys);
// Draws the caps for `keys` inside rect, left- or right-aligned, vertically
// centred. Returns the width used.
int draw(QPainter &p, const QRect &rect, const QString &keys, const QPalette &palette,
         Qt::Alignment align = Qt::AlignLeft);
} // namespace KeyCaps
