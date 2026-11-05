#include "theme.h"
#include "fuzzy.h"

#include <chrome.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QApplication>
#include <QPainter>

#include <cmath>

namespace
{

KConfigGroup config()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Appearance"));
}

// The colours a theme is made of.
struct Colours {
    QColor window, base, text, button, accent;
};

const QHash<QString, Colours> &definitions()
{
    static const QHash<QString, Colours> table{
        {QStringLiteral("light"), {QColor(0xef, 0xf0, 0xf3), Qt::white, QColor(0x1b, 0x1d, 0x22), QColor(0xf7, 0xf7, 0xf9), QColor(0x3d, 0x7e, 0xff)}},
        {QStringLiteral("dark"), {QColor(0x20, 0x22, 0x26), QColor(0x18, 0x19, 0x1c), QColor(0xe6, 0xe7, 0xea), QColor(0x2a, 0x2c, 0x31), QColor(0x4c, 0x8d, 0xff)}},
        {QStringLiteral("midnight"), {QColor(0x12, 0x18, 0x28), QColor(0x0c, 0x11, 0x20), QColor(0xdf, 0xe6, 0xf5), QColor(0x1a, 0x22, 0x36), QColor(0x5a, 0xc8, 0xfa)}},
        {QStringLiteral("paper"), {QColor(0xf3, 0xee, 0xe4), QColor(0xfb, 0xf8, 0xf2), QColor(0x2b, 0x26, 0x22), QColor(0xf7, 0xf2, 0xe8), QColor(0xb5, 0x65, 0x1d)}},
        {QStringLiteral("mocha"), {QColor(0x2a, 0x24, 0x20), QColor(0x21, 0x1c, 0x19), QColor(0xec, 0xe3, 0xda), QColor(0x34, 0x2c, 0x27), QColor(0xd4, 0xa3, 0x73)}},
        {QStringLiteral("forest"), {QColor(0x18, 0x20, 0x1b), QColor(0x12, 0x19, 0x14), QColor(0xe1, 0xeb, 0xe3), QColor(0x21, 0x2b, 0x24), QColor(0x5f, 0xbf, 0x77)}},
    };
    return table;
}

// The palette the system gave Lumen, to return to.
QPalette &systemPalette()
{
    static QPalette palette = QGuiApplication::palette();
    return palette;
}

bool applying = false; // our own palette change, not the system's

// Relative luminance (sRGB), for choosing readable text on an accent.
qreal luminance(const QColor &c)
{
    auto channel = [](qreal v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

} // namespace

namespace Theme
{

QList<Choice> themes()
{
    return {
        {QStringLiteral("system"), QObject::tr("System"), {}},
        {QStringLiteral("light"), QObject::tr("Light"), definitions().value(QStringLiteral("light")).window},
        {QStringLiteral("dark"), QObject::tr("Dark"), definitions().value(QStringLiteral("dark")).window},
        {QStringLiteral("midnight"), QObject::tr("Midnight"), definitions().value(QStringLiteral("midnight")).window},
        {QStringLiteral("paper"), QObject::tr("Paper"), definitions().value(QStringLiteral("paper")).window},
        {QStringLiteral("mocha"), QObject::tr("Mocha"), definitions().value(QStringLiteral("mocha")).window},
        {QStringLiteral("forest"), QObject::tr("Forest"), definitions().value(QStringLiteral("forest")).window},
    };
}

QList<Accent> accents()
{
    return {
        {QStringLiteral("system"), QObject::tr("Theme's own"), {}},
        {QStringLiteral("blue"), QObject::tr("Blue"), QColor(0x3d, 0x7e, 0xff)},
        {QStringLiteral("purple"), QObject::tr("Purple"), QColor(0x9b, 0x6b, 0xff)},
        {QStringLiteral("pink"), QObject::tr("Pink"), QColor(0xff, 0x5f, 0xa2)},
        {QStringLiteral("red"), QObject::tr("Red"), QColor(0xef, 0x4b, 0x4b)},
        {QStringLiteral("orange"), QObject::tr("Orange"), QColor(0xff, 0x8a, 0x3d)},
        {QStringLiteral("yellow"), QObject::tr("Yellow"), QColor(0xe8, 0xb5, 0x00)},
        {QStringLiteral("green"), QObject::tr("Green"), QColor(0x34, 0xc0, 0x6b)},
        {QStringLiteral("graphite"), QObject::tr("Graphite"), QColor(0x8a, 0x8f, 0x98)},
    };
}

QString currentTheme()
{
    return config().readEntry("Theme", QStringLiteral("system"));
}

QString currentAccent()
{
    return config().readEntry("Accent", QStringLiteral("system"));
}

QPalette palette(const QString &theme, const QString &accent, const QPalette &system)
{
    QPalette pal = system;
    if (definitions().contains(theme)) {
        const Colours c = definitions().value(theme);
        const bool dark = c.window.lightness() < 128;
        auto set = [&](QPalette::ColorRole role, const QColor &colour) {
            pal.setColor(QPalette::Active, role, colour);
            pal.setColor(QPalette::Inactive, role, colour);
            pal.setColor(QPalette::Disabled, role, Chrome::mix(colour, c.window, 0.5));
        };
        set(QPalette::Window, c.window);
        set(QPalette::WindowText, c.text);
        set(QPalette::Base, c.base);
        set(QPalette::AlternateBase, Chrome::mix(c.base, c.window, 0.5));
        set(QPalette::Text, c.text);
        set(QPalette::Button, c.button);
        set(QPalette::ButtonText, c.text);
        set(QPalette::ToolTipBase, c.button);
        set(QPalette::ToolTipText, c.text);
        set(QPalette::PlaceholderText, Chrome::mix(c.text, c.base, 0.5));
        set(QPalette::BrightText, dark ? Qt::white : Qt::black);
        set(QPalette::Light, c.button.lighter(dark ? 130 : 108));
        set(QPalette::Midlight, Chrome::mix(c.button, c.text, 0.1));
        set(QPalette::Mid, Chrome::mix(c.window, c.text, 0.25));
        set(QPalette::Dark, Chrome::mix(c.window, c.text, 0.45));
        set(QPalette::Shadow, Qt::black);
        set(QPalette::Highlight, c.accent);
        set(QPalette::Link, c.accent);
    }
    // The accent, over the theme's own (or the system's).
    for (const Accent &a : accents()) {
        if (a.id != accent || !a.colour.isValid())
            continue;
        for (auto group : {QPalette::Active, QPalette::Inactive}) {
            pal.setColor(group, QPalette::Highlight, a.colour);
            pal.setColor(group, QPalette::Link, a.colour);
        }
    }
    // Text on the accent: white on the deep ones, near-black on the bright
    // ones (yellow, the cyan of Midnight, Mocha's tan), where white would
    // wash out.
    const QColor highlight = pal.color(QPalette::Active, QPalette::Highlight);
    const QColor onAccent = luminance(highlight) > 0.36 ? QColor(0x1b, 0x1d, 0x22) : Qt::white;
    for (auto group : {QPalette::Active, QPalette::Inactive})
        pal.setColor(group, QPalette::HighlightedText, onAccent);
    return pal;
}

void apply(const QString &theme, const QString &accent)
{
    KConfigGroup g = config();
    g.writeEntry("Theme", theme);
    g.writeEntry("Accent", accent);
    g.sync();
    applySaved();
}

QIcon themeSwatch(const QString &theme, const QString &accent)
{
    const QPalette pal = palette(theme, accent, QGuiApplication::palette());
    const bool dark = Chrome::isDark(pal);
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    // The card in miniature: its window colour, the well in the base
    // colour, the accent beam along the top and a selected row.
    const QRectF card(5, 5, 54, 54);
    p.setPen(QPen(Chrome::withAlpha(dark ? Qt::white : Qt::black, dark ? 70 : 60), 1.5));
    p.setBrush(pal.color(QPalette::Window));
    p.drawRoundedRect(card, 13, 13);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(QRectF(12, 24, 40, 28), 7, 7);
    p.setBrush(Chrome::withAlpha(pal.color(QPalette::Highlight), dark ? 120 : 90));
    p.drawRoundedRect(QRectF(16, 29, 32, 8), 4, 4);
    p.setBrush(pal.color(QPalette::Highlight));
    p.drawRoundedRect(QRectF(16, 12, 14, 6), 3, 3);
    p.setBrush(Chrome::withAlpha(pal.color(QPalette::WindowText), 90));
    p.drawRoundedRect(QRectF(34, 12, 14, 6), 3, 3);
    return QIcon(pixmap);
}

QIcon accentSwatch(const QString &accent)
{
    const QColor colour = palette(currentTheme(), accent, QGuiApplication::palette()).color(QPalette::Highlight);
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    // A disc of the colour with a soft halo, like a selected row's marker.
    p.setPen(Qt::NoPen);
    p.setBrush(Chrome::withAlpha(colour, 60));
    p.drawEllipse(QPointF(32, 32), 24, 24);
    p.setBrush(colour);
    p.drawEllipse(QPointF(32, 32), 15, 15);
    p.setPen(QPen(Chrome::withAlpha(Qt::white, 110), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(32, 32), 14.5, 14.5);
    return QIcon(pixmap);
}

void applySaved()
{
    if (applying)
        return;
    applying = true;
    QPalette &system = systemPalette();
    // A palette change we did not make is the system's new scheme.
    static QPalette ours;
    if (QGuiApplication::palette() != ours)
        system = QGuiApplication::palette();
    ours = palette(currentTheme(), currentAccent(), system);
    if (QGuiApplication::palette() != ours) // setting it again would announce a change again
        QApplication::setPalette(ours);
    applying = false;
}

} // namespace Theme

void ThemeSource::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    const QString theme = Theme::currentTheme(), accent = Theme::currentAccent();
    int order = 0;
    for (const Theme::Choice &t : Theme::themes()) {
        if (!q.isEmpty() && fuzzyScore(q, t.name) <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("theme:") + t.id;
        item.title = t.name;
        item.subtitle = t.id == QLatin1String("system") ? tr("Follow the system colour scheme") : tr("Theme");
        item.kind = t.id == theme ? tr("Current") : QString();
        item.icon = Theme::themeSwatch(t.id, accent);
        item.score = 1000 - order++;
        const QString id = t.id;
        item.actions << Action{tr("Use Theme"), {}, {}, [this, id, text] {
            Theme::apply(id, Theme::currentAccent());
            query(text);
        }, true};
        items << item;
    }
    for (const Theme::Accent &a : Theme::accents()) {
        if (!q.isEmpty() && fuzzyScore(q, a.name) <= 0 && fuzzyScore(q, QStringLiteral("accent ") + a.name) <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("accent:") + a.id;
        item.title = a.id == QLatin1String("system") ? tr("Accent: Theme's Own") : tr("Accent: %1").arg(a.name);
        item.subtitle = tr("Accent colour");
        item.kind = a.id == accent ? tr("Current") : QString();
        item.icon = Theme::accentSwatch(a.id);
        item.score = 1000 - order++;
        const QString id = a.id;
        item.actions << Action{tr("Use Accent"), {}, {}, [this, id, text] {
            Theme::apply(Theme::currentTheme(), id);
            query(text);
        }, true};
        items << item;
    }
    Q_EMIT resultsReady(items);
}
