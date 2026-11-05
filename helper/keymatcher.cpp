#include "keymatcher.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

namespace
{

constexpr size_t MaxTyped = 64; // bytes of recent text kept, enough for any keyword

// Keys that move the cursor or leave the text: what was typed before no
// longer sits right before the cursor.
bool breaksTyping(unsigned code)
{
    switch (code) {
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_TAB:
    case KEY_ESC:
    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
    case KEY_HOME:
    case KEY_END:
    case KEY_PAGEUP:
    case KEY_PAGEDOWN:
    case KEY_DELETE:
        return true;
    default:
        return false;
    }
}

// Removes the last UTF-8 character.
void popCharacter(std::string &text)
{
    while (!text.empty()) {
        const unsigned char c = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((c & 0xC0) != 0x80) // not a continuation byte: a character start
            break;
    }
}

bool endsWith(const std::string &text, const std::string &end)
{
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

} // namespace

KeyMatcher::KeyMatcher(const std::string &layout, const std::string &variant)
{
    m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m_context)
        return;
    xkb_rule_names names{};
    names.layout = layout.empty() ? nullptr : layout.c_str();
    names.variant = variant.empty() ? nullptr : variant.c_str();
    m_keymap = xkb_keymap_new_from_names(m_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (m_keymap)
        m_state = xkb_state_new(m_keymap);
}

KeyMatcher::~KeyMatcher()
{
    xkb_state_unref(m_state);
    xkb_keymap_unref(m_keymap);
    xkb_context_unref(m_context);
}

void KeyMatcher::setKeywords(std::vector<std::string> keywords)
{
    m_keywords = std::move(keywords);
    m_typed.clear();
}

bool KeyMatcher::shortcutModifierHeld() const
{
    for (const char *mod : {XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT, XKB_MOD_NAME_LOGO})
        if (xkb_state_mod_name_is_active(m_state, mod, XKB_STATE_MODS_EFFECTIVE) > 0)
            return true;
    return false;
}

std::string KeyMatcher::key(unsigned code, int value)
{
    if (!m_state)
        return {};
    const xkb_keycode_t keycode = code + 8; // evdev → XKB key codes
    if (value == 0) {
        xkb_state_update_key(m_state, keycode, XKB_KEY_UP);
        return {};
    }
    if (value == 1)
        xkb_state_update_key(m_state, keycode, XKB_KEY_DOWN);

    if (breaksTyping(code) || shortcutModifierHeld()) {
        m_typed.clear(); // a shortcut or cursor move: the text before is no longer "just typed"
        return {};
    }
    if (code == KEY_BACKSPACE) {
        popCharacter(m_typed);
        return {};
    }

    char utf8[16];
    const int length = xkb_state_key_get_utf8(m_state, keycode, utf8, sizeof utf8);
    if (length <= 0 || static_cast<unsigned char>(utf8[0]) < 0x20)
        return {}; // a modifier or a key that types nothing
    m_typed.append(utf8, size_t(length));
    if (m_typed.size() > MaxTyped)
        m_typed.erase(0, m_typed.size() - MaxTyped);

    // The longest keyword wins (";address" over ";addr" sharing its end).
    std::string match;
    for (const std::string &k : m_keywords)
        if (!k.empty() && endsWith(m_typed, k) && k.size() > match.size())
            match = k;
    if (!match.empty())
        m_typed.clear();
    return match;
}
