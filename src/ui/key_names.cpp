#include "ui/key_names.hpp"

#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_mouse.h>

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef __APPLE__
#include "core/macos_platform.hpp"
#endif

namespace wowee::ui {
namespace {

struct KeyEntry {
    SDL_Scancode scancode;
    std::string name;
    ImGuiKey imguiKey;
};

/// Every key the interface has a name for, in one table.
///
/// The order matters in one place: the reverse lookup answers with the first
/// entry whose name matches, and the keypad's Enter shares ENTER with the main
/// one. The main key is listed first so a binding read back from the interface
/// comes home to the key the player is most likely holding.
const std::vector<KeyEntry>& keyTable() {
    static const std::vector<KeyEntry> table = [] {
        std::vector<KeyEntry> entries;

        // Letters and digits stand for themselves; neither has a KEY_ entry in
        // GlobalStrings, because there is nothing to translate.
        for (int i = 0; i < 26; ++i) {
            entries.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_A + i),
                               std::string(1, static_cast<char>('A' + i)),
                               static_cast<ImGuiKey>(ImGuiKey_A + i)});
        }
        for (int digit = 0; digit <= 9; ++digit) {
            // SDL runs 1..9 then 0; ImGui runs 0..9.
            const SDL_Scancode scancode =
                digit == 0 ? SDL_SCANCODE_0
                           : static_cast<SDL_Scancode>(SDL_SCANCODE_1 + digit - 1);
            entries.push_back({scancode, std::string(1, static_cast<char>('0' + digit)),
                               static_cast<ImGuiKey>(ImGuiKey_0 + digit)});
        }
        for (int f = 1; f <= 12; ++f) {
            entries.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + f - 1),
                               "F" + std::to_string(f),
                               static_cast<ImGuiKey>(ImGuiKey_F1 + f - 1)});
        }

        // The named keys, spelled as GlobalStrings' KEY_* entries spell them.
        // The punctuation names are the reason this table exists: WoW calls the
        // `=` key PLUS and the backtick TILDE, and a binding written any other
        // way is one the interface cannot label or match.
        static const KeyEntry kNamed[] = {
            {SDL_SCANCODE_ESCAPE,       "ESCAPE",        ImGuiKey_Escape},
            {SDL_SCANCODE_SPACE,        "SPACE",         ImGuiKey_Space},
            {SDL_SCANCODE_RETURN,       "ENTER",         ImGuiKey_Enter},
            {SDL_SCANCODE_KP_ENTER,     "ENTER",         ImGuiKey_KeypadEnter},
            {SDL_SCANCODE_TAB,          "TAB",           ImGuiKey_Tab},
            {SDL_SCANCODE_BACKSPACE,    "BACKSPACE",     ImGuiKey_Backspace},
            {SDL_SCANCODE_DELETE,       "DELETE",        ImGuiKey_Delete},
            {SDL_SCANCODE_INSERT,       "INSERT",        ImGuiKey_Insert},
            {SDL_SCANCODE_HOME,         "HOME",          ImGuiKey_Home},
            {SDL_SCANCODE_END,          "END",           ImGuiKey_End},
            {SDL_SCANCODE_PAGEUP,       "PAGEUP",        ImGuiKey_PageUp},
            {SDL_SCANCODE_PAGEDOWN,     "PAGEDOWN",      ImGuiKey_PageDown},
            {SDL_SCANCODE_LEFT,         "LEFT",          ImGuiKey_LeftArrow},
            {SDL_SCANCODE_RIGHT,        "RIGHT",         ImGuiKey_RightArrow},
            {SDL_SCANCODE_UP,           "UP",            ImGuiKey_UpArrow},
            {SDL_SCANCODE_DOWN,         "DOWN",          ImGuiKey_DownArrow},
            {SDL_SCANCODE_NUMLOCKCLEAR, "NUMLOCK",       ImGuiKey_NumLock},
            {SDL_SCANCODE_SCROLLLOCK,   "SCROLLLOCK",    ImGuiKey_ScrollLock},
            {SDL_SCANCODE_PRINTSCREEN,  "PRINTSCREEN",   ImGuiKey_PrintScreen},
            {SDL_SCANCODE_PAUSE,        "PAUSE",         ImGuiKey_Pause},
            {SDL_SCANCODE_MINUS,        "MINUS",         ImGuiKey_Minus},
            {SDL_SCANCODE_EQUALS,       "PLUS",          ImGuiKey_Equal},
            {SDL_SCANCODE_LEFTBRACKET,  "LEFTBRACKET",   ImGuiKey_LeftBracket},
            {SDL_SCANCODE_RIGHTBRACKET, "RIGHTBRACKET",  ImGuiKey_RightBracket},
            {SDL_SCANCODE_BACKSLASH,    "BACKSLASH",     ImGuiKey_Backslash},
            {SDL_SCANCODE_SEMICOLON,    "SEMICOLON",     ImGuiKey_Semicolon},
            {SDL_SCANCODE_APOSTROPHE,   "APOSTROPHE",    ImGuiKey_Apostrophe},
            {SDL_SCANCODE_GRAVE,        "TILDE",         ImGuiKey_GraveAccent},
            {SDL_SCANCODE_COMMA,        "COMMA",         ImGuiKey_Comma},
            {SDL_SCANCODE_PERIOD,       "PERIOD",        ImGuiKey_Period},
            {SDL_SCANCODE_SLASH,        "SLASH",         ImGuiKey_Slash},
            {SDL_SCANCODE_KP_0,         "NUMPAD0",       ImGuiKey_Keypad0},
            {SDL_SCANCODE_KP_1,         "NUMPAD1",       ImGuiKey_Keypad1},
            {SDL_SCANCODE_KP_2,         "NUMPAD2",       ImGuiKey_Keypad2},
            {SDL_SCANCODE_KP_3,         "NUMPAD3",       ImGuiKey_Keypad3},
            {SDL_SCANCODE_KP_4,         "NUMPAD4",       ImGuiKey_Keypad4},
            {SDL_SCANCODE_KP_5,         "NUMPAD5",       ImGuiKey_Keypad5},
            {SDL_SCANCODE_KP_6,         "NUMPAD6",       ImGuiKey_Keypad6},
            {SDL_SCANCODE_KP_7,         "NUMPAD7",       ImGuiKey_Keypad7},
            {SDL_SCANCODE_KP_8,         "NUMPAD8",       ImGuiKey_Keypad8},
            {SDL_SCANCODE_KP_9,         "NUMPAD9",       ImGuiKey_Keypad9},
            {SDL_SCANCODE_KP_PERIOD,    "NUMPADDECIMAL", ImGuiKey_KeypadDecimal},
            {SDL_SCANCODE_KP_DIVIDE,    "NUMPADDIVIDE",  ImGuiKey_KeypadDivide},
            {SDL_SCANCODE_KP_MULTIPLY,  "NUMPADMULTIPLY", ImGuiKey_KeypadMultiply},
            {SDL_SCANCODE_KP_MINUS,     "NUMPADMINUS",   ImGuiKey_KeypadSubtract},
            {SDL_SCANCODE_KP_PLUS,      "NUMPADPLUS",    ImGuiKey_KeypadAdd},
        };
        for (const auto& entry : kNamed) entries.push_back(entry);
        return entries;
    }();
    return table;
}

/// The name resolved for every key in the table, in the table's own order,
/// under the keyboard layout in use.
///
/// SDL and ImGui both name the physical ANSI position, and a binding has to
/// carry the character the key actually prints - so this is resolved once for
/// the whole keyboard rather than per key. It has to be the whole keyboard,
/// because the answer for one key decides the answer for another: on German
/// QWERTZ the ANSI slash position prints -, which makes it MINUS, and the ANSI
/// minus position, whose key prints the sharp s, therefore cannot keep the
/// name MINUS as well. Two
/// keys under one name is a binding that fires from the wrong key, which is
/// exactly the bug - pressing - opened chat, because - sat in the slash
/// position and SLASH is bound to chat.
///
/// So the layout is asked first and position fills in only what is left over:
///
///   1. every key the layout gives a nameable character takes that name,
///   2. every other key keeps its position's name, unless step 1 gave that
///      name away - then it has no name, and cannot be bound. The sharp s
///      and the acute accent land here on German; both would otherwise be a
///      second MINUS and a second PLUS.
const std::vector<std::string>& layoutNames() {
    const auto& table = keyTable();
#ifdef __APPLE__
    static std::vector<std::string> resolved;
    static std::string resolvedFor = "\x01";  // no layout answers this
    if (const std::string layout = core::keyboardLayoutIdentifier();
        resolved.empty() || layout.empty() || layout != resolvedFor) {
        resolvedFor = layout;
        resolved.assign(table.size(), std::string());
        std::unordered_set<std::string> claimed;
        for (size_t i = 0; i < table.size(); ++i) {
            resolved[i] = wowKeyNameForCharacter(core::localizedKeyName(table[i].scancode));
            if (!resolved[i].empty()) claimed.insert(resolved[i]);
        }
        for (size_t i = 0; i < table.size(); ++i) {
            if (resolved[i].empty() && !claimed.contains(table[i].name)) {
                resolved[i] = table[i].name;
            }
        }
    }
    return resolved;
#else
    // Nothing here can say what the keys print, so the ANSI position is the
    // only answer there is.
    static const std::vector<std::string> resolved = [&] {
        std::vector<std::string> names;
        names.reserve(table.size());
        for (const auto& entry : table) names.push_back(entry.name);
        return names;
    }();
    return resolved;
#endif
}

}  // namespace

std::string wowKeyNameForCharacter(const std::string& printed) {
    // Accented letters and the dead keys are several bytes; none names a key.
    if (printed.size() != 1) return {};
    const auto value = static_cast<unsigned char>(printed[0]);
    if (std::isalnum(value) != 0) {
        return std::string(1, static_cast<char>(std::toupper(value)));
    }
    // The punctuation the interface has a name for. Two characters answer
    // PLUS because two layouts disagree about which one that key prints: the
    // ANSI keyboard WoW's names were written for prints =, and a German one
    // prints + on the key next to it.
    switch (printed[0]) {
        case '-':  return "MINUS";
        case '=':
        case '+':  return "PLUS";
        case '[':  return "LEFTBRACKET";
        case ']':  return "RIGHTBRACKET";
        case '\\': return "BACKSLASH";
        case ';':  return "SEMICOLON";
        case '\'': return "APOSTROPHE";
        case '`':
        case '~':  return "TILDE";
        case ',':  return "COMMA";
        case '.':  return "PERIOD";
        case '/':  return "SLASH";
        default:   return {};
    }
}

std::string wowKeyNameFromScancode(int sdlScancode) {
    const auto& table = keyTable();
    const auto& names = layoutNames();
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].scancode == sdlScancode) return names[i];
    }
    return {};
}

std::string wowKeyNameFromKeycode(int sdlKeycode) {
    return wowKeyNameFromScancode(SDL_GetScancodeFromKey(sdlKeycode));
}

std::string wowKeyNameFromImGuiKey(ImGuiKey key) {
    const auto& table = keyTable();
    const auto& names = layoutNames();
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].imguiKey == key) return names[i];
    }
    return {};
}

ImGuiKey imGuiKeyFromWowName(const std::string& name) {
    if (name.empty()) return ImGuiKey_None;
    const auto& table = keyTable();
    const auto& names = layoutNames();
    for (size_t i = 0; i < table.size(); ++i) {
        if (names[i] == name) return table[i].imguiKey;
    }
    return ImGuiKey_None;
}

std::vector<std::pair<std::string, std::string>> layoutKeyLabels() {
    std::vector<std::pair<std::string, std::string>> labels;
#ifdef __APPLE__
    const auto& table = keyTable();
    const auto& names = layoutNames();
    for (size_t i = 0; i < table.size(); ++i) {
        // Only the keys named by position, under the name this layout actually
        // resolved for them. A letter or a digit is its own label, and the
        // interface has no KEY_ entry for either; a key the layout left
        // nameless has nothing to label.
        if (names[i].size() <= 1) continue;
        std::string label = core::localizedKeyLabel(table[i].scancode);
        if (label.empty() || label == names[i]) continue;
        labels.emplace_back(names[i], std::move(label));
    }
#endif
    return labels;
}

bool isBindableName(const std::string& name) {
    std::string key = name;
    // ALT before CTRL before SHIFT, which is how the binding tables are keyed.
    for (const char* prefix : {"ALT-", "CTRL-", "SHIFT-"}) {
        const std::string text(prefix);
        if (key.rfind(text, 0) == 0) key = key.substr(text.size());
    }
    if (key.empty()) return false;
    if (imGuiKeyFromWowName(key) != ImGuiKey_None) return true;

    // The mouse, which has no ImGui key of its own here. Buttons run to 31 in
    // the interface's own tables, and the wheel is bindable there too - both
    // are real names a player may have saved, whether or not this build
    // dispatches them yet.
    if (key == "MOUSEWHEELUP" || key == "MOUSEWHEELDOWN") return true;
    if (key.rfind("BUTTON", 0) == 0) {
        const std::string digits = key.substr(6);
        if (digits.empty() || digits.size() > 2) return false;
        for (const char c : digits) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        const int button = std::stoi(digits);
        return button >= 1 && button <= 31;
    }
    return false;
}

int wowMouseButton(int sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_LEFT:   return 1;
        case SDL_BUTTON_RIGHT:  return 2;
        case SDL_BUTTON_MIDDLE: return 3;
        case SDL_BUTTON_X1:     return 4;
        case SDL_BUTTON_X2:     return 5;
        default:                return 0;
    }
}

std::string wowMouseButtonName(int sdlButton) {
    const int button = wowMouseButton(sdlButton);
    if (button == 0) return {};
    return "BUTTON" + std::to_string(button);
}

int mouseBindingPressKey(int wowButton) { return -wowButton; }

}  // namespace wowee::ui
