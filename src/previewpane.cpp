#include "previewpane.h"
#include "item.h"
#include "style.h"

#include <chrome.h>
#include <motion.h>

#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace
{
constexpr int PadX = 12;      // pane edge to the content
constexpr int PadTop = 10;
constexpr int PadBottom = 6;
constexpr int ThumbMargin = 18; // stage edge to a window thumbnail
constexpr qreal ThumbCorner = 6;

// The bounding box of the pixels that are (nearly) opaque. KWin's window
// thumbnails carry transparent margins for the decoration shadow; cropping
// to the window itself lets the painted shadow hug its edges.
QRect opaqueBounds(const QImage &image)
{
    if (!image.hasAlphaChannel())
        return image.rect();
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    int left = argb.width(), top = argb.height(), right = -1, bottom = -1;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(row[x]) < 200)
                continue;
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
    }
    return right < 0 ? image.rect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

// A window thumbnail: the image scaled to fit, on the stage with rounded
// corners and a soft shadow, like a photo in Quick Look's panel.
class ThumbnailView : public QWidget
{
public:
    ThumbnailView(const QString &path, QWidget *parent)
        : QWidget(parent)
    {
        const QImage image(path);
        m_image = image.copy(opaqueBounds(image));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_image.isNull())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect area = rect().adjusted(ThumbMargin, ThumbMargin, -ThumbMargin, -ThumbMargin);
        const qreal dpr = devicePixelRatioF();
        QSize fitted = m_image.size();
        if (fitted.width() > area.width() * dpr || fitted.height() > area.height() * dpr)
            fitted = fitted.scaled((QSizeF(area.size()) * dpr).toSize(), Qt::KeepAspectRatio);
        if (m_scaled.size() != fitted) {
            m_scaled = QPixmap::fromImage(m_image.scaled(fitted, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_scaled.setDevicePixelRatio(dpr);
        }
        const QSizeF logical = QSizeF(m_scaled.size()) / dpr;
        const QRectF frame(QPointF(area.center()) - QPointF(logical.width() / 2, logical.height() / 2), logical);
        const bool dark = Chrome::isDark(palette());
        Chrome::drawShadow(p, frame, ThumbCorner, 16, dark ? 130 : 80, 5);
        p.setClipPath(Chrome::roundedPath(frame, ThumbCorner));
        p.drawPixmap(frame.topLeft(), m_scaled);
        p.setClipping(false);
        // A hairline keeps light windows from melting into a light stage.
        p.setPen(Chrome::withAlpha(dark ? Qt::white : Qt::black, dark ? 40 : 50));
        p.setBrush(Qt::NoBrush);
        p.drawPath(Chrome::roundedPath(frame.adjusted(0.5, 0.5, -0.5, -0.5), ThumbCorner));
    }

private:
    QImage m_image;
    QPixmap m_scaled; // cache for the current widget size
};

QString shortDate(const QDateTime &when)
{
    return QLocale().toString(when.date(), QStringLiteral("d MMM yyyy"));
}

} // namespace

// The line under the content: the file or window name, with metadata chips
// ("PNG image", "1600 × 1000", "4 Nov 2026") centred beneath it.
class DetailsBar : public QWidget
{
public:
    explicit DetailsBar(QWidget *parent)
        : QWidget(parent)
    {
        setFont(Chrome::scaledFont(font(), 0.95, QFont::Medium));
    }

    void set(const QString &caption, const QStringList &chips)
    {
        m_caption = caption;
        m_chips = chips;
        int height = 0;
        if (!m_caption.isEmpty())
            height += QFontMetrics(font()).height() + 2;
        if (!m_chips.isEmpty())
            height += Chrome::chipHeight(font()) + (m_caption.isEmpty() ? 0 : 4);
        setFixedHeight(height ? height + 8 : 0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setFont(font());
        const QFontMetrics fm(font());
        int y = 8;
        if (!m_caption.isEmpty()) {
            p.setPen(Chrome::mix(palette().color(QPalette::Text), palette().color(QPalette::Base), 0.15));
            p.drawText(QRect(0, y, width(), fm.height()), Qt::AlignCenter, fm.elidedText(m_caption, Qt::ElideMiddle, width()));
            y += fm.height() + 6;
        }
        if (!m_chips.isEmpty())
            Chrome::drawChips(p, QRect(0, y, width(), Chrome::chipHeight(font())), m_chips, palette(), Qt::AlignHCenter);
    }

private:
    QString m_caption;
    QStringList m_chips;
};

PreviewPane::PreviewPane(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QVBoxLayout(this))
    , m_details(new DetailsBar(this))
    , m_spacer(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding))
{
    m_layout->setContentsMargins(PadX, PadTop, PadX, PadBottom);
    m_layout->setSpacing(0);
    m_layout->addWidget(m_details);
    m_layout->insertSpacerItem(0, m_spacer); // keeps the details at the bottom while empty
    m_details->set({}, {});
    restyle();

    m_delay.setSingleShot(true);
    m_delay.setInterval(90);
    connect(&m_delay, &QTimer::timeout, this, &PreviewPane::rebuild);
}

void PreviewPane::showItem(const Item *item)
{
    m_image = item ? item->previewImage : QString();
    m_caption = item ? item->title : QString();
    m_path = item && m_image.isEmpty() ? item->filePath : QString();
    m_text = item && m_path.isEmpty() ? item->previewText : QString();
    m_markdown = item && item->markdown;
    m_monospace = item && item->monospace;
    m_facts.clear();
    if (item && !m_image.isEmpty()) {
        for (const QString &fact : {item->subtitle, item->kind})
            if (!fact.isEmpty())
                m_facts << fact;
    }
    m_delay.start();
}

void PreviewPane::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    delete m_content; // stop any media and free memory
    m_shownKey.clear();
}

void PreviewPane::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        restyle();
}

void PreviewPane::restyle()
{
    const QPalette pal = QGuiApplication::palette();
    setPalette(pal); // reaches the details bar and the content
    setStyleSheet(Style::scrollBars(pal, Style::wellTone(pal)) + Style::transparentText(pal));
}

void PreviewPane::rebuild()
{
    const QString key = !m_image.isEmpty() ? u'I' + m_image
        : m_path.isEmpty()                ? (m_markdown ? u'M' : m_monospace ? u'C' : u'T') + m_text
                                          : u'F' + m_path;
    if (key == m_shownKey && m_content)
        return;
    m_shownKey = key;
    // What is shown now fades out over what comes next.
    FadeOut::begin(this, rect());
    delete m_content; // the layout drops its slot by itself
    if (m_spacer) {
        m_layout->removeItem(m_spacer);
        delete m_spacer;
        m_spacer = nullptr;
    }
    m_fileFacts.clear();
    m_backdrop = Viewer::Paper;

    if (!m_image.isEmpty()) {
        m_content = new ThumbnailView(m_image, this);
        m_backdrop = Viewer::Stage;
    } else if (!m_path.isEmpty()) {
        Viewer *viewer = createViewer(m_path, this, false);
        m_content = viewer;
        m_backdrop = viewer->backdrop();
        // Facts the viewer finds later (an image's size) join the chips.
        connect(viewer, &Viewer::detailsChanged, this, &PreviewPane::updateDetails);
        const QFileInfo info(m_path);
        m_caption = info.fileName();
        m_fileFacts << QMimeDatabase().mimeTypeForFile(info).comment();
        if (!info.isDir())
            m_fileFacts << QLocale().formattedDataSize(info.size());
        m_fileFacts << shortDate(info.lastModified());
    } else if (!m_text.isEmpty() && m_markdown) {
        auto *text = new QTextBrowser(this);
        text->setOpenExternalLinks(true);
        text->setFocusPolicy(Qt::NoFocus);
        text->setFrameShape(QFrame::NoFrame);
        text->viewport()->setAutoFillBackground(false);
        text->setAttribute(Qt::WA_TranslucentBackground); // the scroll bar's lane shows the well, like the list's
        text->document()->setDocumentMargin(14);
        text->document()->setDefaultFont(Chrome::scaledFont(font(), 0.98));
        text->setMarkdown(m_text);
        Style::styleMarkdown(text->document(), palette()); // headings, code, links, tables in Lumen's tones
        m_content = text;
        m_caption.clear();
    } else if (!m_text.isEmpty()) {
        auto *text = new QPlainTextEdit(this);
        text->setReadOnly(true);
        text->setFocusPolicy(Qt::NoFocus);
        text->setFrameShape(QFrame::NoFrame);
        text->viewport()->setAutoFillBackground(false);
        text->setAttribute(Qt::WA_TranslucentBackground);
        text->document()->setDocumentMargin(14);
        if (m_monospace) {
            // A program's output keeps its columns.
            QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            fixed.setPointSizeF(font().pointSizeF() * 0.92);
            text->setFont(fixed);
            text->setLineWrapMode(QPlainTextEdit::NoWrap);
        }
        text->setPlainText(m_text);
        m_content = text;
        m_caption.clear();
        const auto lines = m_text.count(u'\n') + 1;
        m_fileFacts << (m_text.size() == 1 ? tr("1 character") : tr("%1 characters").arg(m_text.size()));
        if (lines > 1)
            m_fileFacts << tr("%1 lines").arg(lines);
    } else {
        // Nothing to show: the empty state is painted on the pane itself.
        m_content.clear();
        m_caption.clear();
    }
    if (m_content) {
        m_layout->insertWidget(0, m_content, 1);
        m_content->show();
    } else {
        m_spacer = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
        m_layout->insertSpacerItem(0, m_spacer);
    }
    updateDetails();
    update();
}

void PreviewPane::updateDetails()
{
    QStringList chips = m_fileFacts;
    if (auto *viewer = qobject_cast<Viewer *>(m_content.data()))
        chips = QStringList{chips.value(0)} + viewer->details() + chips.mid(1);
    chips += m_facts;
    chips.removeAll(QString());
    m_details->set(m_content ? m_caption : QString(), m_content ? chips : QStringList());
}

void PreviewPane::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!m_content) {
        drawEmptyState(p);
        return;
    }
    if (m_backdrop == Viewer::Paper)
        return; // text and documents read straight off the well's paper
    // The stage behind images, thumbnails and info cards: Quick Look's
    // muted neutral, or black behind video. Its rounded corners match the
    // clipping of the image viewer, which fills this rect exactly.
    const QPalette pal = palette();
    const bool dark = Chrome::isDark(pal);
    const QColor tone = m_backdrop == Viewer::Screen ? QColor(Qt::black)
                                                      : Chrome::mix(pal.color(QPalette::Window), Qt::black, dark ? 0.28 : 0.05);
    const QRectF stage = QRectF(m_content->geometry()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(Chrome::withAlpha(Qt::black, dark ? 90 : 22));
    p.setBrush(tone);
    p.drawRoundedRect(stage, Chrome::WellRadius, Chrome::WellRadius);
}

// "No preview" with a small picture-frame glyph, in the same tones as the
// launcher's "No results" state.
void PreviewPane::drawEmptyState(QPainter &p) const
{
    const QPalette pal = palette();
    const QColor strong = Chrome::mix(pal.color(QPalette::Text), pal.color(QPalette::Base), 0.35);
    const QColor faint = Chrome::mix(pal.color(QPalette::Text), pal.color(QPalette::Base), 0.55);
    const QPointF c(width() / 2.0, height() / 2.0 - 30);

    // The glyph: a rounded frame with a sun and a hill inside it.
    const QRectF frame(c.x() - 22, c.y() - 17, 44, 34);
    p.setPen(QPen(faint, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(frame, 6, 6);
    p.setPen(Qt::NoPen);
    p.setBrush(faint);
    p.drawEllipse(frame.topLeft() + QPointF(13, 11), 3.2, 3.2);
    QPainterPath hill;
    hill.moveTo(frame.left() + 6, frame.bottom() - 6);
    hill.lineTo(frame.left() + 17, frame.bottom() - 17);
    hill.lineTo(frame.left() + 25, frame.bottom() - 10);
    hill.lineTo(frame.left() + 30, frame.bottom() - 14);
    hill.lineTo(frame.right() - 6, frame.bottom() - 6);
    hill.closeSubpath();
    p.drawPath(hill);

    QRect textArea(0, int(frame.bottom()) + 14, width(), 100);
    p.setFont(Chrome::scaledFont(font(), 1.15, QFont::DemiBold));
    p.setPen(strong);
    p.drawText(textArea, Qt::AlignHCenter | Qt::AlignTop, tr("No preview"));
    textArea.translate(0, QFontMetrics(p.font()).height() + 6);
    p.setFont(Chrome::scaledFont(font(), 0.95));
    p.setPen(faint);
    p.drawText(textArea, Qt::AlignHCenter | Qt::AlignTop, tr("Select a result to see it here"));
}
