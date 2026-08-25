#include <catch_amalgamated.hpp>

#include "ui/binding_press_tracker.hpp"
#include "ui/mouse_binding.hpp"

using wowee::ui::BindingPressTracker;

TEST_CASE("a release belongs to the command selected on press", "[bindings]") {
    BindingPressTracker presses;
    presses.press(44, "CTRL-ACTION");

    // The caller may now report no Ctrl modifier or resolve the key to another
    // command; neither changes the active pair.
    REQUIRE(presses.release(44) == "CTRL-ACTION");
    CHECK_FALSE(presses.release(44).has_value());
}

TEST_CASE("rebinding another key does not disturb an active press", "[bindings]") {
    BindingPressTracker presses;
    presses.press(10, "FIRST");
    presses.press(11, "SECOND");
    presses.press(11, "REBOUND");

    CHECK(presses.release(10) == "FIRST");
    CHECK(presses.release(11) == "REBOUND");
}

// Mouse buttons share the tracker with keys, so their identifiers are part of
// the same contract as the pairing above.
TEST_CASE("mouse buttons carry the interface's numbering", "[bindings]") {
    // SDL orders middle before right; the interface does not.
    CHECK(wowee::ui::wowMouseButtonName(SDL_BUTTON_LEFT) == "BUTTON1");
    CHECK(wowee::ui::wowMouseButtonName(SDL_BUTTON_RIGHT) == "BUTTON2");
    CHECK(wowee::ui::wowMouseButtonName(SDL_BUTTON_MIDDLE) == "BUTTON3");
    CHECK(wowee::ui::wowMouseButtonName(SDL_BUTTON_X1) == "BUTTON4");
    CHECK(wowee::ui::wowMouseButtonName(SDL_BUTTON_X2) == "BUTTON5");

    // A button the interface has no name for resolves to nothing rather than
    // to another button's binding.
    CHECK(wowee::ui::wowMouseButtonName(9).empty());
    CHECK(wowee::ui::wowMouseButton(9) == 0);
}

TEST_CASE("a mouse press cannot release a key's binding", "[bindings]") {
    BindingPressTracker presses;
    // SDLK_a, and the mouse identifier that would collide with it if mouse
    // buttons were tracked by their own numbering.
    presses.press('a', "KEY_COMMAND");
    presses.press(wowee::ui::mouseBindingPressKey(1), "MOUSE_COMMAND");

    CHECK(presses.release('a') == "KEY_COMMAND");
    CHECK(presses.release(wowee::ui::mouseBindingPressKey(1)) == "MOUSE_COMMAND");

    for (int button = 1; button <= 5; ++button) {
        CHECK(wowee::ui::mouseBindingPressKey(button) < 0);
    }
}
