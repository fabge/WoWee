// The unit query API, registered against a Lua state with no GameHandler.
//
// The first test of any lua_*_api.cpp file. It exists as much for the seam as
// for the assertions: nothing in src/addons/ was reachable from CTest before,
// which is why the highest-churn subsystem in the client had no test at all.
//
// Every binding here guards its handler pointer, because the interface calls
// them before a world is entered and during logout. That is what makes them
// testable without a server: registerUnitLuaAPI(L) against a bare state is a
// legitimate runtime state, not a contrivance for the test.
//
// It carried 41 stub definitions until 2026-08-26, 32 of them GameHandler
// methods: every binding guards its handler pointer so none is ever called,
// but the linker wants a definition anyway and the real ones sat behind 59
// translation units. The target links wowee_addons now and the stubs are gone.
//
// What is checked is the part FrameXML actually depends on and that a
// refactor silently breaks: the *return contract*. Blizzard's Lua treats nil,
// false, 0 and "" as different answers, and AGENTS.md says so - a binding that
// returns nil where FrameXML expects "" aborts the script that called it.
#include <catch_amalgamated.hpp>

#include <string>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include "addons/lua_api_registrations.hpp"
#include "addons/lua_api_helpers.hpp"
#include "core/app_clock.hpp"
#include "game/game_handler.hpp"



namespace {

struct LuaState {
    lua_State* L = luaL_newstate();
    LuaState() {
        // Base only. luaL_openlibs also wants the package library, which this
        // build of lua51 does not ship, and nothing here needs require().
        lua_pushcfunction(L, luaopen_base);
        lua_pushstring(L, "");
        lua_call(L, 1, 0);
        wowee::addons::registerUnitLuaAPI(L);
    }
    ~LuaState() { if (L) lua_close(L); }

    // Run a chunk and fail the test with Lua's own message if it does not.
    void run(const char* code) const {
        if (luaL_loadstring(L, code) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
            const char* message = lua_tostring(L, -1);
            FAIL(std::string(message ? message : "unknown Lua error"));
        }
    }
};

}  // namespace

TEST_CASE("the unit API registers without a game handler", "[lua][unit]") {
    LuaState lua;

    // The registration itself is the first assertion: it must not need a
    // world, a handler, or a widget tree to have been built.
    lua_getglobal(lua.L, "UnitName");
    REQUIRE(lua_isfunction(lua.L, -1));
    lua_pop(lua.L, 1);
}

TEST_CASE("unit queries answer safely with no world", "[lua][unit]") {
    LuaState lua;

    // UnitExists is a boolean in WoW, never nil: UnitFrame_Update tests it
    // directly and a nil would take the same branch by accident.
    lua.run("assert(UnitExists('player') == false, 'UnitExists should be false, got '"
            "  .. tostring(UnitExists('player')))");

    // UnitIsUnit answers about two units that both do not exist. It must still
    // return a boolean rather than erroring.
    lua.run("assert(type(UnitIsUnit('player', 'target')) ~= 'nil')");

    // Numeric health is 0 rather than nil: UnitFrameHealthBar_Update divides
    // by the max, and nil there is an error inside Blizzard's own code.
    lua.run("assert(UnitHealth('player') == 0, 'UnitHealth should be 0')");
    lua.run("assert(UnitHealthMax('player') == 0, 'UnitHealthMax should be 0')");
}

TEST_CASE("a nonsense unit id is answered, not raised", "[lua][unit]") {
    LuaState lua;

    // FrameXML passes unit ids built by concatenation - "party" .. i - and
    // asks about ids that do not resolve all the time. None of these may throw.
    lua.run("UnitName('party9')");
    lua.run("UnitName('')");
    lua.run("UnitExists('nonsense')");
    lua.run("UnitHealth('raid40')");
}
