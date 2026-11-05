#pragma once

#include "item.h"

#include <motion.h>

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QListView>
#include <QPixmap>
#include <QSet>
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

// The list of results. The selection highlight belongs to the list rather
// than to the rows: it glides from row to row (and follows a result that
// moves as the results refine) instead of jumping, and flashes to confirm
// an action (pulse()). Rows new to the list settle in with a short rise and
// fade, one shortly after the other, while rows that stay put do not move,
// so typing never flickers. When a view opens or closes, the whole content
// slides in from the side the navigation went (revealNext()).
//
// All of this follows KDE's animation speed (see motion.h): at "instant"
// the list behaves like a plain QListView.
class ResultList : public QListView
{
    Q_OBJECT
public:
    enum Reveal {
        FromRight, // a view opened: deeper into the launcher
        FromLeft,  // a view left: back towards the main search
        FromBelow, // a list that pops up over the results (actions)
        FadeIn,    // back from one of those
    };

    explicit ResultList(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *model) override;
    // The content of the next results slides in from that side.
    void revealNext(Reveal how);
    // Flashes the highlight: something was done to the selected result.
    void pulse();

    // For the delegate: how far a row has settled in, 0 (just arrived) to 1.
    qreal settled(const QModelIndex &index) const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;
    void scrollContentsBy(int dx, int dy) override;
    void hideEvent(QHideEvent *event) override;

private:
    void aboutToReset();
    void wasReset();
    void scheduleCapture(); // a fresh picture of the content for the slide, once this event is done
    void captureSlide();
    void paintHighlight(QPainter &p, const QRectF &r) const;
    QRectF highlightRect(const QModelIndex &index) const;
    QRectF currentHighlight() const; // where it belongs right now, mid-glide included

    // The highlight's glide: from where it was drawn to the current row.
    Tween m_glide;
    QRectF m_glideFrom;
    QRectF m_drawn;                // where it was painted last
    bool m_highlightShown = false; // painted last time: a glide has somewhere to start from
    Tween m_pulse;

    // Rows settling in, by result id, with the moment each one started.
    QElapsedTimer m_clock;
    QHash<QString, qint64> m_settleStart;
    QSet<QString> m_idsBefore; // the ids before the current reset
    Tween m_settleTicker;      // repaints while rows settle

    // The content sliding in: drawn from a picture taken outside the paint
    // event (a widget cannot render itself while it is being painted).
    Reveal m_reveal = FadeIn;
    qint64 m_revealAsked = -1; // when revealNext() was called, or -1
    Tween m_slide;
    QPixmap m_slidePicture;
    bool m_capturing = false;       // painting into the picture
    bool m_captureQueued = false;
};

// Draws a row: the icon on a faint tile, title, a dimmer subtitle after it,
// and the kind on the right as a chip (or key caps for an action's
// shortcut). The list paints the highlight of the selected row (see
// ResultList) under the row's content.
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
// centred. `pressed` (0–1) sinks the caps into the surface, as a key being
// held. Returns the width used.
int draw(QPainter &p, const QRect &rect, const QString &keys, const QPalette &palette,
         Qt::Alignment align = Qt::AlignLeft, qreal pressed = 0);
} // namespace KeyCaps
