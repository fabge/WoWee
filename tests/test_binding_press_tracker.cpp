#include <catch_amalgamated.hpp>

#include "ui/binding_press_tracker.hpp"

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
