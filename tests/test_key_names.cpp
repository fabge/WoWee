#include <catch_amalgamated.hpp>

#include "ui/key_names.hpp"

#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_mouse.h>

using namespace wowee::ui;

// The contract this module exists for. The binding panel names a key one way
// and the event pump names the same key another way; a binding is a string, so
// the two spellings have to be one spelling. They were not: ImGui's debug names
// gave the panel LEFTARROW, GRAVEACCENT and KEYPAD0 while the pump answered
// LEFT, ` and nothing, and every one of those keys could be bound and would
// never fire.
TEST_CASE("the press and the panel name every key identically", "[bindings][keys]") {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const auto key = static_cast<ImGuiKey>(k);
        const std::string panel = wowKeyNameFromImGuiKey(key);
        if (panel.empty()) continue;  // not a key the interface can bind

        const ImGuiKey back = imGuiKeyFromWowName(panel);
        REQUIRE(back != ImGuiKey_None);
        // Enter is the one name two keys share, so the keypad's Enter comes
        // home to the main one. Every other name is its own key.
        CHECK(wowKeyNameFromImGuiKey(back) == panel);
    }
}

TEST_CASE("a keycode and its scancode name the same key", "[bindings][keys]") {
    const SDL_Scancode scancodes[] = {
        SDL_SCANCODE_A,      SDL_SCANCODE_5,     SDL_SCANCODE_F7,
        SDL_SCANCODE_ESCAPE, SDL_SCANCODE_LEFT,  SDL_SCANCODE_GRAVE,
        SDL_SCANCODE_EQUALS, SDL_SCANCODE_KP_4,  SDL_SCANCODE_SPACE,
    };
    for (const SDL_Scancode scancode : scancodes) {
        const std::string byScancode = wowKeyNameFromScancode(scancode);
        CHECK_FALSE(byScancode.empty());
        CHECK(wowKeyNameFromKeycode(SDL_GetKeyFromScancode(scancode)) == byScancode);
    }
}

// Blizzard's spellings, from the KEY_* entries in GlobalStrings.lua that
// GetBindingText(key, "KEY_") looks up to label a binding. A name that is not
// one of these labels as nothing and matches nothing.
TEST_CASE("keys carry Blizzard's names", "[bindings][keys]") {
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_LEFT) == "LEFT");
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_PAGEUP) == "PAGEUP");
    // The two that ImGui spells GraveAccent and Equal.
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_GRAVE) == "TILDE");
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_EQUALS) == "PLUS");
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_MINUS) == "MINUS");
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_KP_0) == "NUMPAD0");
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_KP_PLUS) == "NUMPADPLUS");
    // The keypad's Enter is the main Enter to the binding tables.
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_KP_ENTER) == "ENTER");
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_RETURN) == "ENTER");
    CHECK(imGuiKeyFromWowName("ENTER") == ImGuiKey_Enter);

    // Keys the interface has no name for answer with nothing rather than with
    // an invented one: a binding written for them could never be matched.
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_CAPSLOCK).empty());
    CHECK(wowKeyNameFromScancode(SDL_SCANCODE_LGUI).empty());
    CHECK(imGuiKeyFromWowName("") == ImGuiKey_None);
    CHECK(imGuiKeyFromWowName("NOSUCHKEY") == ImGuiKey_None);
}

// Letters follow the layout, whatever it is. Asserting Y stays Y would fail on
// a German keyboard and asserting it becomes Z would fail on a US one, so what
// is checked is the property that has to hold on both: the physical key names
// itself as a single letter, and that letter comes home to the same key.
TEST_CASE("a letter key names itself under the active layout", "[bindings][keys]") {
    const std::string name = wowKeyNameFromScancode(SDL_SCANCODE_Y);
    REQUIRE(name.size() == 1);
    CHECK(name[0] >= 'A');
    CHECK(name[0] <= 'Z');
    CHECK(imGuiKeyFromWowName(name) == ImGuiKey_Y);
    CHECK(wowKeyNameFromImGuiKey(ImGuiKey_Y) == name);
}

TEST_CASE("mouse buttons carry the interface's numbering", "[bindings][keys]") {
    // SDL orders middle before right; the interface does not.
    CHECK(wowMouseButtonName(SDL_BUTTON_LEFT) == "BUTTON1");
    CHECK(wowMouseButtonName(SDL_BUTTON_RIGHT) == "BUTTON2");
    CHECK(wowMouseButtonName(SDL_BUTTON_MIDDLE) == "BUTTON3");
    CHECK(wowMouseButtonName(SDL_BUTTON_X1) == "BUTTON4");
    CHECK(wowMouseButtonName(SDL_BUTTON_X2) == "BUTTON5");

    // A button the interface has no name for resolves to nothing rather than
    // to another button's binding.
    CHECK(wowMouseButtonName(9).empty());
    CHECK(wowMouseButton(9) == 0);

    // Mouse presses share the press tracker with keys, whose identifiers are
    // SDL keycodes and always positive.
    for (int button = 1; button <= 5; ++button) {
        CHECK(mouseBindingPressKey(button) < 0);
    }
}

// What a saved bindings file is allowed to say. Files exist carrying the
// panel's old ImGui spellings, and a name this build cannot match is a listed
// binding that never fires - the loader drops those and keeps the default.
TEST_CASE("a saved binding is checked against the vocabulary", "[bindings][keys]") {
    CHECK(isBindableName("A"));
    CHECK(isBindableName("SPACE"));
    CHECK(isBindableName("NUMPAD7"));
    CHECK(isBindableName("BUTTON2"));
    CHECK(isBindableName("BUTTON31"));
    CHECK(isBindableName("MOUSEWHEELUP"));

    // Modifier prefixes are part of the name.
    CHECK(isBindableName("SHIFT-A"));
    CHECK(isBindableName("ALT-CTRL-SHIFT-F4"));
    CHECK(isBindableName("CTRL-BUTTON3"));

    // The old spellings, and the shapes that are simply not names.
    CHECK_FALSE(isBindableName("LEFTARROW"));
    CHECK_FALSE(isBindableName("GRAVEACCENT"));
    CHECK_FALSE(isBindableName("EQUAL"));
    CHECK_FALSE(isBindableName("KEYPAD0"));
    CHECK_FALSE(isBindableName(""));
    CHECK_FALSE(isBindableName("SHIFT-"));
    CHECK_FALSE(isBindableName("BUTTON"));
    CHECK_FALSE(isBindableName("BUTTON0"));
    CHECK_FALSE(isBindableName("BUTTON32"));
    CHECK_FALSE(isBindableName("BUTTONX"));
}
