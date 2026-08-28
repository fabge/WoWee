#pragma once

#include <imgui.h>

#include <string>
#include <utility>
#include <vector>

/// The interface's vocabulary for physical inputs, in one place.
///
/// A binding is a string. The panel writes the name of the key the player
/// pressed into the binding table, and the event pump looks a press up by the
/// name it computes for that same key - so the two have to spell every key
/// identically or the binding is written and never fires. They did not: the
/// panel wrote LEFTARROW, GRAVEACCENT and KEYPAD0 (ImGui's debug spellings,
/// which ImGui's own source says are "not meant to be saved persistently nor
/// compared"), while the pump answered LEFT, ` and nothing at all. Every one of
/// those keys was bindable and dead.
///
/// The names here are Blizzard's, taken from the KEY_* entries in
/// GlobalStrings.lua, which is what GetBindingText(key, "KEY_") looks up to
/// label a binding. Letters, digits and function keys have no KEY_ entry and
/// stand for themselves.
namespace wowee::ui {

/// The binding name for a character a key prints, or empty when the interface
/// has no name for it. Letters and digits stand for themselves, uppercased;
/// the punctuation the interface names is spelled its way, so - is MINUS and
/// the backtick is TILDE. Anything longer than one byte - an accented letter,
/// a dead key - names nothing.
[[nodiscard]] std::string wowKeyNameForCharacter(const std::string& printed);

/// The name for a physical key, or empty for one the interface cannot bind.
///
/// Where the platform can say what character a key produces under the active
/// layout, that character wins: on German QWERTZ the key in the ANSI Y
/// position is Z, and the key in the ANSI slash position prints -, so it is
/// MINUS rather than SLASH. A key whose character has no name keeps the name
/// of the ANSI position it sits in - unless the layout already handed that
/// name to another key, in which case it has none, because two keys under one
/// name is a binding that fires from the wrong key.
[[nodiscard]] std::string wowKeyNameFromScancode(int sdlScancode);

/// The same, for the keycode an SDL key event carries.
[[nodiscard]] std::string wowKeyNameFromKeycode(int sdlKeycode);

/// The same, for the key ImGui reports. This is the panel's side of the
/// contract above, and it must agree with the two functions above for every
/// key that has a name at all.
[[nodiscard]] std::string wowKeyNameFromImGuiKey(ImGuiKey key);

/// The key that name belongs to, or ImGuiKey_None. The client's own keybinding
/// manager stores ImGui keys, so a binding read back from the interface has to
/// come home this way.
[[nodiscard]] ImGuiKey imGuiKeyFromWowName(const std::string& name);

/// Whether a saved binding names an input this build can match.
///
/// Modifier prefixes are part of the name and are accepted in the interface's
/// own order. A saved name this build cannot match is a binding that would be
/// listed and never fire, and the caller is expected to fall back to the
/// default rather than keep it.
[[nodiscard]] bool isBindableName(const std::string& name);

/// What the active keyboard layout prints on the keys the interface names by
/// position, as (binding name, label) pairs.
///
/// The names are stable - APOSTROPHE is the key beside L wherever it is - but
/// the label on it is not, and the interface takes its labels from KEY_* in
/// GlobalStrings, which is one locale's answer. Reading enUS on a German
/// keyboard shows an apostrophe for Ä and a semicolon for Ö, which is what the
/// key would print on the keyboard Blizzard shipped that file for.
///
/// Empty where the platform cannot say. Only macOS can here; another platform
/// wanting this needs its own layout query behind core::localizedKeyLabel.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> layoutKeyLabels();

/// WoW's mouse button numbering, which is not SDL's: the interface calls the
/// right button BUTTON2 and the middle one BUTTON3, while SDL orders them the
/// other way round. Zero for a button the interface cannot bind, so a gaming
/// mouse's extra buttons resolve to nothing rather than to another button.
[[nodiscard]] int wowMouseButton(int sdlButton);

/// The binding table's own spelling for that button, or empty when unbindable.
[[nodiscard]] std::string wowMouseButtonName(int sdlButton);

/// The identifier a mouse press is tracked under. Negative, because presses are
/// kept in one table keyed by physical input and SDL keycodes are all positive
/// - a mouse button sharing an identifier with a key would release its binding.
[[nodiscard]] int mouseBindingPressKey(int wowButton);

}  // namespace wowee::ui
