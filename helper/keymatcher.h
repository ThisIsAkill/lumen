#pragma once

// Turns key events into typed text and reports when the text just typed ends
// with one of the registered keywords (";addr"). Kept apart from the device
// handling so it can be tested without any keyboard.

#include <string>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

class KeyMatcher
{
public:
    // layout and variant as in XKB ("us", "de" + "nodeadkeys"); empty = defaults.
    KeyMatcher(const std::string &layout, const std::string &variant);
    ~KeyMatcher();
    KeyMatcher(const KeyMatcher &) = delete;
    KeyMatcher &operator=(const KeyMatcher &) = delete;

    bool valid() const { return m_state != nullptr; }
    void setKeywords(std::vector<std::string> keywords);

    // A Linux key event (code from linux/input-event-codes.h; value 1 press,
    // 2 repeat, 0 release). Returns the keyword just completed, or "".
    std::string key(unsigned code, int value);

    // Forgets what was typed (focus may have moved, e.g. after a click).
    void reset() { m_typed.clear(); }

private:
    bool shortcutModifierHeld() const;

    xkb_context *m_context = nullptr;
    xkb_keymap *m_keymap = nullptr;
    xkb_state *m_state = nullptr;
    std::vector<std::string> m_keywords;
    std::string m_typed; // the last few characters, UTF-8
};
