#include "noteswindow.h"
#include "library.h"
#include "style.h"

#include <chrome.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace
{
constexpr int Pad = 10;      // window edge to the paper well
constexpr int WellInset = 4; // well edge to the text view
} // namespace

NotesWindow::NotesWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_text(new QPlainTextEdit(this))
    , m_saveNote(new QLabel(tr("Saved as you type"), this))
    , m_count(new QLabel(this))
{
    setWindowTitle(tr("Notes"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("note")));
    resize(460, 520);

    // Layout: the paper well with the text, then a footer line under it.
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(Pad + WellInset, Pad + 4 + WellInset, Pad + WellInset, 0);
    column->setSpacing(0);
    column->addWidget(m_text, 1);
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(Pad - WellInset + 6, WellInset + 8, Pad - WellInset + 6, 9);
    footer->addWidget(m_saveNote, 1);
    footer->addWidget(m_count);
    column->addLayout(footer);

    m_text->setFrameShape(QFrame::NoFrame);
    m_text->viewport()->setAutoFillBackground(false);
    m_text->document()->setDocumentMargin(18);
    m_text->setCursorWidth(2);
    m_text->setPlaceholderText(tr("Jot something down. It is saved as you type."));
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPointSizeF(font.pointSizeF() * 1.1);
    m_text->setFont(font);
    m_saveNote->setFont(Chrome::scaledFont(m_saveNote->font(), 0.9, QFont::Medium));
    m_count->setFont(Chrome::scaledFont(m_count->font(), 0.9));
    restyle();

    m_saveDelay.setSingleShot(true);
    m_saveDelay.setInterval(400);
    connect(&m_saveDelay, &QTimer::timeout, this, &NotesWindow::save);
    connect(m_text, &QPlainTextEdit::textChanged, &m_saveDelay, qOverload<>(&QTimer::start));
    connect(m_text, &QPlainTextEdit::textChanged, this, &NotesWindow::updateCount);
    load();
}

QString NotesWindow::file()
{
    return Library::configuredFolder() + QStringLiteral("/notes.md"); // syncs along with the library
}

void NotesWindow::load()
{
    QFile f(file());
    m_text->setPlainText(f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString());
    m_saveDelay.stop(); // loading is not an edit
}

QString NotesWindow::firstLine() const
{
    return m_text->toPlainText().section(u'\n', 0, 0).trimmed();
}

void NotesWindow::showAndFocus()
{
    if (!isVisible())
        load(); // it may have changed on another computer
    show();
    raise();
    activateWindow();
    m_text->setFocus();
    m_text->moveCursor(QTextCursor::End);
}

void NotesWindow::save()
{
    QDir().mkpath(QFileInfo(file()).path());
    QSaveFile f(file());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(m_text->toPlainText().toUtf8());
        f.commit();
    }
}

void NotesWindow::updateCount()
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const auto words = m_text->toPlainText().split(whitespace, Qt::SkipEmptyParts).size();
    m_count->setText(words == 0 ? QString() : words == 1 ? tr("1 word") : tr("%1 words").arg(words));
}

void NotesWindow::restyle()
{
    const QPalette pal = palette();
    const QColor muted = Chrome::mutedText(pal);
    QPalette text = m_text->palette();
    text.setColor(QPalette::PlaceholderText, Chrome::mix(pal.color(QPalette::Text), pal.color(QPalette::Base), 0.5));
    m_text->setPalette(text);
    m_text->setStyleSheet(Style::transparentText(pal) + Style::scrollBars(pal));
    const QString mutedStyle = QStringLiteral("color: %1").arg(muted.name(QColor::HexArgb));
    m_saveNote->setStyleSheet(mutedStyle);
    m_count->setStyleSheet(mutedStyle);
}

void NotesWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        restyle();
}

void NotesWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const bool dark = Chrome::isDark(pal);
    const QRectF body = rect();

    // The glass body, as on the launcher card but edge to edge: the window
    // manager draws the frame. The accent wash sits behind the top band.
    const QColor base = Chrome::mix(pal.color(QPalette::Window), pal.color(QPalette::Base), dark ? 0.0 : 0.3);
    QLinearGradient glass(body.topLeft(), body.bottomLeft());
    glass.setColorAt(0.0, Chrome::mix(base, Qt::white, dark ? 0.07 : 0.3));
    glass.setColorAt(1.0, base);
    p.fillRect(body, glass);
    QLinearGradient wash(body.topLeft(), QPointF(body.left(), body.top() + 70));
    wash.setColorAt(0.0, Chrome::withAlpha(pal.color(QPalette::Highlight), dark ? 38 : 26));
    wash.setColorAt(1.0, Qt::transparent);
    p.fillRect(body, wash);

    // Lumen's mark: the hairline of accent light along the top edge.
    QLinearGradient beam(body.topLeft(), body.topRight());
    beam.setColorAt(0.0, Qt::transparent);
    beam.setColorAt(0.5, Chrome::withAlpha(pal.color(QPalette::Highlight), dark ? 200 : 170));
    beam.setColorAt(1.0, Qt::transparent);
    p.setPen(QPen(QBrush(beam), 1.5));
    p.drawLine(QPointF(body.left(), body.top() + 0.75), QPointF(body.right(), body.top() + 0.75));

    // The paper well behind the text, toned like the launcher's result well.
    const QRectF well = QRectF(m_text->geometry()).adjusted(-WellInset + 0.5, -WellInset + 0.5, WellInset - 0.5, WellInset - 0.5);
    const QColor tone = dark ? Chrome::mix(pal.color(QPalette::Base), Qt::black, 0.08) : pal.color(QPalette::Base);
    p.setPen(Chrome::withAlpha(Qt::black, dark ? 90 : 22));
    p.setBrush(Chrome::withAlpha(tone, dark ? 215 : 235));
    p.drawRoundedRect(well, Chrome::WellRadius, Chrome::WellRadius);
}

void NotesWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
        close();
    else
        QWidget::keyPressEvent(event);
}

void NotesWindow::closeEvent(QCloseEvent *event)
{
    if (m_saveDelay.isActive()) {
        m_saveDelay.stop();
        save();
    }
    QWidget::closeEvent(event);
}
