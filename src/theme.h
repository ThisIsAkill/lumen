#pragma once

#include "source.h"

#include <QColor>
#include <QIcon>
#include <QPalette>

// Lumen's own look, independent of the system colour scheme: a theme
// (System, Light, Dark, Midnight, Paper, Mocha, Forest) and an accent
// colour. Every part of Lumen draws from the application palette, so a
// theme is a palette. Kept in lumenrc [Appearance].
namespace Theme
{

struct Choice {
    QString id;
    QString name;
    QColor swatch; // its window colour, for previews
};
QList<Choice> themes();
struct Accent {
    QString id;
    QString name;
    QColor colour; // invalid for "system"
};
QList<Accent> accents();

QString currentTheme();
QString currentAccent();

// Icons for choosing: a theme as a small card in its colours (with the
// accent that would apply), an accent as a disc.
QIcon themeSwatch(const QString &theme, const QString &accent);
QIcon accentSwatch(const QString &accent);

// The palette for a theme and accent; "system" keeps those parts of `system`.
QPalette palette(const QString &theme, const QString &accent, const QPalette &system);

// Saves and applies a choice to the whole application.
void apply(const QString &theme, const QString &accent);
// At start-up, and when the system's palette changed under a chosen theme.
void applySaved();

} // namespace Theme

// The "Lumen Theme" view: the themes and accents, applied with Enter.
class ThemeSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Theme"); }
    QString viewPlaceholder() const override { return tr("Search themes and accent colours…"); }
};
