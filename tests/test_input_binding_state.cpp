#include <catch_amalgamated.hpp>

#include "core/input.hpp"

using wowee::core::Input;

TEST_CASE("binding command edges last for one event-pump iteration", "[input][bindings]") {
    auto& input = Input::getInstance();
    input.clearBindingCommands();
    input.beginFrame();

    input.setBindingCommandHeld("JUMP", true);
    CHECK(input.isBindingCommandHeld("JUMP"));
    CHECK(input.isBindingCommandJustPressed("JUMP"));

    input.beginFrame();
    CHECK(input.isBindingCommandHeld("JUMP"));
    CHECK_FALSE(input.isBindingCommandJustPressed("JUMP"));

    input.setBindingCommandHeld("JUMP", false);
    CHECK_FALSE(input.isBindingCommandHeld("JUMP"));
}

TEST_CASE("two physical bindings can hold the same command", "[input][bindings]") {
    auto& input = Input::getInstance();
    input.clearBindingCommands();
    input.beginFrame();

    input.setBindingCommandHeld("MOVEFORWARD", true);
    input.setBindingCommandHeld("MOVEFORWARD", true);
    input.setBindingCommandHeld("MOVEFORWARD", false);
    CHECK(input.isBindingCommandHeld("MOVEFORWARD"));

    input.setBindingCommandHeld("MOVEFORWARD", false);
    CHECK_FALSE(input.isBindingCommandHeld("MOVEFORWARD"));
}
