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
// What is checked is the part FrameXML actually depends on and that a
// refactor silently breaks: the *return contract*. Blizzard's Lua treats nil,
// false, 0 and "" as different answers, and AGENTS.md says so - a binding that
// returns nil where FrameXML expects "" aborts the script that called it.
#include <catch_amalgamated.hpp>

#include <memory>
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
#include "game/shapeshift_forms.hpp"
#include "game/update_field_table.hpp"
#include "ui/widget_tree.hpp"

// ---------------------------------------------------------------------------
// The sixteen symbols this file needs to link, and the reason they are here.
//
// Every one of these is behind a `if (!gh) return` guard at runtime, so none is
// ever called by these tests. The linker does not care: an unreached call still
// needs a definition, and the definitions live in translation units that pull
// in the rest of the client behind them.
//
// So this is the library-targets item in TODO.md, priced. Linking the real
// game_handler.cpp here would drag in its 59 translation units; stubbing is
// what the build graph leaves. When subsystem libraries exist, delete this
// block and link the library instead - that is the whole point of doing it.
// ---------------------------------------------------------------------------
namespace wowee::addons {
uint32_t cursorItemId() { return 0; }
int cursorEquipSlot() { return 0; }
}  // namespace wowee::addons

namespace wowee::core {
double appTimeSeconds() { return 0.0; }
}  // namespace wowee::core

namespace wowee::game {
std::vector<ShapeshiftForm> allShapeshiftForms(uint8_t) { return {}; }
const UpdateFieldTable* getActiveUpdateFieldTable() { return nullptr; }
uint16_t UpdateFieldTable::index(UF) const { return 0; }
std::shared_ptr<Entity> EntityManager::getEntity(uint64_t) const { return nullptr; }
const ItemSlot& Inventory::getEquipSlot(EquipSlot) const {
    static const ItemSlot empty{};
    return empty;
}

void GameHandler::cancelAura(uint32_t) {}
void GameHandler::castSpell(uint32_t, uint64_t) {}
void GameHandler::dismount() {}
void GameHandler::reclaimCorpse() {}
void GameHandler::reportPvpAfk(uint64_t) {}
void GameHandler::requestPvpLog() {}
void GameHandler::startAutoAttack(uint64_t) {}
void GameHandler::stopAutoAttack() {}
bool GameHandler::isInCombat() const { return false; }
bool GameHandler::isOnTaxiFlight() const { return false; }
bool GameHandler::isInGroup() const { return false; }
bool GameHandler::isVendorWindowOpen() const { return false; }
uint64_t GameHandler::getEncounterUnitGuid(uint32_t) const { return 0; }
float GameHandler::getCombatRatingBonus(int) const { return 0.0f; }
float GameHandler::getHealthRegenFromSpirit() const { return 0.0f; }
float GameHandler::getManaRegenFromSpirit() const { return 0.0f; }
float GameHandler::getMeleeCritFromAgility() const { return 0.0f; }
float GameHandler::getSpellCritFromIntellect() const { return 0.0f; }
float GameHandler::getServerRunSpeed() const { return 0.0f; }
float GameHandler::getSpellCooldownTotal(uint32_t) const { return 0.0f; }
std::string GameHandler::getFormattedTitle(uint32_t) const { return {}; }
const Character* GameHandler::getActiveCharacter() const { return nullptr; }
const std::string& GameHandler::getFactionNamePublic(uint32_t) const {
    static const std::string empty;
    return empty;
}
uint64_t GameHandler::getBankerGuid() const { return 0; }
uint64_t GameHandler::getVendorGuid() const { return 0; }
uint32_t GameHandler::getSkillCategory(uint32_t) const { return 0; }
float GameHandler::getSpellCooldown(uint32_t) const { return 0.0f; }
const std::vector<CombatHandler::ThreatEntry>* GameHandler::getThreatList(uint64_t) const {
    return nullptr;
}
const std::vector<Companion>& GameHandler::getCompanions(bool) const {
    static const std::vector<Companion> empty{};
    return empty;
}
const GossipMessageData& GameHandler::getCurrentGossip() const {
    static const GossipMessageData empty{};
    return empty;
}
const QuestDetailsData& GameHandler::getQuestDetails() const {
    static const QuestDetailsData empty{};
    return empty;
}
const GroupListData& GameHandler::getPartyData() const {
    static const GroupListData empty{};
    return empty;
}
}  // namespace wowee::game

namespace wowee::ui {
void WidgetTree::setPortraitUnit(uint32_t, const std::string&) {}
}  // namespace wowee::ui


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
