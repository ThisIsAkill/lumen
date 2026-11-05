#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <viewers.h>

class DetailsBar;
class QSpacerItem;
class QVBoxLayout;
struct Item;

// A live preview beside the results in the Files, Clipboard and Windows
// views: the selected file through Quick Look's viewers (videos paused on
// their first frame), a clipboard text in full, or a window's thumbnail.
// Rebuilt shortly after the selection settles, so arrowing quickly through
// a list stays smooth.
//
// The pane shares the launcher's result well. Content that is not paper
// (images, thumbnails, info cards) sits on a toned stage with rounded
// corners, and a details bar under it names the content with metadata
// chips, like Quick Look's header.
class PreviewPane : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewPane(QWidget *parent = nullptr);

    void showItem(const Item *item);
    // Takes the application palette (a theme change): the pane's style
    // sheet pins its own palette, so the launcher passes the change on.
    void restyle();

protected:
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void rebuild();
    void updateDetails();
    void drawEmptyState(QPainter &p) const;

    QTimer m_delay;
    QString m_path;
    QString m_text;
    bool m_markdown = false;
    bool m_monospace = false;
    QString m_image;
    QString m_caption;
    QStringList m_facts;     // short facts from the result itself (app id, "Active")
    QStringList m_fileFacts; // kind, size and date of a previewed file
    QString m_shownKey;      // what the current content shows
    Viewer::Backdrop m_backdrop = Viewer::Paper; // the tone behind the content
    QVBoxLayout *m_layout;
    QPointer<QWidget> m_content;
    DetailsBar *m_details;
    QSpacerItem *m_spacer; // fills the content's place while there is none
};
