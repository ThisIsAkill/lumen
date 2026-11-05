#include "style.h"

#include <chrome.h>

#include <QFontDatabase>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextTable>

namespace Style
{

QColor wellTone(const QPalette &palette)
{
    return Chrome::isDark(palette) ? Chrome::mix(palette.color(QPalette::Base), Qt::black, 0.08) : palette.color(QPalette::Base);
}

QString scrollBars(const QPalette &palette, const QColor &lane)
{
    const bool dark = Chrome::isDark(palette);
    const QColor handle = Chrome::withAlpha(palette.color(QPalette::WindowText), dark ? 70 : 60);
    const QColor hover = Chrome::withAlpha(palette.color(QPalette::WindowText), dark ? 120 : 110);
    return QStringLiteral("QScrollBar { background: %3; }"
                          "QScrollBar:vertical { width: 10px; margin: 6px 3px 6px 1px; }"
                          "QScrollBar::handle:vertical { background: %1; border-radius: 3px; min-height: 28px; }"
                          "QScrollBar::handle:vertical:hover { background: %2; }"
                          "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
                          "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
                          "QScrollBar:horizontal { height: 10px; margin: 1px 6px 3px 6px; }"
                          "QScrollBar::handle:horizontal { background: %1; border-radius: 3px; min-width: 28px; }"
                          "QScrollBar::handle:horizontal:hover { background: %2; }"
                          "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
                          "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }")
        .arg(handle.name(QColor::HexArgb), hover.name(QColor::HexArgb), lane.name(QColor::HexArgb));
}

QString transparentText(const QPalette &palette)
{
    return QStringLiteral("QPlainTextEdit, QTextBrowser { background: transparent; color: %1; "
                          "selection-background-color: %2; selection-color: %3; }")
        .arg(palette.color(QPalette::Text).name(QColor::HexArgb), palette.color(QPalette::Highlight).name(QColor::HexArgb),
             palette.color(QPalette::HighlightedText).name(QColor::HexArgb));
}

void styleMarkdown(QTextDocument *document, const QPalette &palette)
{
    const bool dark = Chrome::isDark(palette);
    const QColor accent = palette.color(QPalette::Highlight);
    const QColor muted = Chrome::mix(palette.color(QPalette::Text), palette.color(QPalette::Base), 0.35);
    const QColor tint = Chrome::withAlpha(palette.color(QPalette::WindowText), dark ? 30 : 16);
    const QFont base = document->defaultFont();
    QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fixed.setPointSizeF(base.pointSizeF() * 0.92);

    // Tables: no grid, a little padding, the header row on the tint.
    const QList<QTextFrame *> frames = document->rootFrame()->childFrames();
    for (QTextFrame *frame : frames) {
        auto *table = qobject_cast<QTextTable *>(frame);
        if (!table)
            continue;
        // After a blockquote the importer leaves an empty paragraph in the
        // first cell, which pushes its text a line down: join it.
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextCursor first = table->cellAt(row, column).firstCursorPosition();
                if (first.block().text().isEmpty() && first.block().next().isValid() && table->cellAt(first.block().next().position()) == table->cellAt(row, column))
                    first.deleteChar();
            }
        }
        QTextTableFormat format = table->format();
        format.setBorder(0);
        format.setCellSpacing(0);
        format.setCellPadding(5);
        format.setTopMargin(4);
        format.setBottomMargin(8);
        table->setFormat(format);
        // The header row's text in a heavier weight; the cells themselves
        // stay as they are (a cell format shifts the layout).
        for (int column = 0; column < table->columns() && table->rows() > 0; ++column) {
            const QTextTableCell cell = table->cellAt(0, column);
            QTextCursor header = cell.firstCursorPosition();
            header.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
            QTextCharFormat bold;
            bold.setFontWeight(QFont::DemiBold);
            bold.setForeground(muted);
            header.mergeCharFormat(bold);
        }
    }

    bool previousCode = false;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextBlockFormat bf = block.blockFormat();
        const int heading = bf.headingLevel();
        const bool code = bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeLanguage);
        const bool nextCode = block.next().isValid()
            && (block.next().blockFormat().nonBreakableLines() || block.next().blockFormat().hasProperty(QTextFormat::BlockCodeLanguage));
        const bool quote = bf.hasProperty(QTextFormat::BlockQuoteLevel) && bf.intProperty(QTextFormat::BlockQuoteLevel) > 0;
        if (heading) {
            bf.setTopMargin(block == document->begin() ? 0 : (heading == 1 ? 10 : 12));
            bf.setBottomMargin(heading == 1 ? 6 : 4);
        } else if (code) {
            // The lines of a block share one tint, with the fence's air above
            // and below it; the text indent is the padding inside.
            bf.setBackground(tint);
            bf.setTextIndent(8);
            bf.setLineHeight(118, QTextBlockFormat::ProportionalHeight);
            bf.setTopMargin(previousCode ? 0 : 6);
            bf.setBottomMargin(nextCode ? 0 : 8);
        } else if (quote) {
            bf.setLeftMargin(14);
            bf.setBottomMargin(6);
        } else if (!block.textList()) {
            bf.setBottomMargin(6);
        } else {
            bf.setBottomMargin(2);
        }
        cursor.setBlockFormat(bf);
        previousCode = code;

        // Character formats: fragment by fragment, so inline code and links
        // keep their extent.
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat cf = fragment.charFormat();
            QTextCharFormat merge;
            if (heading) {
                const qreal factor = heading == 1 ? 1.45 : heading == 2 ? 1.22 : heading == 3 ? 1.08 : 1.0;
                merge.setFontPointSize(base.pointSizeF() * factor);
                merge.setFontWeight(QFont::DemiBold);
                merge.setProperty(QTextFormat::FontSizeAdjustment, 0); // over the importer's own sizing
            }
            if (cf.fontFixedPitch() || code) {
                merge.setFont(fixed, QTextCharFormat::FontPropertiesSpecifiedOnly);
                merge.setFontFixedPitch(true);
                if (!code)
                    merge.setBackground(tint); // inline code: its own tint
            }
            if (cf.isAnchor()) {
                merge.setForeground(accent);
                merge.setFontUnderline(false);
            }
            if (quote)
                merge.setForeground(muted);
            if (merge.properties().isEmpty())
                continue;
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(merge);
        }
    }
}

} // namespace Style
