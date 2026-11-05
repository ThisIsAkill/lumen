#pragma once

#include <QPalette>
#include <QString>

class QTextDocument;

// Lumen's additions to Quick Look's Chrome: style sheets for the stock Qt
// widgets that sit on the glass, so lists, text views and the notes share
// one look. Everything is derived from the palette, so it follows light
// and dark colour schemes.
namespace Style
{
// The tone of the result well: paper in light schemes, a deeper tone in
// dark ones.
QColor wellTone(const QPalette &palette);

// A thin pill scrollbar without buttons, hugging the right edge of its
// view. Apply it to a view, or to a container to cover every view inside.
// Views on the well pass its tone as `lane`: a text view leaves the strip
// under its scroll bar unpainted, which would show through the glass.
QString scrollBars(const QPalette &palette, const QColor &lane = Qt::transparent);

// A text view that paints no background of its own: the well behind it
// shows through, with the palette's selection colours kept.
QString transparentText(const QPalette &palette);

// Dresses a document made from Markdown (QTextDocument::setMarkdown) in
// Lumen's tones: measured headings, code on a faint tint in the fixed
// font, links in the accent, quiet tables and blockquotes, air between
// paragraphs. Markdown bypasses the document's CSS, so this works on the
// formats themselves.
void styleMarkdown(QTextDocument *document, const QPalette &palette);
} // namespace Style
