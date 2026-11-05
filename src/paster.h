#pragma once

#include <QList>
#include <QObject>

#include <memory>

class FakeInput;

// Sends Ctrl+V to whatever window has focus, so a chosen clipboard item or
// snippet lands in the app the user was typing in.
//
// On Wayland it uses KWin's fake-input protocol, which KWin only grants to
// programs whose desktop entry lists it (X-KDE-Wayland-Interfaces) and points
// at the program's full path; install.sh sets that up. On X11 it uses XTest.
class Paster : public QObject
{
    Q_OBJECT
public:
    explicit Paster(QObject *parent = nullptr);
    ~Paster() override;

    // Whether pasting into other apps works in this session.
    bool isAvailable() const;

    // Presses Ctrl+V in the focused window.
    void paste();

    // Presses Backspace count times (to remove an expanded keyword).
    void backspace(int count);

    // Presses Left count times (to put the cursor where a snippet wants it).
    void left(int count);

private:
    void press(const QList<std::pair<uint, unsigned long>> &keys);

    std::unique_ptr<FakeInput> m_fakeInput;
    bool m_authenticated = false;
};
