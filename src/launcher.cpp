#include "launcher.h"
#include "answersource.h"
#include "apphotkeys.h"
#include "appquit.h"
#include "appsource.h"
#include "browserhistory.h"
#include "builtinsource.h"
#include "calchistory.h"
#include "calendarsource.h"
#include "devsource.h"
#include "downloadsource.h"
#include "emojisource.h"
#include "expansion.h"
#include "scriptcommands.h"
#include "scriptsource.h"
#include "systemsource.h"
#include "timekeeper.h"
#include "theme.h"
#include "timesource.h"
#include "weathersource.h"
#include "windowlayouts.h"
#include "windowsource.h"
#include "clipboardhistory.h"
#include "desktopsource.h"
#include "devices.h"
#include "keepawake.h"
#include "language.h"
#include "network.h"
#include "filesource.h"
#include "focusmode.h"
#include "krunnersource.h"
#include "library.h"
#include "menusearch.h"
#include "monitorsource.h"
#include "resultview.h"
#include "screentools.h"
#include "noteswindow.h"
#include "paster.h"
#include "packages.h"
#include "pastestack.h"
#include "projectsource.h"
#include "previewpane.h"
#include "settingsdialog.h"
#include "style.h"
#include "fuzzy.h"

#include <chrome.h>
#include <panel.h>
#include <viewers.h>

#include <KConfigGroup>
#include <KApplicationTrader>
#include <KGlobalAccel>
#include <KIO/ApplicationLauncherJob>
#include <KSharedConfig>
#include <KIO/OpenUrlJob>
#include <LayerShellQt/Window>

#include <QAction>
#include <QComboBox>
#include <QApplication>
#include <QSet>
#include <QDir>
#include <QCryptographicHash>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

namespace
{

// The card shares Quick Look's chrome: a tinted glass body with a soft
// shadow, an accent wash behind the header and a toned well for the content.
constexpr int Radius = 14;     // card corner radius
constexpr int Shadow = 24;     // transparent margin around the card for the shadow
constexpr int ShadowDrop = 4;  // how far the shadow sits below the card
constexpr int Pad = 10;        // card edge to the result well
constexpr int WellInset = 5;   // well edge to the first row
constexpr int MaxResults = 60;
constexpr int TokenHeight = 32; // an argument field
constexpr int TokenRadius = 9;
constexpr int TokenPad = 10;    // text inset in a token
const QSize CardSize(760, 500);
constexpr int PaneCardWidth = 1080; // the card with the preview pane beside the list
constexpr int CardDrop = 12;        // how far above its place the card starts when it opens

// The hairline of light along the card's top edge: Lumen's mark.
void drawBeam(QPainter &p, const QRectF &body, qreal radius, const QColor &accent, int alpha)
{
    QLinearGradient beam(body.topLeft(), body.topRight());
    beam.setColorAt(0.0, Qt::transparent);
    beam.setColorAt(0.5, Chrome::withAlpha(accent, alpha));
    beam.setColorAt(1.0, Qt::transparent);
    p.save();
    p.setClipRect(QRectF(body.left(), body.top() - 1, body.width(), radius + 2));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(beam), 1.5));
    p.drawPath(Chrome::roundedPath(body.adjusted(1, 1, -1, -1), radius - 1));
    p.restore();
}

// Paints the glass card: shadow, translucent gradient body, outer border
// and the top rim highlight. `headerHeight` is the accent-washed part.
void drawCard(QPainter &p, const QRectF &body, const QPalette &pal, int headerHeight)
{
    const bool dark = Chrome::isDark(pal);
    Chrome::drawShadow(p, body, Radius, Shadow - ShadowDrop, dark ? 130 : 90, ShadowDrop);

    const QColor base = Chrome::withAlpha(Chrome::mix(pal.color(QPalette::Window), pal.color(QPalette::Base), dark ? 0.0 : 0.3), 246);
    QLinearGradient glass(body.topLeft(), body.bottomLeft());
    glass.setColorAt(0.0, Chrome::mix(base, Qt::white, dark ? 0.07 : 0.3));
    glass.setColorAt(1.0, base);
    const QPainterPath shape = Chrome::roundedPath(body, Radius);
    p.fillPath(shape, glass);

    QLinearGradient wash(body.topLeft(), QPointF(body.left(), body.top() + headerHeight));
    wash.setColorAt(0.0, Chrome::withAlpha(pal.color(QPalette::Highlight), dark ? 38 : 26));
    wash.setColorAt(1.0, Qt::transparent);
    p.fillPath(shape, wash);

    p.setBrush(Qt::NoBrush);
    p.setPen(Chrome::withAlpha(dark ? Qt::white : Qt::black, dark ? 36 : 40));
    p.drawPath(shape);
    QLinearGradient rim(body.topLeft(), QPointF(body.left(), body.top() + body.height() * 0.4));
    rim.setColorAt(0.0, Chrome::withAlpha(Qt::white, dark ? 30 : 160));
    rim.setColorAt(1.0, Qt::transparent);
    p.setPen(QPen(QBrush(rim), 1.0));
    p.drawPath(Chrome::roundedPath(body.adjusted(1, 1, -1, -1), Radius - 1));
    drawBeam(p, body, Radius, pal.color(QPalette::Highlight), dark ? 200 : 170);
}

bool onWayland()
{
    return QGuiApplication::platformName() == QLatin1String("wayland");
}

// Pipes data into wl-copy, which keeps serving it after Lumen hides.
void wlCopy(const QByteArray &data, const QString &type)
{
    auto *proc = new QProcess;
    QObject::connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    QStringList args;
    if (!type.isEmpty())
        args << QStringLiteral("--type") << type;
    proc->start(QStringLiteral("wl-copy"), args);
    proc->write(data);
    proc->closeWriteChannel();
}

QString keyText(const QKeySequence &key)
{
    return key.toString(QKeySequence::NativeText);
}

} // namespace

// A small glass pill near the bottom of the screen ("Copied"), used when
// the launcher itself has already closed. It rises into place and sinks
// away again; its margins leave room for the shadow and that movement.
class Hud : public QLabel
{
public:
    Hud()
        : m_reveal(this)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAlignment(Qt::AlignCenter);
        setFont(Chrome::scaledFont(font(), 1.0, QFont::DemiBold));
        setContentsMargins(HudShadow + 22, HudShadow + 9, HudShadow + 22, HudShadow + 9);
        if (onWayland()) {
            create();
            auto *layer = LayerShellQt::Window::get(windowHandle());
            layer->setLayer(LayerShellQt::Window::LayerOverlay);
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
            layer->setAnchors(LayerShellQt::Window::AnchorBottom);
            layer->setMargins({0, 0, 0, 90 - HudShadow});
            layer->setWantsToBeOnActiveScreen(true);
            layer->setScope(QStringLiteral("lumen-hud"));
        }
        m_timer.setSingleShot(true);
        m_timer.setInterval(1400);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] { m_reveal.to(0, Motion::Toast, Motion::exit()); });
        m_reveal.onStep = [this](qreal) { update(); };
        m_reveal.onDone = [this] {
            if (m_reveal.value() <= 0)
                hide();
        };
    }

    void flash(const QString &message)
    {
        setText(message);
        adjustSize();
        if (!onWayland()) {
            const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
            move(screen.center().x() - width() / 2, screen.bottom() - 90 + HudShadow - height());
        }
        if (!isVisible()) {
            show();
            m_reveal.start(0, 1, Motion::Toast, Motion::spring());
        } else if (m_reveal.value() < 1) {
            m_reveal.to(1, Motion::Toast, Motion::spring()); // caught while sinking away
        }
        m_timer.start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // Rising from a little below while it fades in; the reverse going.
        const qreal reveal = m_reveal.value();
        p.setOpacity(reveal);
        p.translate(0, (1 - reveal) * Rise);
        const QPalette pal = palette();
        const bool dark = Chrome::isDark(pal);
        const QRectF body = QRectF(rect()).adjusted(HudShadow + 0.5, HudShadow + 0.5, -HudShadow - 0.5, -HudShadow - 0.5);
        const qreal radius = body.height() / 2;
        Chrome::drawShadow(p, body, radius, HudShadow - 2, dark ? 120 : 80, 2);
        const QColor base = Chrome::withAlpha(Chrome::mix(pal.color(QPalette::Window), pal.color(QPalette::Base), dark ? 0.0 : 0.3), 246);
        p.setPen(Chrome::withAlpha(dark ? Qt::white : Qt::black, dark ? 36 : 40));
        p.setBrush(Chrome::mix(base, Qt::white, dark ? 0.05 : 0.2));
        p.drawRoundedRect(body, radius, radius);
        // Never the active window, so the accent comes from the active
        // group: Breeze fades the inactive highlight.
        drawBeam(p, body, radius, pal.color(QPalette::Active, QPalette::Highlight), dark ? 200 : 170);
        p.setPen(pal.color(QPalette::WindowText));
        p.drawText(body, Qt::AlignCenter, text());
    }

private:
    static constexpr int HudShadow = 16;
    static constexpr int Rise = 8; // how far below its place the pill starts
    QTimer m_timer;
    Tween m_reveal; // 0 hidden → 1 in place
};

// The pill in the search row naming the open view ("Files", "Actions"):
// filled with the accent colour, like Quick Look's main button. While the
// field asks a question ("Alias", "Password") it is outlined instead, so
// the prompt reads as a different state from a view.
//
// It pops in whenever it appears or says something new, so the eye is drawn
// to the row's change of state.
class ViewChip : public QLabel
{
public:
    explicit ViewChip(QWidget *parent)
        : QLabel(parent)
        , m_pop(this)
    {
        setFont(Chrome::scaledFont(font(), 0.92, QFont::DemiBold));
        setContentsMargins(11, 4, 11, 4);
        setAlignment(Qt::AlignCenter);
        m_pop.jump(1);
        m_pop.onStep = [this](qreal) { update(); };
    }

    // Shows the chip with `text`, popping in when that is a change.
    void present(const QString &text, bool outlined)
    {
        const bool changed = !isVisibleTo(parentWidget()) || text != this->text() || outlined != m_outlined;
        setText(text);
        m_outlined = outlined;
        show();
        if (changed && parentWidget()->isVisible())
            m_pop.start(0, 1, Motion::Fade, Motion::spring());
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal pop = m_pop.value();
        p.setOpacity(pop);
        const QPointF c = QRectF(rect()).center();
        p.translate(c);
        p.scale(Motion::lerp(0.8, 1.0, pop), Motion::lerp(0.8, 1.0, pop));
        p.translate(-c);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QPalette pal = palette();
        const bool dark = Chrome::isDark(pal);
        const QColor accent = pal.color(QPalette::Highlight);
        if (m_outlined) {
            p.setPen(QPen(Chrome::withAlpha(accent, dark ? 200 : 170), 1.2));
            p.setBrush(Chrome::withAlpha(accent, dark ? 46 : 30));
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
            p.setPen(dark ? accent.lighter(135) : accent.darker(125));
        } else {
            p.setPen(Chrome::withAlpha(Qt::white, dark ? 50 : 90));
            p.setBrush(accent);
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
            p.setPen(pal.color(QPalette::HighlightedText));
        }
        p.drawText(r, Qt::AlignCenter, text());
    }

private:
    bool m_outlined = false;
    Tween m_pop;
};

// A magnifier glyph drawn by hand in the accent colour, so it matches in
// every icon theme. It sits at the left of the search row. While the field
// asks for text it becomes a speech bubble, while it waits for a key
// combination a key cap, and while a command's fields are filled in a
// small form, so the row says what it is doing. One glyph morphs into the
// next: the old shrinks away as the new grows into place.
class SearchGlyph : public QWidget
{
public:
    enum Mode { Search, Ask, Listen, Fill };

    explicit SearchGlyph(QWidget *parent)
        : QWidget(parent)
        , m_morph(this)
    {
        setFixedSize(26, 26);
        m_morph.jump(1);
        m_morph.onStep = [this](qreal) { update(); };
    }

    void setMode(Mode mode)
    {
        if (mode == m_mode)
            return;
        m_previous = m_mode;
        m_mode = mode;
        if (isVisible())
            m_morph.start(0, 1, Motion::Fade, Motion::spring());
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal t = m_morph.value();
        if (t < 1) {
            // The glyph before, on its way out.
            p.save();
            drawGlyph(p, m_previous, 1 - t, Motion::lerp(1.0, 0.6, t));
            p.restore();
        }
        drawGlyph(p, m_mode, t, Motion::lerp(0.6, 1.0, t));
    }

private:
    // One glyph at `opacity`, scaled about the centre.
    void drawGlyph(QPainter &p, Mode mode, qreal opacity, qreal scale)
    {
        p.setOpacity(opacity);
        const QPointF c(13.0, 13.0);
        p.translate(c);
        p.scale(scale, scale);
        p.translate(-c);
        const QColor accent = palette().color(QPalette::Highlight);
        p.setPen(QPen(accent, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (mode == Ask) {
            // A speech bubble with a dot of text in it.
            QPainterPath bubble;
            bubble.addRoundedRect(QRectF(3.5, 4.5, 17, 13), 4.5, 4.5);
            bubble.moveTo(7.5, 17.5);
            bubble.lineTo(6.0, 22.0);
            bubble.lineTo(11.5, 17.5);
            p.drawPath(bubble);
            p.setPen(Qt::NoPen);
            p.setBrush(Chrome::withAlpha(accent, 150));
            p.drawEllipse(QPointF(12.0, 11.0), 1.6, 1.6);
            return;
        }
        if (mode == Listen) {
            // A key cap with its dot: "press a key".
            p.drawRoundedRect(QRectF(3.5, 6.5, 19, 14), 4, 4);
            p.setPen(Qt::NoPen);
            p.setBrush(Chrome::withAlpha(accent, 150));
            p.drawRoundedRect(QRectF(8.5, 11.5, 9, 2.6), 1.3, 1.3);
            return;
        }
        if (mode == Fill) {
            // Two fields of a form, the first with a cursor in it.
            p.drawRoundedRect(QRectF(3.5, 4.5, 19, 7), 3, 3);
            p.drawRoundedRect(QRectF(3.5, 14.5, 19, 7), 3, 3);
            p.setPen(Qt::NoPen);
            p.setBrush(Chrome::withAlpha(accent, 150));
            p.drawRoundedRect(QRectF(7.0, 6.5, 2.0, 3.0), 1.0, 1.0);
            return;
        }
        const QPointF lens(11.0, 11.0);
        const qreal radius = 6.5;
        p.drawEllipse(lens, radius, radius);
        const QPointF dir(0.7071, 0.7071);
        p.drawLine(lens + dir * (radius + 1.0), lens + dir * (radius + 6.5));
        // A spark inside the lens: the "lumen".
        p.setPen(Qt::NoPen);
        p.setBrush(Chrome::withAlpha(accent, 150));
        p.drawEllipse(lens + QPointF(-1.5, -1.5), 1.6, 1.6);
    }

    Mode m_mode = Search;
    Mode m_previous = Search;
    Tween m_morph; // 0 → 1 from the previous glyph to the current
};

// The footer's right side: "Open ↵   Actions Ctrl K", each shortcut drawn
// as key caps. When one of the shortcuts is used, its caps are pressed and
// released: the footer answers the hand.
class KeyHints : public QWidget
{
public:
    struct Hint {
        QString label;
        QString keys;
    };

    explicit KeyHints(QWidget *parent)
        : QWidget(parent)
        , m_press(this)
    {
        setFont(Chrome::scaledFont(font(), 0.9));
        m_press.onStep = [this](qreal) { update(); };
    }

    // The caps for `keys` go down and come back up.
    void press(const QString &keys)
    {
        if (!isVisible())
            return;
        for (const Hint &h : std::as_const(m_hints)) {
            if (h.keys == keys) {
                m_pressed = keys;
                m_press.start(1, 0, Motion::Press, Motion::exit());
                return;
            }
        }
    }

    void setHints(const QList<Hint> &hints)
    {
        m_hints = hints;
        const QFontMetrics fm(font());
        int width = 0;
        for (const Hint &h : m_hints)
            width += (width ? Gap : 0) + fm.horizontalAdvance(h.label) + LabelGap + KeyCaps::width(font(), h.keys);
        m_width = width;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override { return QSize(m_width, KeyCaps::height(font()) + 4); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setFont(font());
        const QFontMetrics fm(font());
        int x = width() - m_width;
        for (const Hint &h : m_hints) {
            const int labelWidth = fm.horizontalAdvance(h.label);
            p.setFont(font());
            p.setPen(Chrome::mutedText(palette()));
            p.drawText(QRect(x, 0, labelWidth, height()), Qt::AlignVCenter | Qt::AlignLeft, h.label);
            x += labelWidth + LabelGap;
            const qreal pressed = h.keys == m_pressed && m_press.isRunning() ? m_press.value() : 0;
            x += KeyCaps::draw(p, QRect(x, 0, width() - x, height()), h.keys, palette(), Qt::AlignLeft, pressed) + Gap;
        }
    }

private:
    static constexpr int Gap = 16;     // between hints
    static constexpr int LabelGap = 6; // between a label and its caps
    QList<Hint> m_hints;
    int m_width = 0;
    QString m_pressed; // the hint whose caps are down
    Tween m_press;     // 1 fully pressed → 0 released
};

// A field's token: a rounded, slightly inset tint on the glass, ringed
// and washed with the accent while it has the focus, so the fields of a
// command read as parts of the search row rather than stock widgets.
void drawToken(QPainter &p, const QRect &rect, const QPalette &pal, bool focused)
{
    p.setRenderHint(QPainter::Antialiasing);
    const bool dark = Chrome::isDark(pal);
    const QColor accent = pal.color(QPalette::Highlight);
    const QRectF r = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    if (focused) {
        p.setPen(QPen(Chrome::withAlpha(accent, dark ? 210 : 180), 1.4));
        p.setBrush(Chrome::withAlpha(accent, dark ? 44 : 26));
    } else {
        p.setPen(QPen(Chrome::withAlpha(pal.color(QPalette::WindowText), dark ? 60 : 44), 1.0));
        p.setBrush(Chrome::withAlpha(pal.color(QPalette::Base), dark ? 70 : 150));
    }
    p.drawRoundedRect(r, TokenRadius, TokenRadius);
}

// A text argument: a line edit painted as a token, its label as the
// placeholder; a secret one shows dots.
class ArgumentField : public QLineEdit
{
public:
    explicit ArgumentField(QWidget *parent)
        : QLineEdit(parent)
    {
        setFrame(false);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setFixedHeight(TokenHeight);
        setMinimumWidth(120);
        setMaximumWidth(280);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void restyle()
    {
        // The bar's palette: the style sheet below rewrites this field's own.
        const QPalette pal = parentWidget()->palette();
        setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; color: %1; placeholder-text-color: %2; "
                                     "padding: 0 %3px; selection-background-color: %4; selection-color: %5; }")
                          .arg(pal.color(QPalette::WindowText).name(QColor::HexArgb), Chrome::mutedText(pal).name(QColor::HexArgb))
                          .arg(TokenPad)
                          .arg(pal.color(QPalette::Highlight).name(QColor::HexArgb), pal.color(QPalette::HighlightedText).name(QColor::HexArgb)));
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        {
            QPainter p(this);
            drawToken(p, rect(), parentWidget()->palette(), hasFocus()); // the style sheet blanks this field's Base
        }
        QLineEdit::paintEvent(event); // the text, over the token
    }
    void focusInEvent(QFocusEvent *event) override
    {
        QLineEdit::focusInEvent(event);
        update();
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        QLineEdit::focusOutEvent(event);
        update();
    }
};

// A choice argument: a combo box painted as a token with its label in
// muted text before the chosen option and a chevron after it. The list
// that drops down is the platform's.
class ArgumentChoice : public QComboBox
{
public:
    explicit ArgumentChoice(const QString &label, QWidget *parent)
        : QComboBox(parent)
        , m_label(label)
    {
        setFrame(false);
        setFixedHeight(TokenHeight);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        int longest = 0;
        for (int i = 0; i < count(); ++i)
            longest = qMax(longest, fm.horizontalAdvance(itemText(i)));
        return QSize(2 * TokenPad + fm.horizontalAdvance(m_label) + LabelGap + longest + ChevronRoom, TokenHeight);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QPalette pal = palette();
        drawToken(p, rect(), pal, hasFocus());
        p.setFont(font());
        const QFontMetrics fm(font());
        QRect text = rect().adjusted(TokenPad, 0, -TokenPad - ChevronRoom, 0);
        p.setPen(Chrome::mutedText(pal));
        p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, m_label);
        text.setLeft(text.left() + fm.horizontalAdvance(m_label) + LabelGap);
        p.setPen(pal.color(QPalette::WindowText));
        p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(currentText(), Qt::ElideRight, text.width()));
        // The chevron, in the accent while focused.
        const QPointF c(rect().right() - TokenPad - 4.5, rect().center().y() + 0.5);
        p.setPen(QPen(hasFocus() ? pal.color(QPalette::Highlight) : Chrome::mutedText(pal), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{c + QPointF(-4, -2), c + QPointF(0, 2), c + QPointF(4, -2)});
    }
    void focusInEvent(QFocusEvent *event) override
    {
        QComboBox::focusInEvent(event);
        update();
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        QComboBox::focusOutEvent(event);
        update();
    }

private:
    static constexpr int LabelGap = 7;
    static constexpr int ChevronRoom = 18;
    QString m_label;
};

// The fields a command's arguments are typed into, beside the search
// field: a line for text (dotted for secrets) or a list for a choice.
class ArgumentBar : public QWidget
{
public:
    explicit ArgumentBar(QWidget *parent)
        : QWidget(parent)
        , m_layout(new QHBoxLayout(this))
        , m_reveal(this)
    {
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(8);
        hide();
        // The fields drop into the row together, through a pose effect
        // that goes once they have landed.
        m_reveal.onStep = [this](qreal t) {
            if (auto *effect = dynamic_cast<PoseEffect *>(graphicsEffect()))
                effect->setPose(Motion::appearing(t, 0.96, -6));
        };
        m_reveal.onDone = [this] { setGraphicsEffect(nullptr); };
    }

    void setArguments(const QList<Argument> &arguments, const QStringList &initial, QObject *keyFilter)
    {
        clear();
        m_arguments = arguments;
        for (int i = 0; i < arguments.size(); ++i) {
            const Argument &a = arguments.at(i);
            QWidget *field;
            if (!a.options.isEmpty()) {
                auto *box = new ArgumentChoice(a.label(), this);
                box->addItems(a.options);
                const int chosen = std::max(a.options.indexOf(initial.value(i)), a.options.indexOf(a.defaultValue));
                box->setCurrentIndex(std::max(chosen, 0));
                field = box;
            } else {
                auto *line = new ArgumentField(this);
                line->setPlaceholderText(a.optional ? tr("%1 (optional)").arg(a.label()) : a.label());
                line->setEchoMode(a.secret ? QLineEdit::Password : QLineEdit::Normal);
                line->setText(initial.value(i));
                field = line;
            }
            field->setObjectName(QStringLiteral("argument"));
            field->setFont(Chrome::scaledFont(font(), 1.15));
            field->installEventFilter(keyFilter);
            m_layout->addWidget(field, a.options.isEmpty() ? 1 : 0);
            m_fields << field;
        }
        m_layout->addStretch(0);
        restyle();
        show();
        if (Motion::enabled() && parentWidget()->isVisible()) {
            setGraphicsEffect(new PoseEffect(this));
            m_reveal.start(0, 1, Motion::Open, QEasingCurve::Linear); // the pose shapes the curve
        }
        // Start in the first field still empty.
        int first = 0;
        while (first < m_fields.size() - 1 && !value(first).isEmpty() && arguments.at(first).options.isEmpty())
            ++first;
        focusField(first);
    }

    // The fields' colours follow the palette (a theme change).
    void restyle()
    {
        for (QWidget *field : std::as_const(m_fields)) {
            if (auto *line = dynamic_cast<ArgumentField *>(field)) // no Q_OBJECT: a plain cast
                line->restyle();
            field->update();
        }
    }

    void clear()
    {
        m_reveal.stop();
        setGraphicsEffect(nullptr);
        qDeleteAll(m_fields);
        m_fields.clear();
        while (QLayoutItem *item = m_layout->takeAt(0))
            delete item;
        m_arguments.clear();
        hide();
    }

    bool owns(QObject *object) const { return m_fields.contains(object); }
    int current() const
    {
        for (int i = 0; i < m_fields.size(); ++i)
            if (m_fields.at(i)->hasFocus())
                return i;
        return 0;
    }
    void focusField(int index)
    {
        if (QWidget *field = m_fields.value(std::clamp(index, 0, int(m_fields.size()) - 1)))
            field->setFocus();
    }
    int count() const { return int(m_fields.size()); }

    // What was typed (or chosen) in field `index`.
    QString value(int index) const
    {
        if (auto *box = qobject_cast<QComboBox *>(m_fields.value(index)))
            return m_arguments.at(index).valueAt(box->currentIndex());
        if (auto *line = qobject_cast<QLineEdit *>(m_fields.value(index)))
            return line->text();
        return {};
    }
    QStringList values() const
    {
        QStringList out;
        for (int i = 0; i < m_fields.size(); ++i)
            out << (value(i).isEmpty() ? m_arguments.at(i).defaultValue : value(i));
        return out;
    }
    // The first required argument left empty, or -1.
    int missing() const
    {
        for (int i = 0; i < m_fields.size(); ++i)
            if (!m_arguments.at(i).optional && value(i).trimmed().isEmpty())
                return i;
        return -1;
    }
    QString label(int index) const { return m_arguments.value(index).label(); }

private:
    QHBoxLayout *m_layout;
    QList<QWidget *> m_fields;
    QList<Argument> m_arguments;
    Tween m_reveal;
};

// The footer's left side: the name of what is open ("Lumen", "Files", the
// result whose actions are listed), or for a moment a message ("Copied to
// Clipboard"). A message rises in over the name and the name comes back
// the same way, so the footer is seen to answer.
class StatusLine : public QWidget
{
public:
    explicit StatusLine(QWidget *parent)
        : QWidget(parent)
        , m_flip(this)
    {
        setFont(Chrome::scaledFont(font(), 0.9, QFont::Medium));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_flip.jump(1);
        m_flip.onStep = [this](qreal) { update(); };
    }

    QSize sizeHint() const override { return QSize(100, QFontMetrics(font()).height()); }

    // The name of what is open; replaces a message only once it is over.
    void setName(const QString &name)
    {
        m_name = name;
        if (!m_toast)
            flipTo(name);
    }

    // A message for a moment, then the name again.
    void toast(const QString &message)
    {
        m_toast = true;
        flipTo(message);
        QTimer::singleShot(1800, this, [this] {
            m_toast = false;
            flipTo(m_name);
        });
    }
    bool isToasting() const { return m_toast; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setFont(font());
        const QPalette pal = QGuiApplication::palette(); // the card's tones, whatever the style sheets around
        p.setPen(Chrome::mutedText(pal));
        const QFontMetrics fm(font());
        const qreal t = m_flip.value();
        if (t < 1) {
            p.setOpacity(1 - t);
            p.drawText(QRectF(0, -t * Rise, width(), height()), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(m_previous, Qt::ElideRight, width()));
        }
        p.setOpacity(t);
        p.drawText(QRectF(0, (1 - t) * Rise, width(), height()), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    static constexpr int Rise = 6; // the new line rises this far, the old one moves on by as much

    void flipTo(const QString &text)
    {
        if (text == m_text)
            return;
        m_previous = m_text;
        m_text = text;
        if (isVisible())
            m_flip.start(0, 1, Motion::Toast, Motion::entrance());
        else
            m_flip.jump(1);
        update();
    }

    QString m_name;
    QString m_text;     // what is shown (or arriving)
    QString m_previous; // what is on its way out
    bool m_toast = false;
    Tween m_flip; // 0 → 1 from the previous line to the current
};

Launcher::Launcher()
    : m_library(new Library(this))
    , m_clipboard(new ClipboardHistory(this))
    , m_card(new QWidget(this))
    , m_glyph(new SearchGlyph(m_card))
    , m_chip(new ViewChip(m_card))
    , m_args(new ArgumentBar(m_card))
    , m_search(new QLineEdit(m_card))
    , m_list(new ResultList(m_card))
    , m_model(new ResultModel(this))
    , m_status(new StatusLine(m_card))
    , m_hints(new KeyHints(m_card))
    , m_toggleAction(new QAction(tr("Show Lumen"), this))
    , m_hud(new Hud)
    , m_opening(this)
    , m_cardWidth(this)
    , m_glow(this)
{
    setWindowTitle(QStringLiteral("Lumen"));
    Theme::applySaved();
    m_builtin = new BuiltinSource(this, m_library);
    m_system = new SystemSource(this);
    m_timekeeper = new Timekeeper(this);
    m_time = new TimeSource(this, m_timekeeper);
    m_calendar = new CalendarSource(this);
    m_focus = new FocusMode(this, m_timekeeper);
    m_awake = new KeepAwake(this);
    m_rootSources = {new KRunnerSource(this), m_builtin, new AnswerSource(this), m_system, new ScriptSource(this, m_library), new ScriptCommandSource(this), new WindowLayoutSource(this, m_library), m_focus, new BrowserHistory(this, false), new PackageRootSource(this), new AppQuitSource(this, false), m_awake, new LanguageSource(this), new NetworkSource(this), new DisplaySource(this), new PhoneSource(this), new AppSource(this), m_time, m_calendar, new WeatherSource(this), new DevSource(this), new ProjectSource(this), new DesktopSource(this), new ScreenTools(this), new MonitorSource(this)};
    m_pasteStack = new PasteStack(this, m_clipboard);
    m_views = {
        {PasteStackView, new PasteStackSource(this, m_pasteStack)},
        {MenusView, new MenuSource(this)},
        {TabsView, new BrowserTabsSource(this)},
        {HistoryView, new BrowserHistory(this, true)},
        {CalculationsView, new CalcHistory(this)},
        {ThemeView, new ThemeSource(this)},
        {PackagesView, new PackageSource(this, new PackageManager(this))},
        {AppsView, new AppQuitSource(this, true)},
        {ClipboardView, new ClipboardSource(this, m_clipboard)},
        {FilesView, new FileSource(this)},
        {SnippetsView, new SnippetsSource(this, m_builtin)},
        {EmojiView, new EmojiSource(this)},
        {WindowsView, new WindowSource(this)},
        {TransformView, new TransformSource(this)},
        {ProcessesView, new ProcessSource(this)},
        {DownloadsView, new DownloadSource(this)},
    };
    m_rootSources << new DownloadRootSource(this, static_cast<DownloadSource *>(m_views.value(DownloadsView)));
    for (Source *s : m_rootSources + m_views.values())
        connectSource(s);
    for (Source *s : m_rootSources + m_views.values())
        s->setParent(this);
    connect(m_library, &Library::changed, this, [this] {
        if (isVisible())
            textChanged(m_search->text());
    });

    // Layout: [glyph][chip][search field] / results well / [status .. hints]
    m_search->setFont(Chrome::scaledFont(m_search->font(), 1.5));
    m_search->setFrame(false);
    m_search->setPlaceholderText(tr("Search apps, commands, files…"));
    m_search->installEventFilter(this);
    m_search->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_chip->hide();
    connect(m_search, &QLineEdit::textEdited, this, [this] { m_reselectId.clear(); });
    connect(m_search, &QLineEdit::textChanged, this, &Launcher::textChanged);

    m_list->setModel(m_model);
    m_list->setItemDelegate(new ResultDelegate(m_list));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setDragEnabled(true); // file results can be dragged into other apps
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(false); // the first fallback row carries its caption band
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->viewport()->setAutoFillBackground(false);
    m_list->setAttribute(Qt::WA_TranslucentBackground);
    connect(m_list, &QListView::clicked, this, [this] { runSelected(0); });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, &Launcher::selectionChanged);
    // The empty state is painted on the card behind the (empty) list.
    connect(m_model, &QAbstractItemModel::modelReset, this, qOverload<>(&QWidget::update));

    // The card's motion. While it opens, the card's widgets are drawn
    // through a pose effect and the card's body with the same pose (see
    // paintEvent); the effect goes once the card has settled.
    m_opening.onStep = [this](qreal t) {
        m_pose = Motion::appearing(t, 0.94, -CardDrop);
        if (auto *effect = dynamic_cast<PoseEffect *>(m_card->graphicsEffect()))
            effect->setPose(m_pose);
        update();
    };
    m_opening.onDone = [this] {
        m_card->setGraphicsEffect(nullptr); // effects slow painting down; only while animating
        m_pose = Motion::Pose();
        update();
    };
    m_cardWidth.jump(CardSize.width());
    m_cardWidth.onStep = [this](qreal) {
        layoutCard();
        update();
    };
    m_glow.onStep = [this](qreal) { update(); };

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(Pad + 10, 12, Pad + 12, 10);
    searchRow->setSpacing(12);
    searchRow->addWidget(m_glyph);
    searchRow->addWidget(m_chip);
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(m_args, 1);
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(Pad + 10, 8, Pad + 8, 9);
    footer->addWidget(m_status, 1);
    footer->addWidget(m_hints);
    auto *column = new QVBoxLayout(m_card);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addLayout(searchRow);
    auto *well = new QHBoxLayout;
    well->setContentsMargins(Pad, WellInset, Pad, WellInset);
    well->addWidget(m_list, 11);
    m_pane = new PreviewPane(m_card);
    m_pane->hide();
    well->addWidget(m_pane, 9);
    column->addLayout(well, 1);
    column->addLayout(footer);

    restyle();
    setupWindow();
    setupShortcut();
    m_paster = new Paster(this);
    m_expansion = new Expansion(this);
    m_hotkeys = new AppHotkeys(this);
    m_pendingTimeout = new QTimer(this);
    m_pendingTimeout->setSingleShot(true);
    m_pendingTimeout->setInterval(3000);
    connect(m_pendingTimeout, &QTimer::timeout, this, [this] {
        m_pendingRunId.clear();
        notify(tr("That favourite is not available right now"));
    });

    // On Wayland the preview is drawn inside the launcher's full-screen
    // surface, so it reliably appears above the card; on X11 it is a
    // separate window that stays on top.
    m_preview = m_overlay ? new Panel(Panel::Embedded, this) : new Panel(Panel::Overlay);
    m_preview->hide();
    connect(m_preview, &Panel::visibilityChanged, this, &Launcher::updateFooter);
    // Load the media backend and image plugins once Lumen is idle, so the
    // first preview opens instantly.
    QTimer::singleShot(2000, this, &prewarmViewers);
}

Launcher::~Launcher()
{
    // The sources are deleted with the launcher's children, after its own
    // members: they must not report back to it any more.
    for (Source *source : std::as_const(m_connected))
        source->disconnect(this);
    if (!m_preview->parent())
        delete m_preview;
    delete m_notes;
    delete m_hud;
}

void Launcher::setupWindow()
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlag(Qt::FramelessWindowHint);
    m_overlay = onWayland();
    if (m_overlay) {
        // A transparent surface over the whole screen, so a click outside
        // the card can close the launcher; the card sits in its upper part.
        create();
        auto *layer = LayerShellQt::Window::get(windowHandle());
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                                                        | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layer->setExclusiveZone(-1);
        layer->setWantsToBeOnActiveScreen(true);
        layer->setScope(QStringLiteral("lumen"));
    } else {
        setWindowFlags(windowFlags() | Qt::Tool | Qt::WindowStaysOnTopHint);
        resize(CardSize.grownBy(QMargins(Shadow, Shadow, Shadow, Shadow)));
    }
}

void Launcher::restyle()
{
    const QPalette pal = palette();
    const QColor muted = Chrome::mutedText(pal);
    // Transparent field with a muted placeholder; the card paints the glass.
    m_search->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; "
                                           "placeholder-text-color: %2; padding: 0; }")
                                .arg(pal.color(QPalette::WindowText).name(), muted.name()));
    m_status->update();
    m_args->restyle();
    m_pane->restyle();
    // A thin pill scrollbar without buttons, hugging the well's right edge.
    m_list->setStyleSheet(QStringLiteral("QListView { background: transparent; }") + Style::scrollBars(pal, Style::wellTone(pal)));
}

void Launcher::setupShortcut()
{
    m_toggleAction->setObjectName(QStringLiteral("toggle"));
    connect(m_toggleAction, &QAction::triggered, this, &Launcher::toggle);
    const QList<QKeySequence> defaultKeys{QKeySequence(Qt::META | Qt::Key_Space)};
    KGlobalAccel::self()->setDefaultShortcut(m_toggleAction, defaultKeys);
    KGlobalAccel::self()->setShortcut(m_toggleAction, defaultKeys); // keeps a shortcut the user has changed

    // Direct shortcuts for the views; no keys by default, assignable in
    // System Settings → Keyboard → Shortcuts → Lumen.
    const QList<std::pair<QString, QString>> views{
        {QStringLiteral("clipboard"), tr("Clipboard History")},
        {QStringLiteral("files"), tr("Search Files")},
        {QStringLiteral("snippets"), tr("Search Snippets")},
        {QStringLiteral("emoji"), tr("Search Emoji")},
        {QStringLiteral("windows"), tr("Switch Window")},
        {QStringLiteral("menu"), tr("Search Menu Items")},
        {QStringLiteral("tabs"), tr("Search Browser Tabs")},
        {QStringLiteral("notes"), tr("Notes")},
    };
    for (const auto &[name, text] : views) {
        auto *action = new QAction(text, this);
        action->setObjectName(QStringLiteral("view-") + name);
        connect(action, &QAction::triggered, this, [this, name] { showView(name, {}); });
        KGlobalAccel::self()->setShortcut(action, {});
    }
    // Paste the clipboard without its formatting, from any app.
    auto *plain = new QAction(tr("Paste as Plain Text"), this);
    plain->setObjectName(QStringLiteral("paste-plain"));
    connect(plain, &QAction::triggered, this, &Launcher::pastePlainText);
    KGlobalAccel::self()->setShortcut(plain, {});
}

bool Launcher::showView(const QString &name, const QString &query)
{
    if (name == QLatin1String("notes")) {
        openNotes();
        return true;
    }
    static const QHash<QString, View> names{
        {QStringLiteral("clipboard"), ClipboardView},
        {QStringLiteral("files"), FilesView},
        {QStringLiteral("snippets"), SnippetsView},
        {QStringLiteral("emoji"), EmojiView},
        {QStringLiteral("windows"), WindowsView},
        {QStringLiteral("transform"), TransformView},
        {QStringLiteral("processes"), ProcessesView},
        {QStringLiteral("downloads"), DownloadsView},
        {QStringLiteral("paste-stack"), PasteStackView},
        {QStringLiteral("menu"), MenusView},
        {QStringLiteral("tabs"), TabsView},
        {QStringLiteral("history"), HistoryView},
        {QStringLiteral("calculations"), CalculationsView},
        {QStringLiteral("theme"), ThemeView},
        {QStringLiteral("packages"), PackagesView},
        {QStringLiteral("apps"), AppsView},
    };
    if (!names.contains(name))
        return false;
    if (!isVisible())
        showWithQuery({});
    openView(names.value(name), query);
    return true;
}

// ------------------------------------------------------------- showing

void Launcher::toggle()
{
    if (isVisible()) {
        hideLauncher();
        return;
    }
    // Reopened soon after closing: back where it was, the text selected so
    // typing replaces it.
    const int resume = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("General")).readEntry("ResumeSeconds", 90);
    const bool recent = m_closedAt.isValid() && m_closedAt.secsTo(QDateTime::currentDateTime()) < resume;
    const View view = m_views.key(m_closedView, RootView);
    if (recent && (view != RootView || !m_closedQuery.isEmpty())) {
        const QString query = m_closedQuery;
        showWithQuery(view == RootView ? query : QString());
        if (view != RootView)
            openView(view, query);
        m_search->selectAll();
        return;
    }
    showWithQuery({});
}

void Launcher::showWithQuery(const QString &query)
{
    for (Source *s : m_rootSources + m_views.values())
        s->sessionStarted();
    if (m_view || m_actionsShown || m_prompt) {
        // Already open somewhere else: back to the main search.
        m_actionsShown = false;
        if (m_prompt)
            endPrompt();
        openView(RootView);
    }
    m_search->setText(query);
    textChanged(query);
    const bool opening = !isVisible();
    show();
    raise();
    activateWindow();
    m_search->setFocus();
    if (opening)
        animateIn();
}

// The card springs open: a little smaller, a little higher and transparent
// at first, it drops into place, a touch past full size, and settles. The
// field takes keys from the first frame. KDE's animation speed setting
// scales it, and "instant" turns it off.
void Launcher::animateIn()
{
    if (!Motion::enabled()) {
        m_pose = Motion::Pose();
        return;
    }
    m_card->setGraphicsEffect(new PoseEffect(m_card));
    m_opening.start(0, 1, Motion::Open, QEasingCurve::Linear); // the pose shapes the curve
}

void Launcher::hideLauncher()
{
    m_preview->hide();
    if (!isVisible())
        return;
    m_closedAt = QDateTime::currentDateTime();
    m_closedView = m_views.key(m_view, RootView) == RootView ? nullptr : m_view;
    m_closedQuery = m_prompt || m_actionsShown ? m_savedQuery : m_search->text();
    // The window goes at once (focus and keys return to the previous app
    // immediately); a picture of the card shrinks away in its place.
    m_opening.stop();
    m_card->setGraphicsEffect(nullptr);
    m_pose = Motion::Pose();
    Ghost::dismiss(this, m_card->geometry().marginsAdded(QMargins(Shadow, Shadow, Shadow, Shadow + ShadowDrop)) & rect());
    hide();
    for (Source *s : m_rootSources + m_views.values())
        s->sessionEnded();
    m_actionsShown = false;
    if (m_prompt)
        endPrompt();
    m_view = nullptr;
    dropViews();
    updatePane();
    m_chip->hide();
    m_search->setPlaceholderText(tr("Search apps, commands, files…"));
    const QSignalBlocker block(m_search);
    m_search->clear();
    m_rootResults.clear();
    m_model->setItems({});
}

void Launcher::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_overlay) {
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        move(screen.center().x() - width() / 2, screen.top() + screen.height() / 5);
    }
    layoutCard();
}

void Launcher::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutCard(); // on Wayland the compositor sends the screen size after showing
}

void Launcher::layoutCard()
{
    if (!m_overlay) {
        m_card->setGeometry(rect().marginsRemoved(QMargins(Shadow, Shadow, Shadow, Shadow)));
        return;
    }
    const QSize card(qMin(qRound(m_cardWidth.value()), width() * 9 / 10), qMin(CardSize.height(), height() * 8 / 10));
    m_card->setGeometry((width() - card.width()) / 2, height() / 5, card.width(), card.height());
}

void Launcher::updatePane()
{
    const bool show = m_view && m_view->hasPreviewPane() && !m_actionsShown;
    if (show != m_pane->isVisibleTo(m_card)) {
        m_pane->setVisible(show);
        const int width = show ? PaneCardWidth : CardSize.width();
        if (!m_overlay) {
            // A window of its own: it takes the new size at once.
            m_cardWidth.jump(width);
            resize(QSize(width, CardSize.height()).grownBy(QMargins(Shadow, Shadow, Shadow, Shadow)));
        } else if (isVisible()) {
            m_cardWidth.to(width, Motion::Resize, Motion::entrance()); // the card eases wider or narrower
        } else {
            m_cardWidth.jump(width);
        }
        layoutCard();
        update();
    }
    if (show)
        m_pane->showItem(selectedItem());
}

void Launcher::mousePressEvent(QMouseEvent *event)
{
    if (m_overlay && !m_card->geometry().contains(event->position().toPoint()))
        hideLauncher();
    else
        QWidget::mousePressEvent(event);
}

void Launcher::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange)
        Theme::applySaved(); // the system scheme changed under a chosen theme
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        restyle();
    // On X11 the launcher is a normal window: close it when it loses focus.
    if (!m_overlay && event->type() == QEvent::ActivationChange && isVisible() && !isActiveWindow()) {
        // Lumen's own badge and messages are not meant to take focus; some
        // platforms give it to them anyway. Take it back rather than close.
        if (const QWidget *active = QApplication::activeWindow(); active && active->testAttribute(Qt::WA_ShowWithoutActivating)) {
            activateWindow();
            return;
        }
        hideLauncher();
    }
}

void Launcher::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const bool dark = Chrome::isDark(pal);
    const QRect cardRect = m_card->geometry();
    // While the card opens, its body takes the same pose as its widgets
    // (scaled about the centre, shifted, faded), so the two stay one card.
    if (m_pose.scale != 1 || m_pose.dy != 0 || m_pose.opacity != 1) {
        const QPointF centre = QRectF(cardRect).center();
        p.setOpacity(m_pose.opacity);
        p.translate(centre.x(), centre.y() + m_pose.dy);
        p.scale(m_pose.scale, m_pose.scale);
        p.translate(-centre);
    }
    const QRectF body = QRectF(cardRect).adjusted(0.5, 0.5, -0.5, -0.5);
    QRect content = m_list->geometry();
    if (m_pane->isVisibleTo(m_card))
        content |= m_pane->geometry();
    const QRect listRect = content.translated(cardRect.topLeft()).adjusted(0, -WellInset, 0, WellInset);
    drawCard(p, body, pal, listRect.top() - cardRect.top() + Pad);

    if (m_prompt && !m_args->isVisibleTo(m_card)) {
        // The field is asking something: an accent-tinted outline around
        // the search row marks it as the place to answer, glowing up as the
        // question is put. (Argument fields are tokens that ring themselves.)
        const qreal glow = m_glow.value();
        const QRect row = (m_glyph->geometry() | m_search->geometry()).translated(cardRect.topLeft()).adjusted(-9, -5, 9, 5);
        const QColor accent = pal.color(QPalette::Highlight);
        p.setPen(QPen(Chrome::withAlpha(accent, qRound((dark ? 150 : 120) * glow)), 1.0));
        p.setBrush(Chrome::withAlpha(accent, qRound((dark ? 30 : 18) * glow)));
        const qreal spread = (1 - glow) * 3; // closes in on the row as it brightens
        p.drawRoundedRect(QRectF(row).adjusted(0.5 - spread, 0.5 - spread, -0.5 + spread, -0.5 + spread), 11 + spread, 11 + spread);
    }

    // The result well: a toned stage inset in the card, like Quick Look's
    // content area. Light schemes get paper, dark ones a deeper tone.
    const QRectF well = QRectF(listRect).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(Chrome::withAlpha(Qt::black, dark ? 90 : 22));
    p.setBrush(Chrome::withAlpha(Style::wellTone(pal), dark ? 215 : 235));
    p.drawRoundedRect(well, Chrome::WellRadius, Chrome::WellRadius);
    if (m_pane->isVisibleTo(m_card)) {
        // A hairline between the results and the preview.
        const int x = cardRect.left() + m_pane->geometry().left();
        p.setPen(Chrome::withAlpha(pal.color(QPalette::WindowText), 30));
        p.drawLine(QPointF(x - 0.5, listRect.top() + 12), QPointF(x - 0.5, listRect.bottom() - 12));
    }

    if (m_model->rowCount() == 0) {
        // Empty state, in the well behind the (empty) list.
        const QString text = m_search->text().trimmed().isEmpty() ? tr("Nothing here yet") : tr("No results");
        const QString detail = m_actionsShown ? tr("No action matches") : m_view ? tr("Try a different search") : tr("Try a different search, or a calculation");
        QRect textArea = listRect.adjusted(0, 0, 0, -listRect.height() / 2);
        p.setFont(Chrome::scaledFont(font(), 1.15, QFont::DemiBold));
        p.setPen(Chrome::mix(pal.color(QPalette::Text), pal.color(QPalette::Base), 0.35));
        p.drawText(textArea, Qt::AlignHCenter | Qt::AlignBottom, text);
        textArea.translate(0, textArea.height());
        p.setFont(Chrome::scaledFont(font(), 0.95));
        p.setPen(Chrome::mix(pal.color(QPalette::Text), pal.color(QPalette::Base), 0.55));
        p.drawText(textArea.adjusted(0, 6, 0, 0), Qt::AlignHCenter | Qt::AlignTop, detail);
    }
}

// -------------------------------------------------------------- search

void Launcher::setQuery(const QString &query)
{
    m_search->setText(query);
    m_search->setCursorPosition(int(query.size()));
}

void Launcher::openView(View view, const QString &query)
{
    dropViews();
    m_forward.clear(); // somewhere new: nothing ahead any more
    revealList(view == RootView ? ResultList::FromLeft : ResultList::FromRight);
    setView(m_views.value(view), query);
}

void Launcher::pushView(Source *source, const QString &query)
{
    if (!source->parent()) {
        source->setParent(this);
        m_ownedViews.insert(source);
    }
    connectSource(source);
    m_forward.clear();
    revealList(ResultList::FromRight);
    if (m_view)
        m_viewStack.append({m_view, m_search->text()});
    else
        m_viewStack.append({nullptr, m_search->text()}); // the main search
    source->sessionStarted();
    setView(source, query);
}

void Launcher::connectSource(Source *source)
{
    if (m_connected.contains(source))
        return;
    m_connected.insert(source);
    connect(source, &Source::resultsReady, this, [this, source](const QList<Item> &items) { sourceResults(source, items); });
    connect(source, &QObject::destroyed, this, [this, source] {
        m_connected.remove(source);
        m_ownedViews.remove(source);
    });
}

void Launcher::dropViews()
{
    for (const auto &[source, query] : std::as_const(m_viewStack))
        if (source && m_ownedViews.contains(source) && source != m_view)
            source->deleteLater();
    if (m_view && m_ownedViews.contains(m_view)) {
        m_view->deleteLater();
        m_view = nullptr;
    }
    m_viewStack.clear();
}

bool Launcher::goBack()
{
    if (!m_view)
        return false;
    Source *left = m_view;
    const QString leftQuery = m_search->text();
    if (m_viewStack.isEmpty()) {
        openView(RootView);
    } else {
        const auto [below, query] = m_viewStack.takeLast();
        revealList(ResultList::FromLeft);
        setView(below, query);
    }
    // Pushed views are deleted on leaving; the others can be returned to.
    if (m_ownedViews.contains(left))
        left->deleteLater();
    else
        m_forward.append({left, leftQuery});
    return true;
}

void Launcher::goForward()
{
    if (m_forward.isEmpty())
        return;
    const auto [view, query] = m_forward.takeLast();
    if (m_view)
        m_viewStack.append({m_view, m_search->text()});
    revealList(ResultList::FromRight);
    setView(view, query);
}

// The next results slide in from the side the navigation went, unless the
// card is still opening (one motion at a time).
void Launcher::revealList(int how)
{
    if (isVisible() && !m_opening.isRunning())
        m_list->revealNext(ResultList::Reveal(how));
}

void Launcher::setView(Source *source, const QString &query)
{
    m_view = source;
    m_actionsShown = false;
    if (m_view)
        m_chip->present(m_view->viewTitle(), false);
    else
        m_chip->hide();
    m_search->setPlaceholderText(m_view ? m_view->viewPlaceholder() : tr("Search apps, commands, files…"));
    const QSignalBlocker block(m_search);
    m_search->setText(query);
    textChanged(query);
    updatePane();
}

void Launcher::textChanged(const QString &text)
{
    m_userMoved = !m_reselectId.isEmpty();
    if (m_actionsShown) {
        QList<Item> rows;
        for (const Action &a : m_actionsItem.actions) {
            if (!text.isEmpty() && fuzzyScore(text, a.text) <= 0)
                continue;
            Item row;
            row.title = a.text;
            row.kind = keyText(a.shortcut);
            row.keyKind = true;
            row.icon = QIcon::fromTheme(a.iconName.isEmpty() ? QStringLiteral("go-next") : a.iconName);
            row.actions << a;
            rows << row;
        }
        if (rows.size() == m_actionsItem.actions.size() && !rows.isEmpty())
            rows.first().kind = QStringLiteral("↵");
        showItems(rows);
        return;
    }
    if (m_prompt)
        return; // the field holds an answer being typed
    if (m_view) {
        m_view->query(text);
        return;
    }
    // An alias searches for its result's title and puts that result first.
    QString search = text;
    m_aliasId.clear();
    for (const Library::Alias &a : m_library->aliases()) {
        if (text.trimmed().compare(a.alias, Qt::CaseInsensitive) == 0) {
            search = a.title;
            m_aliasId = a.id;
        }
    }
    for (Source *s : m_rootSources)
        s->query(search);
    showRootResults();
}

void Launcher::sourceResults(Source *source, const QList<Item> &items)
{
    if (!m_pendingRunId.isEmpty() && m_rootSources.contains(source)) {
        for (const Item &item : items) {
            if (item.id == m_pendingRunId && !item.actions.isEmpty()) {
                const Item found = item;
                m_pendingRunId.clear();
                m_pendingTimeout->stop();
                runAction(found, found.actions.first());
                return;
            }
        }
    }
    if (m_actionsShown || m_prompt)
        return;
    if (source == m_view) {
        showItems(items);
    } else if (!m_view && m_rootSources.contains(source)) {
        m_rootResults.insert(source, items);
        showRootResults();
    }
}

void Launcher::showRootResults()
{
    QList<Item> all;
    if (m_search->text().trimmed().isEmpty()) {
        // Start screen: Lumen's commands, most used first.
        all = m_builtin->allCommands();
        for (Item &item : all)
            item.score = m_usage.boost(item.id);
        // Timers, the next meeting, what is playing, favourites, then commands.
        all = m_focus->startItems() + m_awake->startItems() + m_time->startItems() + m_calendar->startItems()
            + static_cast<PasteStackSource *>(m_views.value(PasteStackView))->startItems() + m_system->startItems() + favouriteItems() + all;
    } else {
        for (auto it = m_rootResults.cbegin(); it != m_rootResults.cend(); ++it)
            all << it.value();
        for (Item &item : all)
            item.score += m_usage.boost(item.id);
    }
    // Computed first: it also tells personalActions() what can be a fallback.
    const QString searched = m_search->text().trimmed();
    const QList<Item> fallbacks = searched.isEmpty() ? QList<Item>() : fallbackItems(searched);

    // Aliases: the result an alias brings up goes first; every result with
    // an alias shows it. Results that can be kept get the personal actions.
    QHash<QString, QString> aliasOf;
    for (const Library::Alias &a : m_library->aliases())
        aliasOf.insert(a.id, a.alias);
    for (Item &item : all) {
        if (!m_aliasId.isEmpty() && item.id == m_aliasId)
            item.score = 500;
        if (const auto it = aliasOf.constFind(item.id); it != aliasOf.constEnd())
            item.kind = item.kind.isEmpty() ? *it : item.kind + QStringLiteral(" · ") + *it;
        if (item.id.startsWith(QLatin1String("app:")) || item.kind.startsWith(QLatin1String("Applications")))
            if (const KService::Ptr app = serviceFor(item))
                if (const QKeySequence keys = m_hotkeys->keysFor(app->storageId()); !keys.isEmpty())
                    item.kind += QStringLiteral(" · ") + keys.toString(QKeySequence::NativeText);
        if (!item.id.startsWith(QLatin1String("favourite:")))
            item.actions << personalActions(item);
    }

    // A typo match for an app that KRunner found anyway is a duplicate.
    QSet<QString> titles;
    for (const Item &item : std::as_const(all))
        if (!item.id.startsWith(QLatin1String("app:")))
            titles.insert(item.title.toLower());
    all.removeIf([&](const Item &item) { return item.id.startsWith(QLatin1String("app:")) && titles.contains(item.title.toLower()); });

    // Lumen's own answers replace KRunner's simpler versions of them.
    static const QHash<QString, QString> replaces{
        {QStringLiteral("answer:colour"), QStringLiteral("Colors")},
        {QStringLiteral("answer:time"), QStringLiteral("Date and Time")},
    };
    QStringList hiddenKinds;
    for (const Item &item : std::as_const(all))
        if (replaces.contains(item.id))
            hiddenKinds << replaces.value(item.id);
    // Lumen reads browser history itself, from every browser.
    if (std::any_of(all.cbegin(), all.cend(), [](const Item &item) { return item.id.startsWith(QLatin1String("history:")); }))
        hiddenKinds << QStringLiteral("Browser History");
    all.removeIf([&](const Item &item) { return hiddenKinds.contains(item.kind); });

    std::stable_sort(all.begin(), all.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    if (all.size() > MaxResults)
        all.resize(MaxResults);
    // The fallback commands close the list, in the user's order.
    all << fallbacks;
    m_model->setSearchText(searched);
    showItems(all);
}

QList<Item> Launcher::fallbackChoices() const
{
    QList<Item> items;
    for (const Source *s : m_rootSources)
        items << s->fallbackItems(QStringLiteral("…"));
    return items;
}

QList<Item> Launcher::fallbackItems(const QString &text)
{
    QHash<QString, Item> available;
    for (const Source *s : std::as_const(m_rootSources))
        for (const Item &item : s->fallbackItems(text))
            available.insert(item.id, item);
    m_fallbackIds = QSet<QString>(available.keyBegin(), available.keyEnd());
    QList<Item> items;
    for (const QString &id : m_library->fallbacks()) {
        if (!available.contains(id))
            continue;
        Item item = available.value(id);
        item.id = QStringLiteral("fallback:") + id;
        item.kind = tr("Fallback");
        item.score = 0;
        items << item;
    }
    return items;
}

void Launcher::showItems(const QList<Item> &items)
{
    // Keep a selection the user made on the same result when results are
    // refined; otherwise the best result is selected.
    const Item *current = selectedItem();
    const QString selectedId = !m_reselectId.isEmpty() ? m_reselectId : current && m_userMoved ? current->id : QString();
    m_model->setItems(items);
    int row = 0;
    for (int i = 0; i < items.size() && !selectedId.isEmpty(); ++i) {
        if (items.at(i).id == selectedId) {
            row = i;
            m_reselectId.clear();
        }
    }
    if (!items.isEmpty())
        m_list->setCurrentIndex(m_model->index(row));
    updateFooter();
}

// ------------------------------------------------------------- actions

const Item *Launcher::selectedItem() const
{
    return m_model->itemAt(m_list->currentIndex().row());
}

void Launcher::showActions(bool show)
{
    if (show) {
        const Item *item = selectedItem();
        if (!item || item->actions.isEmpty())
            return;
        m_actionsItem = *item;
        m_savedQuery = m_search->text();
        m_actionsShown = true;
        m_chip->present(tr("Actions"), false);
        m_search->setPlaceholderText(tr("Search actions for “%1”…").arg(item->title));
        const QSignalBlocker block(m_search);
        m_search->clear();
        revealList(ResultList::FromBelow); // the actions pop up over the results
        textChanged({});
    } else {
        m_actionsShown = false;
        if (m_view)
            m_chip->present(m_view->viewTitle(), false);
        else
            m_chip->hide();
        m_search->setPlaceholderText(m_view ? m_view->viewPlaceholder() : tr("Search apps, commands, files…"));
        // Return to the result whose actions were shown, once the (possibly
        // asynchronous) results are back.
        m_reselectId = m_actionsItem.id;
        const QSignalBlocker block(m_search);
        m_search->setText(m_savedQuery);
        revealList(ResultList::FadeIn);
        textChanged(m_savedQuery);
    }
    updatePane();
}

void Launcher::runAction(const Item &item, const Action &action)
{
    if (!action.run)
        return;
    m_usage.record(item.id);
    if (!m_view && !m_actionsShown && CalcHistory::isCalculation(item.id, item.kind))
        CalcHistory::record(m_search->text(), item.title);
    const auto run = action.run; // the item may be replaced while it runs
    if (action.keepOpen) {
        if (m_actionsShown)
            showActions(false);
        run();
    } else {
        run(); // while focused, so launched apps may take focus
        hideLauncher();
        m_closedAt = {}; // done: the next opening starts afresh
    }
}

void Launcher::runSelected(int actionIndex)
{
    const Item *item = selectedItem();
    if (!item || actionIndex >= item->actions.size())
        return;
    if (m_actionsShown)
        runAction(m_actionsItem, item->actions.first());
    else
        runAction(*item, item->actions.at(actionIndex));
}

// While a preview is open it follows the selection, like Quick Look in a
// file manager; it closes on a result that is not a local file.
void Launcher::selectionChanged()
{
    updatePane();
    if (m_preview->isVisible()) {
        const Item *item = selectedItem();
        if (item && !item->filePath.isEmpty())
            m_preview->showFile(item->filePath);
        else
            m_preview->dismiss();
    }
    updateFooter();
}

void Launcher::moveSelection(int delta)
{
    const int count = m_model->rowCount();
    if (count == 0)
        return;
    const int row = std::clamp(m_list->currentIndex().row() + delta, 0, count - 1);
    m_list->setCurrentIndex(m_model->index(row));
    m_userMoved = true;
}

void Launcher::updateFooter()
{
    const Item *item = selectedItem();
    m_status->setName(m_actionsShown ? m_actionsItem.title : m_view ? m_view->viewTitle() : QStringLiteral("Lumen"));
    if (m_status->isToasting())
        return;

    QList<KeyHints::Hint> hints;
    if (m_prompt) {
        // Answering a question: Enter answers (a key combination answers
        // by itself), Esc goes back to the search.
        if (m_prompt->onKeys)
            m_status->setName(tr("Waiting for a key combination…"));
        else if (m_prompt->onArguments && m_args->count() > 1)
            hints << KeyHints::Hint{tr("Next"), QStringLiteral("Tab")} << KeyHints::Hint{tr("Run"), QStringLiteral("↵")};
        else
            hints << KeyHints::Hint{m_prompt->onArguments ? tr("Run") : tr("Done"), QStringLiteral("↵")};
        hints << KeyHints::Hint{tr("Cancel"), QStringLiteral("Escape")};
        m_hints->setHints(hints);
        return;
    }
    if (item && !item->filePath.isEmpty() && !m_actionsShown)
        hints << KeyHints::Hint{m_preview->isVisible() ? tr("Close Preview") : tr("Preview"),
                                m_userMoved || m_preview->isVisible() ? tr("Space") : keyText(QKeySequence(Qt::CTRL | Qt::Key_Y))};
    if (item && !item->actions.isEmpty()) {
        hints << KeyHints::Hint{m_actionsShown ? item->title : item->actions.first().text, QStringLiteral("↵")};
        if (!m_actionsShown && item->actions.size() > 1)
            hints << KeyHints::Hint{tr("Actions"), keyText(QKeySequence(Qt::CTRL | Qt::Key_K))};
    }
    m_hints->setHints(hints);
}

void Launcher::notify(const QString &message)
{
    if (!isVisible()) {
        m_hud->flash(message);
        return;
    }
    m_status->toast(message);
}

// The footer's key cap for a shortcut just used goes down and comes up.
void Launcher::pressHint(const QString &keys)
{
    m_hints->press(keys);
}

// ---------------------------------------------------------------- keys

bool Launcher::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_search || m_args->owns(watched)) && event->type() == QEvent::KeyPress)
        return handleKey(static_cast<QKeyEvent *>(event));
    return QWidget::eventFilter(watched, event);
}

bool Launcher::handleKey(QKeyEvent *event)
{
    if (m_prompt && m_prompt->onArguments)
        return handleArgumentKey(event);
    const QKeySequence pressed(event->keyCombination());

    // A result's own shortcuts (Ctrl+P Pin, Ctrl+Return Show in Folder, ...).
    if (const Item *item = selectedItem(); item && !m_actionsShown) {
        for (const Action &a : item->actions) {
            if (!a.shortcut.isEmpty() && a.shortcut == pressed) {
                runAction(*item, a);
                return true;
            }
        }
    }

    // Ctrl+Y previews any local file, also while typing.
    if (pressed == QKeySequence(Qt::CTRL | Qt::Key_Y)) {
        if (const Item *item = selectedItem(); item && !item->filePath.isEmpty() && !m_actionsShown) {
            pressHint(keyText(pressed));
            quickLook(item->filePath);
        }
        return true;
    }

    // Space previews the selected file once the user has moved to it with the
    // arrow keys (before that it types a space), and closes an open preview.
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        if (m_preview->isVisible()) {
            pressHint(tr("Space"));
            m_preview->dismiss();
            return true;
        }
        const Item *item = selectedItem();
        if (m_userMoved && !m_actionsShown && item && !item->filePath.isEmpty()) {
            pressHint(tr("Space"));
            quickLook(item->filePath);
            return true;
        }
        return false;
    }

    if (m_prompt) {
        if (event->key() == Qt::Key_Escape) {
            pressHint(QStringLiteral("Escape"));
            endPrompt();
            setQuery(m_savedQuery);
            return true;
        }
        if (m_prompt->onKeys) {
            // Wait for a real key; modifiers alone only start the combination.
            if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Control || event->key() == Qt::Key_Alt
                || event->key() == Qt::Key_Meta || event->key() == Qt::Key_Super_L || event->key() == Qt::Key_Super_R)
                return true;
            const auto done = m_prompt->onKeys;
            endPrompt();
            setQuery(m_savedQuery);
            done(pressed);
            return true;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            pressHint(QStringLiteral("↵"));
            const QString answer = m_search->text();
            const auto done = m_prompt->onText;
            endPrompt();
            setQuery(m_savedQuery);
            done(answer);
            return true;
        }
        return false;
    }

    switch (event->key()) {
    case Qt::Key_Down:
        moveSelection(1);
        return true;
    case Qt::Key_Up:
        moveSelection(-1);
        return true;
    case Qt::Key_PageDown:
        moveSelection(8);
        return true;
    case Qt::Key_PageUp:
        moveSelection(-8);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        pressHint(QStringLiteral("↵"));
        runSelected(0);
        return true;
    case Qt::Key_Tab:
        // A command with arguments asks for them; otherwise complete the
        // search to the selected result's name.
        if (const Item *item = selectedItem(); item && !m_actionsShown && !item->actions.isEmpty() && item->actions.first().asksArguments)
            runSelected(0);
        else if (item && !m_actionsShown && !item->title.isEmpty())
            setQuery(item->title);
        return true;
    case Qt::Key_K:
        if (event->modifiers() == Qt::ControlModifier) {
            pressHint(keyText(pressed));
            showActions(!m_actionsShown);
            return true;
        }
        break;
    case Qt::Key_BracketLeft: // Ctrl+[ back, Ctrl+] forward, as in Raycast
        if (event->modifiers() == Qt::ControlModifier && !m_actionsShown) {
            goBack();
            return true;
        }
        break;
    case Qt::Key_BracketRight:
        if (event->modifiers() == Qt::ControlModifier && !m_actionsShown) {
            goForward();
            return true;
        }
        break;
    case Qt::Key_Escape:
        if (m_preview->isVisible())
            m_preview->dismiss();
        else if (m_actionsShown)
            showActions(false);
        else if (!goBack())
            hideLauncher();
        return true;
    case Qt::Key_Backspace:
        if (m_search->text().isEmpty() && (m_actionsShown || m_view)) {
            if (m_actionsShown)
                showActions(false);
            else
                goBack();
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

// ------------------------------------------------------------- helpers

void Launcher::setClipboardText(const QString &text)
{
    if (onWayland() && !QStandardPaths::findExecutable(QStringLiteral("wl-copy")).isEmpty())
        wlCopy(text.toUtf8(), {});
    else
        QGuiApplication::clipboard()->setText(text);
}

void Launcher::setClipboardImage(const QString &pngPath)
{
    QFile f(pngPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    if (onWayland() && !QStandardPaths::findExecutable(QStringLiteral("wl-copy")).isEmpty())
        wlCopy(f.readAll(), QStringLiteral("image/png"));
    else
        QGuiApplication::clipboard()->setImage(QImage(pngPath));
}

void Launcher::copyText(const QString &text)
{
    setClipboardText(text);
    m_list->pulse();
    notify(tr("Copied to Clipboard"));
}

void Launcher::copyImage(const QString &pngPath)
{
    setClipboardImage(pngPath);
    notify(tr("Copied Image"));
}

bool Launcher::canPaste() const
{
    const KConfigGroup general = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("General"));
    return general.readEntry("PasteDirectly", true) && m_paster->isAvailable();
}

void Launcher::pasteText(const QString &text, int cursorBack)
{
    setClipboardText(text);
    // Once Lumen is gone and focus is back in the previous window.
    runAfterHide([this, cursorBack] {
        m_paster->paste();
        if (cursorBack > 0)
            QTimer::singleShot(80, this, [this, cursorBack] { m_paster->left(cursorBack); });
    });
}

void Launcher::pasteNow(const QString &text)
{
    setClipboardText(text);
    QTimer::singleShot(60, this, [this] { m_paster->paste(); }); // once the clipboard is served
}

void Launcher::pasteImageNow(const QString &pngPath)
{
    setClipboardImage(pngPath);
    QTimer::singleShot(60, this, [this] { m_paster->paste(); });
}

void Launcher::pastePlainText()
{
    // Taking the text alone and copying it back drops the formatting.
    auto use = [this](const QString &text) {
        if (text.isEmpty()) {
            notify(tr("Nothing to paste"));
            return;
        }
        if (isVisible())
            pasteText(text);
        else
            pasteNow(text);
    };
    if (onWayland() && !QStandardPaths::findExecutable(QStringLiteral("wl-paste")).isEmpty()) {
        auto *proc = new QProcess(this);
        connect(proc, &QProcess::finished, this, [proc, use] {
            proc->deleteLater();
            use(QString::fromUtf8(proc->readAllStandardOutput()));
        });
        proc->start(QStringLiteral("wl-paste"), {QStringLiteral("--no-newline"), QStringLiteral("--type"), QStringLiteral("text/plain")});
        QTimer::singleShot(2000, proc, [proc] { proc->kill(); });
    } else {
        use(QGuiApplication::clipboard()->text());
    }
}

QString Launcher::recentClipboardText(int n) const
{
    for (const ClipboardHistory::Entry &e : m_clipboard->entries())
        if (!e.text.isEmpty() && n-- == 0)
            return e.text;
    return n == 0 ? QGuiApplication::clipboard()->text() : QString();
}

void Launcher::useSnippet(const QString &name, const QString &body, SnippetUse use, int typed)
{
    auto finish = [this, body, use, typed](const QHash<QString, QString> &values) {
        const Library::Expanded snippet = Library::expandSnippet(body, values, [this](int n) { return recentClipboardText(n); });
        switch (use) {
        case SnippetUse::Paste:
            pasteText(snippet.text, snippet.cursorBack);
            break;
        case SnippetUse::Copy:
            copyText(snippet.text);
            hideLauncher();
            break;
        case SnippetUse::ReplaceTyped:
            if (isVisible()) {
                // Asked for its arguments: back to the app first.
                runAfterHide([this, typed, snippet] { replaceTypedText(typed, snippet.text, snippet.cursorBack); });
            } else {
                replaceTypedText(typed, snippet.text, snippet.cursorBack);
            }
            break;
        }
    };
    const QList<Argument> arguments = Arguments::fromTemplate(body);
    if (arguments.isEmpty()) {
        finish({});
        return;
    }
    Item shown;
    shown.title = name;
    shown.subtitle = body.section(u'\n', 0, 0).left(80);
    shown.icon = QIcon::fromTheme(QStringLiteral("insert-text"));
    if (!isVisible())
        showWithQuery({}); // typed in another app: ask in Lumen
    askArguments(shown, arguments, {}, [arguments, finish](const QStringList &values) { finish(Arguments::byName(arguments, values)); });
}

void Launcher::replaceTypedText(int count, const QString &text, int cursorBack)
{
    if (!m_paster->isAvailable()) {
        copyText(text); // cannot type here: at least have it ready to paste
        return;
    }
    // What was on the clipboard before, to put back once the snippet is in.
    QString previous;
    for (const ClipboardHistory::Entry &e : m_clipboard->entries()) {
        if (!e.text.isEmpty()) {
            previous = e.text;
            break;
        }
    }
    m_paster->backspace(count);
    setClipboardText(text);
    QTimer::singleShot(80, this, [this, cursorBack] {
        m_paster->paste();
        if (cursorBack > 0)
            QTimer::singleShot(80, this, [this, cursorBack] { m_paster->left(cursorBack); });
    });
    if (!previous.isEmpty())
        QTimer::singleShot(700, this, [this, previous] { setClipboardText(previous); });
}

void Launcher::pasteImage(const QString &pngPath)
{
    setClipboardImage(pngPath);
    runAfterHide([this] { m_paster->paste(); });
}

void Launcher::copyUrls(const QList<QUrl> &urls)
{
    if (onWayland() && !QStandardPaths::findExecutable(QStringLiteral("wl-copy")).isEmpty()) {
        QByteArray list;
        for (const QUrl &u : urls)
            list += u.toEncoded() + "\r\n";
        wlCopy(list, QStringLiteral("text/uri-list"));
    } else {
        auto *data = new QMimeData;
        data->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(data);
    }
    notify(tr("Copied File"));
}

void Launcher::quickLook(const QString &path)
{
    if (m_preview->isVisible() && m_preview->currentFile() == path) {
        m_preview->dismiss();
    } else {
        m_preview->showFile(path);
        m_preview->raise();
    }
}

void Launcher::runAfterHide(std::function<void()> work)
{
    hideLauncher();
    m_closedAt = {}; // something ran: start afresh next time
    // Give the compositor a moment to hand focus back to the previous window.
    QTimer::singleShot(150, this, std::move(work));
}

void Launcher::openSettings(SettingsTab tab, bool addNew)
{
    if (!m_settings)
        m_settings = new SettingsDialog(this);
    m_settings->showTab(tab, addNew);
    hideLauncher();
}

// ----------------------------------------------------- favourites, aliases

QList<Action> Launcher::personalActions(const Item &item) const
{
    // Answers, file search shortcuts and lines of command output change with
    // every query; they cannot be kept.
    for (const char *prefix : {"answer:", "lumen:files-for", "cmd-line:", "ext-result:", "clip:", "file:", "fallback:"})
        if (item.id.isEmpty() || item.id.startsWith(QLatin1String(prefix)))
            return {};
    auto self = const_cast<Launcher *>(this);
    const bool favourite = std::any_of(m_library->favourites().cbegin(), m_library->favourites().cend(),
                                       [&](const Library::Favourite &f) { return f.id == item.id; });
    QList<Action> actions{
        {favourite ? tr("Remove from Favourites") : tr("Add to Favourites"), QStringLiteral("starred-symbolic"),
         QKeySequence(Qt::CTRL | Qt::Key_D), [self, item] { self->toggleFavourite(item); }, true},
        {tr("Set Alias…"), QStringLiteral("tag"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
         [self, item] { self->startAliasInput(item); }, true},
    };
    // Commands that take text can be offered for any search.
    if (m_fallbackIds.contains(item.id)) {
        const bool fallback = m_library->fallbacks().contains(item.id);
        actions << Action{fallback ? tr("Remove from Fallbacks") : tr("Use as Fallback"), QStringLiteral("go-bottom"), {}, [self, item, fallback] {
            QStringList ids = self->m_library->fallbacks();
            if (fallback)
                ids.removeAll(item.id);
            else
                ids << item.id;
            self->m_library->setFallbacks(ids);
            self->notify(fallback ? tr("Removed from Fallbacks") : tr("Offered for every search"));
        }, true};
    }
    // Apps can get a global hotkey.
    if (const KService::Ptr app = serviceFor(item)) {
        actions << Action{tr("Set Hotkey…"), QStringLiteral("input-keyboard"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H), [self, item, app] {
            self->askForKeys(tr("Hotkey"), tr("Press the keys that should bring up %1 (Esc to cancel)").arg(app->name()), item,
                             [self, app](const QKeySequence &keys) {
                                 QString conflict;
                                 if (self->m_hotkeys->set(app, keys, &conflict))
                                     self->notify(tr("%1 opens %2").arg(keys.toString(QKeySequence::NativeText), app->name()));
                                 else
                                     self->notify(tr("%1 is already used by “%2”").arg(keys.toString(QKeySequence::NativeText), conflict));
                             });
        }, true};
        if (!m_hotkeys->keysFor(app->storageId()).isEmpty())
            actions << Action{tr("Remove Hotkey"), QStringLiteral("edit-delete"), {}, [self, app] {
                self->m_hotkeys->remove(app->storageId());
                self->notify(tr("Hotkey removed"));
            }, true};
    }
    return actions;
}

// The desktop entry behind an app result (Lumen's own or KRunner's).
KService::Ptr Launcher::serviceFor(const Item &item) const
{
    if (item.id.startsWith(QLatin1String("app:")))
        return KService::serviceByStorageId(item.id.mid(4));
    if (item.kind.section(QStringLiteral(" · "), 0, 0) != QLatin1String("Applications"))
        return {};
    const KService::List apps = KApplicationTrader::query([&](const KService::Ptr &s) { return !s->noDisplay() && s->name() == item.title; });
    return apps.isEmpty() ? KService::Ptr() : apps.first();
}

void Launcher::openUrl(const QUrl &url)
{
    (new KIO::OpenUrlJob(url))->start();
}

void Launcher::launchApp(const QString &storageId)
{
    if (const KService::Ptr app = KService::serviceByStorageId(storageId))
        (new KIO::ApplicationLauncherJob(app))->start();
}

void Launcher::toggleFavourite(const Item &item)
{
    QList<Library::Favourite> favourites = m_library->favourites();
    const auto it = std::find_if(favourites.begin(), favourites.end(), [&](const Library::Favourite &f) { return f.id == item.id; });
    m_list->pulse(); // the row answers either way
    if (it != favourites.end()) {
        QFile::remove(QDir(m_library->folder() + QStringLiteral("/icons")).filePath(it->iconFile));
        favourites.erase(it);
        m_library->setFavourites(favourites);
        notify(tr("Removed from Favourites"));
        return;
    }
    // Keep the icon as an image: KRunner's icons cannot be looked up later.
    // Stored by name in the library folder, so they sync with it.
    const QString dir = m_library->folder() + QStringLiteral("/icons");
    QDir().mkpath(dir);
    const QString iconFile = QString::fromLatin1(QCryptographicHash::hash(item.id.toUtf8(), QCryptographicHash::Sha1).toHex()) + QStringLiteral(".png");
    item.icon.pixmap(64).save(dir + u'/' + iconFile);
    QString kind = item.kind.section(QStringLiteral(" · "), 0, 0);
    favourites << Library::Favourite{item.id, item.title, item.subtitle, kind, iconFile};
    m_library->setFavourites(favourites);
    notify(tr("Added to Favourites"));
}

QList<Item> Launcher::favouriteItems()
{
    QList<Item> items;
    const QList<Library::Favourite> favourites = m_library->favourites();
    for (int i = 0; i < favourites.size(); ++i) {
        const Library::Favourite f = favourites.at(i);
        Item item;
        item.id = QStringLiteral("favourite:") + f.id;
        item.title = f.title;
        item.subtitle = f.subtitle;
        item.kind = f.kind.isEmpty() ? tr("Favourite") : tr("Favourite · %1").arg(f.kind);
        item.icon = QIcon(QDir(m_library->folder() + QStringLiteral("/icons")).filePath(f.iconFile));
        item.score = 90 - i * 0.01; // the order they were added in
        Item original;
        original.id = f.id;
        item.actions << Action{tr("Open"), {}, {}, [this, f] { runFavourite(f.id, f.title); }, true};
        item.actions << Action{tr("Remove from Favourites"), QStringLiteral("starred-symbolic"), QKeySequence(Qt::CTRL | Qt::Key_D),
                               [this, original] { toggleFavourite(original); }, true};
        items << item;
    }
    return items;
}

void Launcher::runFavourite(const QString &id, const QString &title)
{
    // Search for the favourite's title in the background and run the result
    // with its id as soon as a source returns it (see sourceResults).
    m_pendingRunId = id;
    m_pendingTimeout->start();
    for (Source *s : m_rootSources)
        s->query(title);
}

void Launcher::startAliasInput(const Item &item)
{
    ask(tr("Alias"), tr("Type an alias for “%1”, then press Enter").arg(item.title), item, false, [this, item](const QString &typed) {
        const QString alias = typed.trimmed();
        if (alias.isEmpty())
            return;
        QList<Library::Alias> aliases = m_library->aliases();
        aliases.removeIf([&](const Library::Alias &a) { return a.id == item.id || a.alias.compare(alias, Qt::CaseInsensitive) == 0; });
        aliases << Library::Alias{alias, item.id, item.title};
        m_library->setAliases(aliases);
        notify(tr("Type “%1” for %2").arg(alias, item.title));
        setQuery(alias); // show it working
    });
}

void Launcher::ask(const QString &chip, const QString &placeholder, const Item &shown, bool secret, std::function<void(const QString &)> done)
{
    startPrompt(chip, placeholder, shown, secret, Prompt{std::move(done), {}});
}

void Launcher::askArguments(const Item &shown, const QList<Argument> &arguments, const QStringList &initial,
                            std::function<void(const QStringList &)> done)
{
    if (arguments.isEmpty()) {
        done({});
        return;
    }
    startPrompt(shown.title, {}, shown, false, Prompt{{}, {}, std::move(done)});
    m_search->hide();
    m_args->setArguments(arguments, initial, this);
    m_glyph->setMode(SearchGlyph::Fill);
    updateFooter();
}

bool Launcher::handleArgumentKey(QKeyEvent *event)
{
    const int field = m_args->current();
    auto cancel = [this] {
        endPrompt();
        setQuery(m_savedQuery);
    };
    switch (event->key()) {
    case Qt::Key_Escape:
        cancel();
        return true;
    case Qt::Key_Tab:
        m_args->focusField(field + 1 == m_args->count() ? 0 : field + 1);
        return true;
    case Qt::Key_Backtab:
        m_args->focusField(field == 0 ? m_args->count() - 1 : field - 1);
        return true;
    case Qt::Key_Backspace:
        // In an empty field: back to the one before, or out of the command.
        if (m_args->value(field).isEmpty()) {
            if (field == 0)
                cancel();
            else
                m_args->focusField(field - 1);
            return true;
        }
        return false;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        if (const int missing = m_args->missing(); missing >= 0) {
            m_args->focusField(missing);
            notify(tr("Fill in “%1”").arg(m_args->label(missing)));
            return true;
        }
        const QStringList values = m_args->values();
        const auto done = m_prompt->onArguments;
        endPrompt();
        const QSignalBlocker block(m_search);
        m_search->setText(m_savedQuery);
        done(values);
        return true;
    }
    default:
        return false;
    }
}

void Launcher::askForKeys(const QString &chip, const QString &placeholder, const Item &shown, std::function<void(const QKeySequence &)> done)
{
    startPrompt(chip, placeholder, shown, false, Prompt{{}, std::move(done)});
}

void Launcher::startPrompt(const QString &chip, const QString &placeholder, const Item &shown, bool secret, const Prompt &prompt)
{
    if (m_actionsShown)
        showActions(false);
    m_prompt = prompt;
    m_savedQuery = m_search->text();
    m_chip->present(chip, true);
    m_glyph->setMode(prompt.onKeys ? SearchGlyph::Listen : SearchGlyph::Ask);
    if (isVisible())
        m_glow.start(0, 1, Motion::Fade, Motion::exit());
    else
        m_glow.jump(1);
    m_search->setPlaceholderText(placeholder);
    m_search->setEchoMode(secret ? QLineEdit::Password : QLineEdit::Normal);
    const QSignalBlocker block(m_search);
    m_search->clear();
    Item item = shown;
    item.actions.clear();
    showItems({item});
    update(); // the outline around the field
}

void Launcher::endPrompt()
{
    m_prompt.reset();
    if (m_args->isVisible()) {
        m_args->clear();
        m_search->show();
        m_search->setFocus();
    }
    m_search->setEchoMode(QLineEdit::Normal);
    const QSignalBlocker block(m_search);
    m_search->clear(); // never leave a typed password in the field
    if (m_view)
        m_chip->present(m_view->viewTitle(), false);
    else
        m_chip->hide();
    m_glyph->setMode(SearchGlyph::Search);
    m_glow.jump(0);
    m_search->setPlaceholderText(m_view ? m_view->viewPlaceholder() : tr("Search apps, commands, files…"));
    update();
}

NotesWindow *Launcher::notes()
{
    if (!m_notes)
        m_notes = new NotesWindow; // its own window: a Wayland overlay cannot parent one
    return m_notes;
}

void Launcher::openNotes()
{
    notes()->showAndFocus();
    hideLauncher();
}
