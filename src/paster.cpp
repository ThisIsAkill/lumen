#include "paster.h"

#include "qwayland-fake-input.h"

#include <QGuiApplication>
#include <QWaylandClientExtensionTemplate>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <linux/input-event-codes.h>

// KWin's fake-input global, bound when the compositor offers it to us.
class FakeInput : public QWaylandClientExtensionTemplate<FakeInput>, public QtWayland::org_kde_kwin_fake_input
{
public:
    FakeInput()
        : QWaylandClientExtensionTemplate<FakeInput>(4) // keyboard_key appeared in version 4
    {
        initialize();
    }
};

namespace
{

bool onWayland()
{
    return QGuiApplication::platformName() == QLatin1String("wayland");
}

Display *x11Display()
{
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->display() : nullptr;
}

} // namespace

Paster::Paster(QObject *parent)
    : QObject(parent)
{
    if (onWayland())
        m_fakeInput = std::make_unique<FakeInput>();
}

Paster::~Paster() = default;

bool Paster::isAvailable() const
{
    if (onWayland())
        return m_fakeInput && m_fakeInput->isActive();
    int event, error, major, minor;
    Display *display = x11Display();
    return display && XTestQueryExtension(display, &event, &error, &major, &minor);
}

// Presses the keys (Linux key codes / X11 keysyms) in order, then releases
// them in reverse order.
void Paster::press(const QList<std::pair<uint, unsigned long>> &keys)
{
    if (!isAvailable())
        return;
    if (onWayland()) {
        if (!m_authenticated) {
            m_fakeInput->authenticate(QStringLiteral("Lumen"), tr("Paste clipboard items and snippets into apps"));
            m_authenticated = true;
        }
        constexpr uint Pressed = 1, Released = 0;
        for (const auto &key : keys)
            m_fakeInput->keyboard_key(key.first, Pressed);
        for (auto it = keys.crbegin(); it != keys.crend(); ++it)
            m_fakeInput->keyboard_key(it->first, Released);
        return;
    }
    Display *display = x11Display();
    for (const auto &key : keys)
        XTestFakeKeyEvent(display, XKeysymToKeycode(display, key.second), True, CurrentTime);
    for (auto it = keys.crbegin(); it != keys.crend(); ++it)
        XTestFakeKeyEvent(display, XKeysymToKeycode(display, it->second), False, CurrentTime);
    XFlush(display);
}

void Paster::paste()
{
    press({{KEY_LEFTCTRL, XK_Control_L}, {KEY_V, XK_v}});
}

void Paster::left(int count)
{
    for (int i = 0; i < count; ++i)
        press({{KEY_LEFT, XK_Left}});
}

void Paster::backspace(int count)
{
    for (int i = 0; i < count; ++i)
        press({{KEY_BACKSPACE, XK_BackSpace}});
}
