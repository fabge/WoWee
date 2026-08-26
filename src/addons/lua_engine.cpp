#include "addons/lua_engine.hpp"
#include "addons/lua_widget_internal.hpp"
#include "ui/link_hit.hpp"
#include "ui/text_markup.hpp"
#include "ui/plural_escape.hpp"
#include "ui/widget_tree.hpp"
#include "ui/interface_fonts.hpp"
#include "ui/ui_colors.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/key_names.hpp"
#include <chrono>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <climits>
#include <set>
#include <cstdlib>
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_handler_globals.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/toc_parser.hpp"
#include "core/window.hpp"
#include <imgui.h>
#include <SDL2/SDL_keyboard.h>
#include <fstream>
#include "core/app_clock.hpp"
#include "core/config_paths.hpp"
#include "core/input.hpp"
#ifdef __APPLE__
#include "core/macos_platform.hpp"
#endif
#include <filesystem>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace wowee::addons {

bool LuaEngine::uiSoundsSuppressed_ = false;


namespace {
}

// ---- The seam with lua_widget_api.cpp ----
//
// Declared in lua_widget_internal.hpp and defined here at namespace scope:
// the widget bindings in the other translation unit are built on them.
// They were file-local while everything lived in one file, and the length
// of this list is the measure of how separable the widget surface was.

/// Names asked for and not found, while the fallback is on. File-scope because
/// the recorder is a Lua callback and the report runs at shutdown.
std::set<std::string>& missingApiNames() {
    static std::set<std::string> names;
    return names;
}

/// Log at warning from Lua.
///
/// print() goes to chat and to the log at info, and the log carries nothing
/// below warning - so anything printed for diagnosis is invisible in the one
/// place it would be read. This is for the interface probe, which had been
/// producing no output at all for that reason.
int lua_wowee_warn(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    LOG_WARNING(msg ? msg : "(nil)");
    return 0;
}

int lua_wow_print(lua_State* L) {
    int nargs = lua_gettop(L);
    std::string result;
    for (int i = 1; i <= nargs; i++) {
        if (i > 1) result += '\t';
        // Lua 5.1: use lua_tostring (luaL_tolstring is 5.3+)
        if (lua_isstring(L, i) || lua_isnumber(L, i)) {
            const char* s = lua_tostring(L, i);
            if (s) result += s;
        } else if (lua_isboolean(L, i)) {
            result += lua_toboolean(L, i) ? "true" : "false";
        } else if (lua_isnil(L, i)) {
            result += "nil";
        } else {
            result += lua_typename(L, lua_type(L, i));
        }
    }

    auto* gh = getGameHandler(L);
    if (gh) {
        game::MessageChatData msg;
        msg.type = game::ChatType::SYSTEM;
        msg.language = game::ChatLanguage::UNIVERSAL;
        msg.message = result;
        gh->addLocalChatMessage(msg);
    }
    LOG_INFO("[Lua] ", result);
    return 0;
}

// WoW-compatible message() - same as print for now
int lua_wow_message(lua_State* L) {
    return lua_wow_print(L);
}

uint32_t widgetIdOf(lua_State* L, int index) {
    if (!lua_istable(L, index)) return 0;
    lua_getfield(L, index, "__wid");
    const uint32_t id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return id;
}

wowee::ui::Widget* widgetOf(lua_State* L, int index) {
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree) return nullptr;
    return tree->get(widgetIdOf(L, index));
}

/// Report a script that raised, from the free functions that call one.
///
/// Four of these swallowed the error outright - OnEnable, OnDisable,
/// OnValueChanged and OnColorSelect each ended `!= 0) lua_pop(L, 1)`, so a
/// raise in a slider's handler or a button's enable left no log line, no entry
/// in the error report and no sign on screen. An error nothing records is the
/// hardest kind to be asked about afterwards.
void recordScriptError(lua_State* L, const char* script) {
    const char* err = lua_tostring(L, -1);
    const std::string msg = std::string(script ? script : "script") + ": " +
                            (err ? err : "?");
    LOG_ERROR("LuaEngine: ", msg);
    if (auto* e = engineFrom(L)) e->noteLuaError(msg);
    lua_pop(L, 1);
}

/// Runs a frame's own handler, given its table already on the stack.
///
/// Separate from callFrameScript, which starts from a widget id and looks the
/// table up: inside a binding the table is the first argument already, and
/// going back through the registry to find what is in hand is both slower and
/// wrong for a frame that was never registered.
void callScriptOnTable(lua_State* L, int tableIdx, const char* script,
                              double arg) {
    // Positive indices only, which is all any caller here has: lua_absindex
    // is 5.2 and this is 5.1.
    const int abs = tableIdx;
    lua_getfield(L, abs, "__scripts");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, script);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
    lua_pushvalue(L, abs);
    lua_pushnumber(L, arg);
    if (pcallScript(L, script, 2, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        LOG_ERROR("LuaEngine: ", script, " error: ", err ? err : "?");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/// __WoweeSetAnimOffset(frame, x, y) - where a Translation has moved it to.
///
/// Not a WoW function: it is how the animation system, which is written in Lua,
/// reaches the one thing it cannot do from there. Displacing the anchors would
/// leave the movement behind after the animation stopped.
int lua_wowee_setAnimOffset(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->animOffsetX = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        w->animOffsetY = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    }
    return 0;
}

/// GetMouseFocus() - the frame the cursor is over, or nil.
///
/// The tree already knows: it is what decides which frame receives OnEnter and
/// which takes a click. FrameXML compares against it to decide whether a
/// tooltip belongs to the frame under the pointer, and the vehicle bar asks it
/// directly. It answered nothing at all before.
int lua_GetMouseFocus(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree) { lua_pushnil(L); return 1; }
    const uint32_t hovered = tree->hoveredWidget();
    if (hovered == 0) { lua_pushnil(L); return 1; }
    lua_getglobal(L, "__WoweeFramesByWid");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; }
    lua_pushinteger(L, static_cast<lua_Integer>(hovered));
    lua_rawget(L, -2);
    lua_remove(L, -2);          // drop the registry table, keep the frame
    return 1;
}

LuaEngine* engineFrom(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* e = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return e;
}






// Helper: resolve WoW unit IDs to GUID

// --- Frame system functions ---










// ── Widget-backed regions ───────────────────────────────────────────────────
//
// Frames and regions are Lua tables, as they were, but each now carries a
// __wid handle into the C++ widget tree that holds its geometry and its art.
// Without that the methods below were a table of no-ops: an addon could call
// SetTexture all day and nothing existed to draw.

namespace {







/// Whether the frame is shown, from the widget rather than a field beside it.
///
/// Show and Hide write both, but they are not the only way a frame's
/// visibility changes, so the two drift. The bag buttons ask this to decide
/// whether to draw themselves pressed - which is why one stayed lit over a bag
/// that had been closed. Defined here rather than with the other frame methods
/// because it needs widgetOf, which is declared just above.




























































/// Shared by every setter that ends up naming an item: title in the quality
/// colour, then whatever else is worth saying. One place, because the four
/// item setters differ only in how they find the item.































// ── Fonts ───────────────────────────────────────────────────────────────────














} // namespace



// ── Backdrop and StatusBar ──────────────────────────────────────────────────












































// Modifier key state queries using ImGui IO


// SetAlpha and GetAlpha are lua_Region_SetAlpha and lua_Region_GetAlpha, which
// is what both registrations name. A second pair here kept the value in a
// __alpha field on the frame table instead of on the widget, was bound to
// nothing, and stood as an alternative implementation for anyone reading -
// the shape where a later fix lands on the copy nothing calls.



/// Records a global FrameXML or an addon asked for and did not find. Logged
/// once per name; the set is reported at shutdown so the gap can be read off a
/// run rather than guessed at.
static int lua_RecordMissingApi(lua_State* L) {
    const char* name = luaL_optstring(L, 1, "");
    if (name && *name) {
        // Once per name, so a warning here is a bounded list rather than
        // a stream, and it is the only trace of a gap as it happens.
        LOG_WARNING("[Lua] missing API called: ", name);
        missingApiNames().insert(name);
    }
    return 0;
}


// --- WoW Utility Functions ---

// strsplit(delimiter, str) - WoW's string split

LuaEngine::LuaEngine() = default;

LuaEngine::~LuaEngine() {
    shutdown();
}

bool LuaEngine::initialize() {
    if (L_) return true;

    L_ = luaL_newstate();
    if (!L_) {
        LOG_ERROR("LuaEngine: failed to create Lua state");
        return false;
    }

    // Open safe standard libraries (no io, os, debug, package)
    luaopen_base(L_);
    luaopen_table(L_);
    luaopen_string(L_);
    luaopen_math(L_);

    // Remove unsafe globals from base library.
    //
    // newproxy is not among them, despite the name. It returns a userdata with
    // a fresh metatable and reaches nothing else; what it buys is __index and
    // __newindex on a value that cannot be tampered with, which is exactly how
    // Blizzard's own RestrictedFrames builds secure frame handles. Removing it
    // cost us SecureHandlerTemplates and everything inheriting from it.
    const char* unsafeGlobals[] = {
        "dofile", "loadfile", "load", "collectgarbage", nullptr
    };
    for (const char** g = unsafeGlobals; *g; ++g) {
        lua_pushnil(L_);
        lua_setglobal(L_, *g);
    }

    // Publish the widget tree before any API is registered, so a script that
    // runs during registration still finds it.
    lua_pushlightuserdata(L_, &widgets_);
    lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_widget_tree");

    // The engine itself, for the few bindings that need to do more than touch a
    // widget - taking focus fires handlers on the frame losing it as well as
    // the one gaining it, and only the engine knows which that was.
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_lua_engine");

    registerCoreAPI();
    registerEventAPI();

    // Last, so every bootstrap block above reads a _G that answers honestly.
    installMissingApiFallback();

    LOG_INFO("LuaEngine: initialized (Lua 5.1)");
    return true;
}

void LuaEngine::shutdown() {
    // Inside the guard, not above it. The report asks _G whether each recorded
    // name is still absent, so it needs the state it is asking about. Shutdown
    // runs twice on the way out - AddonManager's destructor calls it, and then
    // destroying the engine member calls it again - and the second pass found
    // a closed state and dereferenced it.
    if (L_) {
        reportMissingApi();
        // Before the state goes: the cursor bridge holds this pointer so the
        // client's own windows can ask what FrameXML is carrying, and calling
        // through it after the close would be reading a freed state.
        ui::frameXmlSetCursorBridge(nullptr, nullptr);
        lua_close(L_);
        L_ = nullptr;
        LOG_INFO("LuaEngine: shut down");
    }
    bindingPresses_.clear();
}

void LuaEngine::setGameHandler(game::GameHandler* handler) {
    gameHandler_ = handler;
    if (L_) {
        lua_pushlightuserdata(L_, handler);
        lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_game_handler");
    }
}

void LuaEngine::setLuaServices(const LuaServices& services) {
    luaServices_ = services;
    if (L_) {
        lua_pushlightuserdata(L_, &luaServices_);
        lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_lua_services");
    }
}


// The frame method table, at file scope so registerFrameGlobals can put these
// back over the Lua block that runs between the two. It was a function-local
// static and a lambda beside it, which worked only while both halves lived in
// the same 2,712-line function.
// Defined with the other edit-box bindings further down; declared here because
// the table below refers to them first. Outside the anonymous namespace, which
// is where their definitions are - inside it these would be three different
// functions with internal linkage and no bodies.
int lua_EditBox_SetFocus(lua_State* L);
int lua_EditBox_ClearFocus(lua_State* L);
int lua_EditBox_HasFocus(lua_State* L);

namespace {



}  // namespace

// The client's own Lua surface, in the order it has to be built.
//
// This was one 2,712-line function. The order between the parts is load-bearing
// in two places and is now stated by the call list rather than by position in a
// wall of text: the widget metatable exists before the Lua that writes onto it,
// and the C methods go back on afterwards.
void LuaEngine::registerCoreAPI() {
    registerBaseGlobals();
    registerWidgetMethods();
    registerWidgetStubLua();
    registerFrameGlobals();
    registerAddonCompatLua();
    registerWidgetSupportLua();
    registerUiCompatLua();
    registerAddonUtilityLua();
}

/// print, the __Wowee* seams, the WoW-flavoured standard library, and the
/// per-domain registration calls.
///
/// WoW's Lua predates 5.1's module tables and exposes most of math and
/// string as bare globals, and its trigonometric globals work in degrees.
/// FrameXML is written against that, not against stock 5.1.
void LuaEngine::registerBaseGlobals() {
    // Override print() to go to chat
    lua_pushcfunction(L_, lua_wow_print);
    lua_setglobal(L_, "print");

    lua_pushcfunction(L_, lua_wowee_warn);
    lua_setglobal(L_, "__WoweeWarn");

    // The micro menu's game-menu button reaches this client's own settings.
    // GameMenuFrame is suppressed, so ToggleGameMenu had nothing to show and
    // the button did nothing at all; which interface owns that panel is a
    // decision rather than a gap, and this is the decision.
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, [](lua_State* L) -> int {
        auto* self = static_cast<LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
        if (self && self->openSettingsCallbackRef()) self->openSettingsCallbackRef()();
        return 0;
    }, 1);
    lua_setglobal(L_, "__WoweeOpenClientSettings");

    // Run the deferred OnTextChanged queue now rather than at the next frame.
    //
    // The frame loop drains this; the headless runner has no frame loop, and
    // the behaviour it guards - MoneyInputFrame absorbing its own edits - is
    // exactly the kind that is only visible once the drain has happened. A
    // seam in the same __Wowee* idiom as the rest.
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, [](lua_State* L) -> int {
        auto* self = static_cast<LuaEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
        if (self) self->drainPendingTextChanged();
        return 0;
    }, 1);
    lua_setglobal(L_, "__WoweeDrainTextChanged");

    lua_pushcfunction(L_, lua_wowee_setAnimOffset);
    lua_setglobal(L_, "__WoweeSetAnimOffset");

    lua_pushcfunction(L_, lua_WoweeFireOnLoad);
    lua_setglobal(L_, "__WoweeFireOnLoad");

    lua_pushcfunction(L_, lua_GetMouseFocus);
    lua_setglobal(L_, "GetMouseFocus");


    lua_pushcfunction(L_, [](lua_State* L) -> int {
        LOG_WARNING("[FrameXML] ", luaL_optstring(L, 1, ""));
        return 0;
    });
    lua_setglobal(L_, "__WoweeLogWarning");

    // WoW API stubs
    lua_pushcfunction(L_, lua_wow_message);
    lua_setglobal(L_, "message");

    // --- Per-domain Lua API registration ---
    registerUnitLuaAPI(L_);
    registerSpellLuaAPI(L_);
    registerInventoryLuaAPI(L_);
    registerQuestLuaAPI(L_);
    registerSocialLuaAPI(L_);
    registerSystemLuaAPI(L_);
    registerActionLuaAPI(L_);
    registerLfgLuaAPI(L_);
    registerSocketLuaAPI(L_);

    // WoW aliases
    lua_getglobal(L_, "string");
    lua_getfield(L_, -1, "format");
    lua_setglobal(L_, "format");
    lua_pop(L_, 1);  // pop string table

    // tinsert/tremove aliases
    lua_getglobal(L_, "table");
    lua_getfield(L_, -1, "insert");
    lua_setglobal(L_, "tinsert");
    lua_getfield(L_, -1, "remove");
    lua_setglobal(L_, "tremove");
    lua_pop(L_, 1);  // pop table

    // WoW's Lua predates the 5.1 module tables and exposes most of math, string
    // and table as bare globals as well. FrameXML calls min, ceil and PI
    // directly at file scope, and one nil there loses the whole file: mainmenubar
    // and spellbookframe each died on a single arithmetic name.
    //
    // Skipped rather than assumed where the vendored Lua lacks one - getn is
    // compiled out here, and setting a global to nil would be no better than
    // leaving it absent.
    struct Alias { const char* lib; const char* field; const char* global; };
    static constexpr Alias kAliases[] = {
        {"math", "abs", "abs"},        {"math", "ceil", "ceil"},
        {"math", "floor", "floor"},    {"math", "max", "max"},
        {"math", "min", "min"},        {"math", "fmod", "mod"},
        {"math", "sqrt", "sqrt"},      {"math", "random", "random"},
        {"math", "exp", "exp"},        {"math", "log", "log"},
        {"math", "log10", "log10"},    {"math", "fmod", "fmod"},
        {"math", "modf", "modf"},      {"math", "frexp", "frexp"},
        {"math", "ldexp", "ldexp"},    {"math", "huge", "huge"},
        {"math", "deg", "deg"},        {"math", "rad", "rad"},
        {"string", "gsub", "gsub"},    {"string", "sub", "strsub"},
        {"string", "len", "strlen"},   {"string", "upper", "strupper"},
        {"string", "lower", "strlower"}, {"string", "find", "strfind"},
        {"string", "rep", "strrep"},   {"string", "byte", "strbyte"},
        {"string", "char", "strchar"}, {"string", "match", "strmatch"},
        {"string", "gmatch", "gmatch"}, {"table", "sort", "sort"},
        {"table", "getn", "getn"},     {"table", "concat", "tconcat"},
    };
    for (const auto& a : kAliases) {
        lua_getglobal(L_, a.lib);
        if (lua_istable(L_, -1)) {
            lua_getfield(L_, -1, a.field);
            if (lua_isnil(L_, -1)) lua_pop(L_, 1);
            else lua_setglobal(L_, a.global);
        }
        lua_pop(L_, 1);
    }
    // The trigonometric globals, which take and answer **degrees**.
    //
    // Not aliases of math.cos and friends: WoW's bare globals work in degrees
    // where the library works in radians, and the interface relies on it
    // everywhere - `sin(elapsed*360)` for one cycle a second, `cos(degree)`,
    // `sin(fraction*90)`. Aliasing them straight across would have left every
    // one of those quietly wrong rather than absent, which is worse.
    //
    // They were missing outright, and the cost was not a wrong curve but a
    // dead subsystem: CombatText_FountainScroll calls cos(), so every floating
    // combat message raised, and after five consecutive failures the engine
    // unhooks an OnUpdate - so the scroll stopped, and every message queued
    // sat on screen for the rest of the session. That is exactly what
    // "entering/leaving combat messages never clear" was.
    executeString(
        "do\n"
        "  local m = math\n"
        "  local toRad = m.pi / 180\n"
        "  local toDeg = 180 / m.pi\n"
        "  cos   = function(x) return m.cos(x * toRad) end\n"
        "  sin   = function(x) return m.sin(x * toRad) end\n"
        "  tan   = function(x) return m.tan(x * toRad) end\n"
        "  acos  = function(x) return m.acos(x) * toDeg end\n"
        "  asin  = function(x) return m.asin(x) * toDeg end\n"
        "  atan  = function(x) return m.atan(x) * toDeg end\n"
        "  atan2 = function(y, x) return m.atan2(y, x) * toDeg end\n"
        "end\n");

    // The names WoW's own Lua carried and 5.1 does not.
    //
    // getglobal is how an interface written before 3.0 reaches a name it has
    // built - getglobal("PartyMemberFrame" .. i) - and 1.12's FrameXML uses it
    // wherever this one would write _G[name]. Written to go through _G rather
    // than rawget so that it behaves exactly like reading the global directly,
    // missing-API fallback and all: a lookup that answers a stand-in either
    // way is one behaviour to reason about rather than two.
    //
    // gfind is 5.0's name for gmatch. 5.1 renamed it and WoW's 1.12 and 2.4.3
    // interfaces ask for the old one, on the string library and as a bare
    // global. Defined for every interface rather than only those, because a
    // name 5.1 dropped is a name nothing here can be shadowing.
    bootstrap(
        "function getglobal(name) return _G[name] end\n"
        "function setglobal(name, value) _G[name] = value end\n"
        "string.gfind = string.gfind or string.gmatch\n"
        "gfind = string.gfind\n");

    // A constant, not a function, so the fallback's rule for SCREAMING_SNAKE
    // names leaves it nil and gametime.lua does arithmetic on nothing.
    lua_getglobal(L_, "math");
    lua_getfield(L_, -1, "pi");
    lua_setglobal(L_, "PI");
    lua_pop(L_, 1);

    // WoW-specific and not derivable from a standard library.
    bootstrap(
        "function strtrim(s, chars)\n"
        "  chars = chars or ' \\t\\r\\n'\n"
        "  local p = '[' .. chars:gsub('(%W)', '%%%1') .. ']'\n"
        "  return (s:gsub('^' .. p .. '*', ''):gsub(p .. '*$', ''))\n"
        "end\n");

    // SlashCmdList table - addons register slash commands here
    lua_newtable(L_);
    lua_setglobal(L_, "SlashCmdList");
}




/// What addons expect to find already loaded: XML templates, C_Timer,
/// DEFAULT_CHAT_FRAME, LibStub and CallbackHandler-1.0.
///
/// These are the standard implementations addons embed and expect globally,
/// not this client's own.
void LuaEngine::registerAddonCompatLua() {
    // Where XML templates land. A virtual frame compiles to a function that
    // replays itself onto a real frame, and inherits= calls it; both halves are
    // emitted by the FrameXML loader and meet here.
    bootstrap(
        "__WoweeTemplates = {}\n"
        "local reported = {}\n"
        "function __WoweeMissingTemplate(name)\n"
        "  if reported[name] then return end\n"
        "  reported[name] = true\n"
        "  -- Said once per template. A frame inheriting one that never loaded\n"
        "  -- still gets built, just without whatever the template gave it,\n"
        "  -- which is a much better outcome than refusing the whole file.\n"
        "  __WoweeLogWarning('missing XML template: ' .. tostring(name))\n"
        "end\n");

    // C_Timer implementation via Lua (uses OnUpdate internally)
    bootstrap(
        "C_Timer = {}\n"
        "local timers = {}\n"
        "local timerFrame = CreateFrame('Frame', '__WoweeTimerFrame')\n"
        "timerFrame:SetScript('OnUpdate', function(self, elapsed)\n"
        "    local i = 1\n"
        "    while i <= #timers do\n"
        "        timers[i].remaining = timers[i].remaining - elapsed\n"
        "        if timers[i].remaining <= 0 then\n"
        "            local cb = timers[i].callback\n"
        "            table.remove(timers, i)\n"
        "            cb()\n"
        "        else\n"
        "            i = i + 1\n"
        "        end\n"
        "    end\n"
        "    if #timers == 0 then self:Hide() end\n"
        "end)\n"
        "timerFrame:Hide()\n"
        "function C_Timer.After(seconds, callback)\n"
        "    tinsert(timers, {remaining = seconds, callback = callback})\n"
        "    timerFrame:Show()\n"
        "end\n"
        "function C_Timer.NewTicker(seconds, callback, iterations)\n"
        "    local count = 0\n"
        "    local maxIter = iterations or -1\n"
        "    local ticker = {cancelled = false}\n"
        "    local function tick()\n"
        "        if ticker.cancelled then return end\n"
        "        count = count + 1\n"
        "        callback(ticker)\n"
        "        if maxIter > 0 and count >= maxIter then return end\n"
        "        C_Timer.After(seconds, tick)\n"
        "    end\n"
        "    C_Timer.After(seconds, tick)\n"
        "    function ticker:Cancel() self.cancelled = true end\n"
        "    return ticker\n"
        "end\n"
    );

    // DEFAULT_CHAT_FRAME with AddMessage method (used by many addons)
    bootstrap(
        "DEFAULT_CHAT_FRAME = {}\n"
        "function DEFAULT_CHAT_FRAME:AddMessage(text, r, g, b)\n"
        "    if r and g and b then\n"
        "        local hex = format('|cff%02x%02x%02x', "
        "            math.floor(r*255), math.floor(g*255), math.floor(b*255))\n"
        "        print(hex .. tostring(text) .. '|r')\n"
        "    else\n"
        "        print(tostring(text))\n"
        "    end\n"
        "end\n"
        "ChatFrame1 = DEFAULT_CHAT_FRAME\n"
        // Kept under a name FrameXML will not take, because chatframe.lua and
        // ChatFrame1's own OnLoad both reassign DEFAULT_CHAT_FRAME. See the
        // redirect in AddonManager::loadFrameXml for what it is kept for.
        "__WoweeClientChatAddMessage = DEFAULT_CHAT_FRAME.AddMessage\n"
    );

    // hooksecurefunc - hook a function to run additional code after it
    bootstrap(
        "function hooksecurefunc(tblOrName, nameOrFunc, funcOrNil)\n"
        "    local tbl, name, hook\n"
        "    if type(tblOrName) == 'table' then\n"
        "        tbl, name, hook = tblOrName, nameOrFunc, funcOrNil\n"
        "    else\n"
        "        tbl, name, hook = _G, tblOrName, nameOrFunc\n"
        "    end\n"
        "    local orig = tbl[name]\n"
        "    if type(orig) ~= 'function' then return end\n"
        "    tbl[name] = function(...)\n"
        "        local r = {orig(...)}\n"
        "        hook(...)\n"
        "        return unpack(r)\n"
        "    end\n"
        "end\n"
    );

    // LibStub - universal library version management used by Ace3 and virtually all addon libs.
    // This is the standard WoW LibStub implementation that addons embed/expect globally.
    bootstrap(
        // rawget, so the missing-API fallback cannot answer this. Read through
        // the metatable, "LibStub or {}" is never nil - it is the fallback
        // object - and the shim then hangs its tables off that instead of a
        // fresh one, so every library registering against it dies indexing a
        // field that was never really there.
        "local LibStub = rawget(_G, 'LibStub') or {}\n"
        "LibStub.libs = LibStub.libs or {}\n"
        "LibStub.minors = LibStub.minors or {}\n"
        "function LibStub:NewLibrary(major, minor)\n"
        "    assert(type(major) == 'string', 'LibStub:NewLibrary: bad argument #1 (string expected)')\n"
        "    minor = assert(tonumber(minor or (type(minor) == 'string' and minor:match('(%d+)'))), 'LibStub:NewLibrary: bad argument #2 (number expected)')\n"
        "    local oldMinor = self.minors[major]\n"
        "    if oldMinor and oldMinor >= minor then return nil end\n"
        "    local lib = self.libs[major] or {}\n"
        "    self.libs[major] = lib\n"
        "    self.minors[major] = minor\n"
        "    return lib, oldMinor\n"
        "end\n"
        "function LibStub:GetLibrary(major, silent)\n"
        "    if not self.libs[major] and not silent then\n"
        "        error('Cannot find a library instance of \"' .. tostring(major) .. '\".')\n"
        "    end\n"
        "    return self.libs[major], self.minors[major]\n"
        "end\n"
        "function LibStub:IterateLibraries() return pairs(self.libs) end\n"
        "setmetatable(LibStub, { __call = LibStub.GetLibrary })\n"
        "_G['LibStub'] = LibStub\n"
    );

    // CallbackHandler-1.0 - minimal implementation for Ace3-based addons
    bootstrap(
        "if LibStub then\n"
        "  local CBH = LibStub:NewLibrary('CallbackHandler-1.0', 7)\n"
        "  if CBH then\n"
        "    CBH.mixins = { 'RegisterCallback', 'UnregisterCallback', 'UnregisterAllCallbacks', 'Fire' }\n"
        "    function CBH:New(target, regName, unregName, unregAllName, onUsed)\n"
        "      local registry = setmetatable({}, { __index = CBH })\n"
        "      registry.callbacks = {}\n"
        "      target = target or {}\n"
        "      target[regName or 'RegisterCallback'] = function(self, event, method, ...)\n"
        "        if not registry.callbacks[event] then registry.callbacks[event] = {} end\n"
        "        local handler = type(method) == 'function' and method or self[method]\n"
        "        registry.callbacks[event][self] = handler\n"
        "      end\n"
        "      target[unregName or 'UnregisterCallback'] = function(self, event)\n"
        "        if registry.callbacks[event] then registry.callbacks[event][self] = nil end\n"
        "      end\n"
        "      target[unregAllName or 'UnregisterAllCallbacks'] = function(self)\n"
        "        for event, handlers in pairs(registry.callbacks) do handlers[self] = nil end\n"
        "      end\n"
        "      registry.Fire = function(self, event, ...)\n"
        "        if not self.callbacks[event] then return end\n"
        "        for obj, handler in pairs(self.callbacks[event]) do\n"
        "          handler(obj, event, ...)\n"
        "        end\n"
        "      end\n"
        "      return registry\n"
        "    end\n"
        "  end\n"
        "end\n"
    );

}

/// The no-op stubs, the tooltip builders, and the money and coin formatters.
///
/// Ordering matters against registerWidgetMethods: anything bound in C there
/// must not appear here, because this runs afterwards and would overwrite a
/// working method with a no-op that still answers - the failure EnableMouse
/// hit, where no frame took the mouse however plainly the call read.
void LuaEngine::registerWidgetSupportLua() {
    // Noop stubs for commonly called functions that don't need implementation
    bootstrap(
        // Empty a table in place, keeping the table itself. WoW's, not Lua's -
        // it exists as both a global and table.wipe, and FrameXML calls it 21
        // times. Missing, BuffFrame_Update errored on its first line and no
        // buff button was ever created.
        // The global already exists as a binding; only the table form was
        // missing, and FrameXML calls both.
        // Positional format specifiers, which this Lua does not have.
        //
        // The client's format accepts "%2$s" - argument two, whatever its
        // place in the string - and the interface leans on it hard: 189 uses
        // of %2$s, 184 of %4$s, 154 of %1$s, nearly all of them combat log
        // lines in GlobalStrings. Stock Lua 5.1 answers "invalid option '%$'
        // to 'format'" and raises, so every one of those took down whatever
        // was building the line. The combat log raised on its own first entry.
        //
        // Rewritten rather than reimplemented: the specifiers are stripped to
        // plain ones and the arguments put in the order they asked for, then
        // the real format does the work. A string with no positional specifier
        // takes the original path untouched.
        "do\n"
        "    local rawformat = string.format\n"
        "    local function positional(fmt, ...)\n"
        "        if type(fmt) ~= 'string' or not fmt:find('%%%d+%$') then\n"
        "            return rawformat(fmt, ...)\n"
        "        end\n"
        // A literal %% is not the start of a specifier, so it is put aside
        // before the scan and restored after - otherwise "%%2$s" would be read
        // as an argument reference and eat the escape.
        "        local ESC = '\\1'\n"
        "        local work = fmt:gsub('%%%%', ESC)\n"
        "        local order = {}\n"
        "        work = work:gsub('%%(%d+)%$', function(n)\n"
        "            order[#order + 1] = tonumber(n)\n"
        "            return '%'\n"
        "        end)\n"
        "        work = work:gsub(ESC, '%%%%')\n"
        "        local args = {...}\n"
        "        local picked = {}\n"
        "        for i = 1, #order do picked[i] = args[order[i]] end\n"
        // Guarded like the rest of the formatting here: a format string and
        // its arguments disagreeing is an error, and losing the line is
        // better than losing the frame that was writing it.
        "        local ok, out = pcall(rawformat, work, unpack(picked, 1, #order))\n"
        "        return ok and out or fmt\n"
        "    end\n"
        "    string.format = positional\n"
        "    format = positional\n"
        "end\n"
        "table.wipe = wipe\n"
        "function SetDesaturation() end\n"
        // A class circle rather than the 3D portrait the real client renders,
        // which needs a model rendered to a texture. The coordinates come from
        // FrameXML's own CLASS_ICON_TCOORDS, read when called so it does not
        // matter that this is defined before that table exists.
        // Captured before being replaced, and called rather than discarded.
        //
        // This bootstrap runs after the bindings are registered, so defining
        // the name here won and the binding underneath it never ran again.
        // That binding is not a stub: it marks the frame as one to fill with
        // the player's rendered face, which application.cpp does every frame
        // from widgets.playerPortraits(). The list stayed empty, so the five
        // frames in FrameXML that name the player wore a class circle while
        // the code that would have given them a face had nothing to iterate.
        //
        // PlayerPortrait itself was never affected - that one is found by name
        // and filled directly - which is why the circle looked deliberate
        // everywhere it appeared.
        "local markPortrait = SetPortraitTexture\n"
        "function SetPortraitTexture(texture, unit)\n"
        "    if type(texture) ~= 'table' then return end\n"
        "    if markPortrait then markPortrait(texture, unit) end\n"
        // The player's face is a real render and is assigned every frame, so
        // stamping a class circle over it would only fight that assignment.
        "    if unit and strlower(unit) == 'player' then return end\n"
        "    local _, class = UnitClass(unit or 'player')\n"
        "    local coords = class and CLASS_ICON_TCOORDS and CLASS_ICON_TCOORDS[class]\n"
        "    if coords then\n"
        "        texture:SetTexture('Interface\\\\TargetingFrame\\\\UI-Classes-Circles')\n"
        "        texture:SetTexCoord(coords[1], coords[2], coords[3], coords[4])\n"
        // Nothing at all when the class is not known yet, rather than the
        // placeholder. This runs at world entry now that events reach frames,
        // which is before the player's entity resolves - so it answered
        // "Unknown", stamped the placeholder shield over the portrait, and
        // never ran again to correct it.
        "    end\n"
        "end\n"
        "function StopSound() end\n"
        "function UIParent_OnEvent() end\n"
        // Filling the screen, not sitting at a point on it. The widget tree's
        // root is already the screen, and a frame created with no anchors falls
        // to the centre-on-parent default with no size - so every frame
        // FrameXML hangs off UIParent inherited a zero-size box in the middle,
        // including its own UIParent, which fills this one. That is why the
        // player frame's name was drawn in the centre of the world.
        //
        // SetAllPoints with no argument fills the parent, which for these is
        // the root.
        "UIParent = CreateFrame('Frame', 'UIParent')\n"
        "UIParent:SetAllPoints()\n"
        "UIPanelWindows = {}\n"
        "WorldFrame = CreateFrame('Frame', 'WorldFrame')\n"
        "WorldFrame:SetAllPoints()\n"
        // Two frames the real client builds in C and the world map then uses
        // without checking. FrameXML says so itself, twice, in a comment
        // naming CWorldMap::CreatePlayerArrowFrame.
        //
        // Missing, the first use raises - and the first use is the third
        // statement of WorldMapFrame_OnLoad, so everything after it was lost:
        // the scale of WorldMapDetailFrame and WorldMapButton, the POI bounds,
        // the objective font metrics, and WorldMapFrame.numQuests, which later
        // code counts up from and cannot when it is nil.
        //
        // Empty is the honest shape for both. This client draws the player
        // arrow and the quest areas itself, so what these need to do is exist,
        // take their scale and frame level, and draw nothing.
        "PlayerArrowEffectFrame = CreateFrame('Frame', 'PlayerArrowEffectFrame')\n"
        // The battlefield minimap's own copy of that arrow, for the same
        // reason. Its OnLoad addresses it with no guard either - and unlike
        // the world map's, it was never made, so the only thing holding that
        // addon up was the fallback that answers an unknown global with a
        // stand-in. Turn the fallback off and Blizzard_BattlefieldMinimap was
        // the one addon in the interface that would not load; with this it is
        // none of them.
        "PlayerMiniArrowEffectFrame = CreateFrame('Frame', 'PlayerMiniArrowEffectFrame')\n"
        "WorldMapBlobFrame = CreateFrame('Frame', 'WorldMapBlobFrame')\n"
        // Only the methods with no implementation behind them. Show, Hide,
        // SetScale and SetFrameLevel are real widget methods, and defining
        // them here would put a no-op table field in front of each.
        "function WorldMapBlobFrame:DrawQuestBlob() end\n"
        "function WorldMapBlobFrame:SetFillAlpha() end\n"
        "function WorldMapBlobFrame:SetBorderAlpha() end\n"
        // Nil rather than zero: the caller reads it as "are there blob
        // tooltips to use instead of the quest log's own objectives", and
        // takes the quest log path when there are none. Zero is true in Lua
        // and would send it down the blob path with nothing in it.
        "function WorldMapBlobFrame:GetNumTooltips() return nil end\n"
        "function WorldMapBlobFrame:GetTooltipIndex(i) return i end\n"
        // GameTooltip: global tooltip frame used by virtually all addons
        // Created as a GameTooltip, not a Frame, so it is one as far as the
        // widget tree is concerned and its lines are drawn.
        //
        // The lines used to be kept in a table here, with SetOwner, AddLine,
        // AddDoubleLine, SetText and ClearLines defined directly on this
        // table - and a field on the table beats the metatable, so those five
        // shadowed the real implementations for the one tooltip that matters
        // most. They stored text nothing draws.
        "GameTooltip = CreateFrame('GameTooltip', 'GameTooltip')\n"
        "GameTooltip.__lines = {}\n"
        // SetHyperlinkCompareItem(link, index, shift, anchor) - the tooltip
        // that appears beside an item when Shift is held.
        //
        // GameTooltip_ShowCompareItem calls this on each of the three shopping
        // tooltips in turn and shows the ones that answer true. It was never
        // implemented, so every one answered nil and nothing was ever shown:
        // shift-hovering an item did nothing at all, silently.
        //
        // The index picks which of the equipped counterparts to show, which
        // matters only for the slots there are two of. A ring is compared
        // against both rings, a trinket against both trinkets, a one-hander
        // against main and off hand; everything else has one slot and answers
        // false for index 2.
        "__WoweeCompareSlots = {\n"
        "    INVTYPE_FINGER = {11, 12},\n"
        "    INVTYPE_TRINKET = {13, 14},\n"
        "    INVTYPE_WEAPON = {16, 17},\n"
        "    INVTYPE_HEAD = {1}, INVTYPE_NECK = {2}, INVTYPE_SHOULDER = {3},\n"
        "    INVTYPE_BODY = {4}, INVTYPE_CHEST = {5}, INVTYPE_ROBE = {5},\n"
        "    INVTYPE_WAIST = {6}, INVTYPE_LEGS = {7}, INVTYPE_FEET = {8},\n"
        "    INVTYPE_WRIST = {9}, INVTYPE_HAND = {10}, INVTYPE_CLOAK = {15},\n"
        "    INVTYPE_2HWEAPON = {16}, INVTYPE_WEAPONMAINHAND = {16},\n"
        "    INVTYPE_WEAPONOFFHAND = {17}, INVTYPE_HOLDABLE = {17},\n"
        "    INVTYPE_SHIELD = {17}, INVTYPE_RANGED = {18},\n"
        "    INVTYPE_RANGEDRIGHT = {18}, INVTYPE_THROWN = {18},\n"
        "    INVTYPE_RELIC = {18}, INVTYPE_TABARD = {19},\n"
        "}\n"
        "function __WoweeFrameMT:SetHyperlinkCompareItem(link, index, shift, anchor)\n"
        "    self:ClearLines()\n"
        "    if not link then return false end\n"
        "    local id = tonumber(link:match('item:(%d+)'))\n"
        "    if not id then return false end\n"
        "    local _, _, _, _, _, _, _, _, equipSlot = GetItemInfo(id)\n"
        "    if not equipSlot or equipSlot == '' then return false end\n"
        "    local slots = __WoweeCompareSlots[equipSlot]\n"
        "    if not slots then return false end\n"
        "    local slot = slots[index or 1]\n"
        "    if not slot then return false end\n"
        // Nothing worn there is not a comparison, it is an empty tooltip -
        // and answering true for one would show a blank box beside the item.
        "    local wornLink = GetInventoryItemLink('player', slot)\n"
        "    if not wornLink then return false end\n"
        "    local wornId = tonumber(wornLink:match('item:(%d+)'))\n"
        "    if not wornId then return false end\n"
        // Comparing something against itself says nothing. WoW leaves the
        // second tooltip off when the item is already the one worn.
        "    if wornId == id then return false end\n"
        "    if not _WoweePopulateItemTooltip(self, wornId) then return false end\n"
        "    self:Show()\n"
        "    return true\n"
        "end\n"
        "function __WoweeFrameMT:GetItem()\n"
        "    if self.__itemId and self.__itemId > 0 then\n"
        "        local name = GetItemInfo(self.__itemId)\n"
        "        local _, itemLink = GetItemInfo(self.__itemId)\n"
        "        return name, itemLink or ('|cffffffff|Hitem:'..self.__itemId..':0|h['..tostring(name)..']|h|r')\n"
        "    end\n"
        "    return nil\n"
        "end\n"
        "function __WoweeFrameMT:GetSpell()\n"
        "    if self.__spellId and self.__spellId > 0 then\n"
        "        local name = GetSpellInfo(self.__spellId)\n"
        "        return name, nil, self.__spellId\n"
        "    end\n"
        "    return nil\n"
        "end\n"
        "function __WoweeFrameMT:GetUnit() return nil end\n"
        // NumLines and GetText come from the widget itself, which is where
        // the lines now live.
        "function __WoweeFrameMT:SetUnitBuff(unit, index, filter)\n"
        "    self:ClearLines()\n"
        "    local name, rank, icon, count, debuffType, duration, expTime, caster, steal, consolidate, spellId = UnitBuff(unit, index, filter)\n"
        "    if name then\n"
        "        self:SetText(name, 1, 1, 1)\n"
        "        if duration and duration > 0 then\n"
        "            self:AddLine(string.format('%.0f sec remaining', expTime - GetTime()), 1, 1, 1)\n"
        "        end\n"
        "        self.__spellId = spellId\n"
        "    end\n"
        "end\n"
        // The one the buff frame actually calls. SetUnitBuff and
        // SetUnitDebuff were written and this was left in the no-op
        // allowlist, so every buff and debuff on the default buff frame
        // hovered to an empty tooltip - buffframe.lua reaches for SetUnitAura
        // in both its handlers and neither of the two that exist.
        // Three more that were left in the no-op allowlist while everything
        // they need was bound. Each is a hover that wrote nothing: a quest
        // reward in the questgiver window, its spell reward, and an item on
        // either side of a trade.
        //
        // They route through the item link rather than rebuilding a tooltip,
        // because SetHyperlink already knows how to render one and the links
        // are what the getters hand back.
        // Two more tooltip methods, and these RAISE rather than answering a
        // no-op - they are in neither the method table nor the allowlist, so
        // the fallback returns nil and the call takes its handler with it.
        // Hovering a trainer's spell, or the item in the auction sell slot.
        // Hovering an item in loot, at a merchant, or in the mail. All six
        // sat in the no-op allowlist while every getter they need was bound,
        // so the windows worked and nothing in them had a tooltip - the same
        // shape as SetUnitAura, and the reason the allowlist is worth
        // auditing rather than trusting.
        //
        // Each prefers the item link, because SetHyperlink already renders one
        // and a link is what these getters hand back.
        "function __WoweeFrameMT:SetLootItem(slot)\n"
        "    self:ClearLines()\n"
        "    local link = GetLootSlotLink and GetLootSlotLink(slot)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local _, name = GetLootSlotInfo(slot)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetLootRollItem(rollId)\n"
        "    self:ClearLines()\n"
        "    local link = GetLootRollItemLink and GetLootRollItemLink(rollId)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "end\n"
        "function __WoweeFrameMT:SetMerchantItem(index)\n"
        "    self:ClearLines()\n"
        "    local link = GetMerchantItemLink and GetMerchantItemLink(index)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name = GetMerchantItemInfo(index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetBuybackItem(index)\n"
        "    self:ClearLines()\n"
        "    local link = GetBuybackItemLink and GetBuybackItemLink(index)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name = GetBuybackItemInfo(index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetInboxItem(index, attachIndex)\n"
        "    self:ClearLines()\n"
        "    local link = GetInboxItemLink and GetInboxItemLink(index, attachIndex)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name = GetInboxItem(index, attachIndex)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetSendMailItem(index)\n"
        "    self:ClearLines()\n"
        "    local name = GetSendMailItem(index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        // A totem's tooltip is its spell's, and the totem bar is drawn by
        // whichever side owns it.
        "function __WoweeFrameMT:SetTotem(slot)\n"
        "    self:ClearLines()\n"
        "    local _, name, _, duration = GetTotemInfo(slot)\n"
        "    if not name or name == '' then return end\n"
        "    self:SetText(name, 1, 1, 1)\n"
        "    if duration and duration > 0 then\n"
        "        self:AddLine(string.format('%.0f sec remaining', duration), 1, 1, 1)\n"
        "    end\n"
        "end\n"
        "function __WoweeFrameMT:SetTrainerService(index)\n"
        "    self:ClearLines()\n"
        "    if not index then return end\n"
        "    local link = GetTrainerServiceItemLink and GetTrainerServiceItemLink(index)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name, subText = GetTrainerServiceInfo(index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "    if subText and subText ~= '' then self:AddLine(subText, 0.5, 0.5, 0.5) end\n"
        "end\n"
        "function __WoweeFrameMT:SetAuctionSellItem()\n"
        "    self:ClearLines()\n"
        "    local name, _, count, quality = GetAuctionSellItemInfo()\n"
        "    if not name then return end\n"
        "    local r, g, b = GetItemQualityColor(quality or 1)\n"
        "    self:SetText(name, r, g, b)\n"
        "    if count and count > 1 then self:AddLine(count .. ' in stack', 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetQuestLogItem(itemType, index)\n"
        "    self:ClearLines()\n"
        "    local link = GetQuestLogItemLink and GetQuestLogItemLink(itemType, index)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name = GetQuestItemInfo(itemType, index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetQuestItem(itemType, index)\n"
        "    self:ClearLines()\n"
        "    local link = GetQuestItemLink and GetQuestItemLink(itemType, index)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name = GetQuestItemInfo(itemType, index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetQuestRewardSpell()\n"
        "    self:ClearLines()\n"
        "    local _, _, _, name = GetRewardSpell()\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetTradePlayerItem(index)\n"
        "    self:ClearLines()\n"
        "    local link = GetTradePlayerItemLink and GetTradePlayerItemLink(index)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name = GetTradePlayerItemInfo(index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        // The other half of the trade window. SetTradePlayerItem was written
        // and its twin was not, so hovering your own offer named the item and
        // hovering theirs said nothing - an asymmetry rather than a decision.
        "function __WoweeFrameMT:SetTradeTargetItem(index)\n"
        "    self:ClearLines()\n"
        "    local link = GetTradeTargetItemLink and GetTradeTargetItemLink(index)\n"
        "    if link then return self:SetHyperlink(link) end\n"
        "    local name = GetTradeTargetItemInfo(index)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        // The stance bar's cooldown swirl. Written here rather than in C
        // because both halves already exist as bindings and neither is where
        // the other lives: the form tables are in lua_unit_api and the cooldown
        // arithmetic - start and duration wound back so the sweep does not
        // restart every time the bar asks - is in lua_spell_api. A third copy
        // of either is how the two would start to disagree.
        //
        // GetSpellCooldown takes a name, which is what makes this work across
        // ranks: every rank of Stealth or Travel Form shares one name, and the
        // form tables carry no spell id to look up instead.
        //
        // It answered a flat no-cooldown before, so a stance swapped on a
        // cooldown showed none - and the shapeshift bar is on screen the whole
        // time for five classes.
        "function GetShapeshiftFormCooldown(index)\n"
        "    local _, name = GetShapeshiftFormInfo(index)\n"
        "    if not name then return 0, 0, 1 end\n"
        "    return GetSpellCooldown(name)\n"
        "end\n"
        // The stance bar's tooltip. Every druid, warrior, rogue, priest and
        // death knight has this bar on screen the whole time, and hovering a
        // form said nothing at all.
        "function __WoweeFrameMT:SetShapeshift(index)\n"
        "    self:ClearLines()\n"
        "    local _, name, isActive = GetShapeshiftFormInfo(index)\n"
        "    if not name then return end\n"
        "    self:SetText(name, 1, 1, 1)\n"
        "    if isActive then self:AddLine(ACTIVE_PETS or 'Active', 0.5, 0.5, 0.5) end\n"
        "end\n"
        // What a vendor wants besides coin - badges, marks, a token. The
        // money frame draws one of these per cost item and the tooltip is the
        // only place the item is named.
        "function __WoweeFrameMT:SetMerchantCostItem(index, costIndex)\n"
        "    self:ClearLines()\n"
        "    local _, value, link = GetMerchantItemCostItem(index, costIndex)\n"
        "    if link then\n"
        "        self:SetHyperlink(link)\n"
        "        if value and value > 1 then self:AddLine('Required: ' .. value, 1, 1, 1) end\n"
        "    end\n"
        "end\n"
        // The reward icons on the dungeon-ready popup.
        // GetLFGDungeonRewardInfo answers a name, an icon and a count, so
        // this can say what the item is; the completion-reward twin cannot,
        // because GetLFGCompletionRewardItem's contract is a texture and a
        // quantity and there is no name behind it.
        "function __WoweeFrameMT:SetLFGDungeonReward(dungeonId, rewardIndex)\n"
        "    self:ClearLines()\n"
        "    local name, _, count = GetLFGDungeonRewardInfo(dungeonId, rewardIndex)\n"
        "    if not name or name == '' then return end\n"
        "    self:SetText(name, 1, 1, 1)\n"
        "    if count and count > 1 then self:AddLine('x' .. count, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetUnitAura(unit, index, filter)\n"
        "    self:ClearLines()\n"
        "    local name, rank, icon, count, debuffType, duration, expTime, caster, steal, consolidate, spellId = UnitAura(unit, index, filter)\n"
        "    if not name then return end\n"
        // Harmful auras name their school and are titled in red; a buff is
        // white. filter is what the caller asked for, debuffType is what came
        // back, and either is enough to tell them apart.
        "    local harmful = debuffType ~= nil or (filter and string.find(filter, 'HARMFUL'))\n"
        "    if harmful then self:SetText(name, 1, 0, 0) else self:SetText(name, 1, 1, 1) end\n"
        "    if debuffType then self:AddLine(debuffType, 0.5, 0.5, 0.5) end\n"
        "    if count and count > 1 then self:AddLine(count .. ' stacks', 1, 1, 1) end\n"
        "    if duration and duration > 0 and expTime then\n"
        "        self:AddLine(string.format('%.0f sec remaining', expTime - GetTime()), 1, 1, 1)\n"
        "    end\n"
        "    self.__spellId = spellId\n"
        "end\n"
        "function __WoweeFrameMT:SetUnitDebuff(unit, index, filter)\n"
        "    self:ClearLines()\n"
        "    local name, rank, icon, count, debuffType, duration, expTime, caster, steal, consolidate, spellId = UnitDebuff(unit, index, filter)\n"
        "    if name then\n"
        "        self:SetText(name, 1, 0, 0)\n"
        "        if debuffType then self:AddLine(debuffType, 0.5, 0.5, 0.5) end\n"
        "        self.__spellId = spellId\n"
        "    end\n"
        "end\n"
        // Shared item tooltip builder using GetItemInfo return values
        "function _WoweePopulateItemTooltip(self, itemId)\n"
        "    local name, itemLink, quality, iLevel, reqLevel, class, subclass, maxStack, equipSlot, texture, sellPrice = GetItemInfo(itemId)\n"
        "    if not name then return false end\n"
        "    local qColors = {[0]={0.62,0.62,0.62},[1]={1,1,1},[2]={0.12,1,0},[3]={0,0.44,0.87},[4]={0.64,0.21,0.93},[5]={1,0.5,0},[6]={0.9,0.8,0.5},[7]={0,0.8,1}}\n"
        "    local c = qColors[quality or 1] or {1,1,1}\n"
        "    self:SetText(name, c[1], c[2], c[3])\n"
        // Colorblind Mode names the quality instead of only colouring it.
        //
        // The colour is the whole of how an item's quality is told here, which
        // is exactly the assumption the setting exists to undo. It goes
        // directly under the name, in the quality colour, which is where the
        // real client puts it.
        "    if GetCVar(\'colorblindMode\') == \'1\' then\n"
        "        local qNames = {[0]=\'Poor\',[1]=\'Common\',[2]=\'Uncommon\',[3]=\'Rare\',\n"
        "                        [4]=\'Epic\',[5]=\'Legendary\',[6]=\'Artifact\',[7]=\'Heirloom\'}\n"
        "        local qn = qNames[quality or 1]\n"
        "        if qn then self:AddLine(qn, c[1], c[2], c[3]) end\n"
        "    end\n"
        // Item level, when the player asked for it. The panel offers the
        // switch and nothing read it, so the line was on every equipment
        // tooltip whatever it said - and its default was unset, so the box
        // would have shown itself unticked with the line on screen.
        "    -- Item level for equipment\n"
        "    if equipSlot and equipSlot ~= '' and iLevel and iLevel > 0\n"
        "       and GetCVar('showItemLevel') == '1' then\n"
        "        self:AddLine('Item Level '..iLevel, 1, 0.82, 0)\n"
        "    end\n"
        "    -- Equip slot and subclass on same line\n"
        "    if equipSlot and equipSlot ~= '' then\n"
        "        local slotNames = {INVTYPE_HEAD='Head',INVTYPE_NECK='Neck',INVTYPE_SHOULDER='Shoulder',\n"
        "            INVTYPE_CHEST='Chest',INVTYPE_WAIST='Waist',INVTYPE_LEGS='Legs',INVTYPE_FEET='Feet',\n"
        "            INVTYPE_WRIST='Wrist',INVTYPE_HAND='Hands',INVTYPE_FINGER='Finger',\n"
        "            INVTYPE_TRINKET='Trinket',INVTYPE_CLOAK='Back',INVTYPE_WEAPON='One-Hand',\n"
        "            INVTYPE_SHIELD='Off Hand',INVTYPE_2HWEAPON='Two-Hand',INVTYPE_RANGED='Ranged',\n"
        "            INVTYPE_WEAPONMAINHAND='Main Hand',INVTYPE_WEAPONOFFHAND='Off Hand',\n"
        "            INVTYPE_HOLDABLE='Held In Off-Hand',INVTYPE_TABARD='Tabard',INVTYPE_ROBE='Chest'}\n"
        "        local slotText = slotNames[equipSlot] or ''\n"
        "        local subText = (subclass and subclass ~= '') and subclass or ''\n"
        "        if slotText ~= '' or subText ~= '' then\n"
        "            self:AddDoubleLine(slotText, subText, 1,1,1, 1,1,1)\n"
        "        end\n"
        "    elseif class and class ~= '' then\n"
        "        self:AddLine(class, 1, 1, 1)\n"
        "    end\n"
        "    -- Fetch detailed stats from C side\n"
        "    local data = _GetItemTooltipData(itemId)\n"
        "    if data then\n"
        "        -- Bind type\n"
        "        if data.isHeroic then self:AddLine('Heroic', 0, 1, 0) end\n"
        "        if data.isUnique then self:AddLine('Unique', 1, 1, 1)\n"
        "        elseif data.isUniqueEquipped then self:AddLine('Unique-Equipped', 1, 1, 1) end\n"
        // The fourth copy of this table, and the one that cannot share: it is
        // Lua source compiled into the tooltip shim. It stops at 3, like the C++
        // copy beside it used to, so a quest item says nothing here about being
        // one. Left as it is rather than changed on inference - what FrameXML's
        // tooltip shows is a question about FrameXML.
        "        if data.bindType == 1 then self:AddLine('Binds when picked up', 1, 1, 1)\n"
        "        elseif data.bindType == 2 then self:AddLine('Binds when equipped', 1, 1, 1)\n"
        "        elseif data.bindType == 3 then self:AddLine('Binds when used', 1, 1, 1) end\n"
        "        -- Armor\n"
        "        if data.armor and data.armor > 0 then\n"
        "            self:AddLine(data.armor..' Armor', 1, 1, 1)\n"
        "        end\n"
        "        -- Weapon damage and speed\n"
        "        if data.damageMin and data.damageMax and data.damageMin > 0 then\n"
        "            local speed = (data.speed or 0) / 1000\n"
        "            if speed > 0 then\n"
        "                self:AddDoubleLine(string.format('%.0f - %.0f Damage', data.damageMin, data.damageMax), string.format('Speed %.2f', speed), 1,1,1, 1,1,1)\n"
        "                local dps = (data.damageMin + data.damageMax) / 2 / speed\n"
        "                self:AddLine(string.format('(%.1f damage per second)', dps), 1, 1, 1)\n"
        "            end\n"
        "        end\n"
        "        -- Stats\n"
        "        if data.stamina then self:AddLine('+'..data.stamina..' Stamina', 0, 1, 0) end\n"
        "        if data.strength then self:AddLine('+'..data.strength..' Strength', 0, 1, 0) end\n"
        "        if data.agility then self:AddLine('+'..data.agility..' Agility', 0, 1, 0) end\n"
        "        if data.intellect then self:AddLine('+'..data.intellect..' Intellect', 0, 1, 0) end\n"
        "        if data.spirit then self:AddLine('+'..data.spirit..' Spirit', 0, 1, 0) end\n"
        "        -- Extra stats (hit, crit, haste, AP, SP, etc.)\n"
        "        if data.extraStats then\n"
        "            local statNames = {[3]='Agility',[4]='Strength',[5]='Intellect',[6]='Spirit',[7]='Stamina',\n"
        "                [12]='Defense Rating',[13]='Dodge Rating',[14]='Parry Rating',[15]='Block Rating',\n"
        "                [16]='Melee Hit Rating',[17]='Ranged Hit Rating',[18]='Spell Hit Rating',\n"
        "                [19]='Melee Crit Rating',[20]='Ranged Crit Rating',[21]='Spell Crit Rating',\n"
        "                [28]='Melee Haste Rating',[29]='Ranged Haste Rating',[30]='Spell Haste Rating',\n"
        "                [31]='Hit Rating',[32]='Crit Rating',[36]='Haste Rating',\n"
        "                [33]='Resilience Rating',[34]='Attack Power',[35]='Spell Power',\n"
        "                [37]='Expertise Rating',[38]='Attack Power',[39]='Ranged Attack Power',\n"
        "                [43]='Mana per 5 sec.',[44]='Armor Penetration Rating',\n"
        "                [45]='Spell Power',[46]='Health per 5 sec.',[47]='Spell Penetration'}\n"
        "            for _, stat in ipairs(data.extraStats) do\n"
        "                local name = statNames[stat.type]\n"
        "                if name and stat.value ~= 0 then\n"
        "                    local prefix = stat.value > 0 and '+' or ''\n"
        "                    self:AddLine(prefix..stat.value..' '..name, 0, 1, 0)\n"
        "                end\n"
        "            end\n"
        "        end\n"
        "        -- Resistances\n"
        "        if data.fireRes and data.fireRes ~= 0 then self:AddLine('+'..data.fireRes..' Fire Resistance', 0, 1, 0) end\n"
        "        if data.natureRes and data.natureRes ~= 0 then self:AddLine('+'..data.natureRes..' Nature Resistance', 0, 1, 0) end\n"
        "        if data.frostRes and data.frostRes ~= 0 then self:AddLine('+'..data.frostRes..' Frost Resistance', 0, 1, 0) end\n"
        "        if data.shadowRes and data.shadowRes ~= 0 then self:AddLine('+'..data.shadowRes..' Shadow Resistance', 0, 1, 0) end\n"
        "        if data.arcaneRes and data.arcaneRes ~= 0 then self:AddLine('+'..data.arcaneRes..' Arcane Resistance', 0, 1, 0) end\n"
        "        -- Item spell effects (Use: / Equip: / Chance on Hit:)\n"
        "        if data.itemSpells then\n"
        "            local triggerLabels = {[0]='Use: ',[1]='Equip: ',[2]='Chance on hit: ',[5]=''}\n"
        "            for _, sp in ipairs(data.itemSpells) do\n"
        "                local label = triggerLabels[sp.trigger] or ''\n"
        "                local text = sp.description or sp.name or ''\n"
        "                if text ~= '' then\n"
        "                    self:AddLine(label .. text, 0, 1, 0)\n"
        "                end\n"
        "            end\n"
        "        end\n"
        "        -- Gem sockets\n"
        "        if data.sockets then\n"
        "            local socketNames = {[1]='Meta',[2]='Red',[4]='Yellow',[8]='Blue'}\n"
        "            for _, sock in ipairs(data.sockets) do\n"
        "                local colorName = socketNames[sock.color] or 'Prismatic'\n"
        "                self:AddLine('[' .. colorName .. ' Socket]', 0.5, 0.5, 0.5)\n"
        "            end\n"
        "        end\n"
        "        -- Required level\n"
        "        if data.requiredLevel and data.requiredLevel > 1 then\n"
        "            self:AddLine('Requires Level '..data.requiredLevel, 1, 1, 1)\n"
        "        end\n"
        "        -- Flavor text\n"
        "        if data.description then self:AddLine('\"'..data.description..'\"', 1, 0.82, 0) end\n"
        "        if data.startsQuest then self:AddLine('This Item Begins a Quest', 1, 0.82, 0) end\n"
        "    end\n"
        "    -- Sell price from GetItemInfo\n"
        // Through the one money formatter, which writes the coin's picture
        // rather than a letter. This split the copper up itself and appended
        // 'g', 's' and 'c' - a third copy of the same arithmetic, and the only
        // one left still writing letters.
        "    if sellPrice and sellPrice > 0 then\n"
        "        self:AddLine('Sell Price: '..GetCoinTextureString(sellPrice), 1, 1, 1)\n"
        "    end\n"
        "    self.__itemId = itemId\n"
        "    return true\n"
        "end\n"
        // No SetInventoryItem here. It was written on this metatable and won,
        // because the bootstrap runs after the C bindings are registered onto
        // the same table - and it answered false for any unit but the player,
        // so every slot of the inspect paperdoll showed nothing. The C binding
        // it was hiding resolves the unit and reads that player's inspected
        // item entries, and carries a comment about an earlier bug where the
        // unit was ignored and the *player's* own item was shown in its place.
        // Two implementations of one method, and the weaker one was winning by
        // load order alone. Found by tools/api_shadowing_check.py, which calls
        // this shape a fault and was right.
        "function __WoweeFrameMT:SetBagItem(bag, slot)\n"
        "    self:ClearLines()\n"
        "    local tex, count, locked, quality, readable, lootable, link = GetContainerItemInfo(bag, slot)\n"
        "    if not link then return end\n"
        "    local id = link:match('item:(%d+)')\n"
        "    if not id then return end\n"
        "    _WoweePopulateItemTooltip(self, tonumber(id))\n"
        "    self:_WoweeAppendItemEnchants(bag, slot)\n"
        "    if count and count > 1 then self:AddLine('Count: '..count, 0.5, 0.5, 0.5) end\n"
        "end\n"
        // The spellbook's tooltip. SpellButton_OnEnter calls this and nothing
        // answered it, so hovering any spell in the book showed nothing -
        // silently, because an unknown method gets a no-op rather than an
        // error. The spellbook is one of the default elements.
        //
        // The argument is a book slot, not a spell id, which is what
        // GetSpellBookItemInfo turns into one; SetSpellByID below already
        // builds the tooltip from there. Returning true matters: the caller is
        // `if ( GameTooltip:SetSpell(...) ) then self.UpdateTooltip = ... end`,
        // so a tooltip that draws but answers nothing never refreshes.
        //
        // The pet book is declined rather than guessed at. Its slots index the
        // pet's own spells and GetSpellBookItemInfo walks the player's tabs, so
        // answering from it would name the wrong spell; false sends the caller
        // down the branch it takes when there is nothing to show.
        "function __WoweeFrameMT:SetSpell(slot, bookType)\n"
        "    self:ClearLines()\n"
        "    if bookType == BOOKTYPE_PET then return false end\n"
        "    local _, spellId = GetSpellBookItemInfo(slot)\n"
        "    if not spellId or spellId == 0 then return false end\n"
        "    self:SetSpellByID(spellId)\n"
        "    return true\n"
        "end\n"
        // The pet bar's tooltip. PetActionButton_OnEnter calls this for any
        // button without its own tooltip text, which is every real ability the
        // pet has - so hovering one showed nothing at all. It did not raise:
        // an unknown method answers with a no-op, which is why an empty
        // tooltip was the symptom rather than an error.
        //
        // GetPetActionInfo already answers everything needed. isToken says the
        // name and subtext are global string keys rather than text, which is
        // how the commands and stances are named - Attack, Follow, Passive -
        // and printing the key instead of the string is what that flag exists
        // to prevent.
        "function __WoweeFrameMT:SetPetAction(slot)\n"
        "    self:ClearLines()\n"
        "    local name, subtext, _, isToken = GetPetActionInfo(slot)\n"
        "    if not name then return end\n"
        "    if isToken then\n"
        "        name = _G[name] or name\n"
        "        subtext = subtext and _G[subtext] or subtext\n"
        "    end\n"
        "    self:SetText(name, 1, 1, 1)\n"
        "    if subtext and subtext ~= '' then\n"
        "        self:AddLine(subtext, 0.5, 0.5, 0.5)\n"
        "    end\n"
        "end\n"
        // The possess bar's tooltip. PossessButton_OnEnter calls this for
        // every slot but the cancel one, so it is reachable the moment
        // GetPossessInfo answers for a slot other than two - and a widget
        // method that does not exist raises on hover rather than showing
        // nothing. It is bound now so enabling that slot later is a change to
        // one binding rather than two, and it answers from the same place the
        // button's icon came from.
        "function __WoweeFrameMT:SetPossession(slot)\n"
        "    self:ClearLines()\n"
        "    local _, name = GetPossessInfo(slot)\n"
        "    if name then self:SetText(name, 1, 1, 1) end\n"
        "end\n"
        "function __WoweeFrameMT:SetSpellByID(spellId)\n"
        "    self:ClearLines()\n"
        "    if not spellId or spellId == 0 then return end\n"
        // Nine values, in the client's order. This used to read the fourth as
        // a cast time, which is where the cost is - so every spell tooltip
        // printed its mana cost as a cast time in seconds.
        "    local name, rank, icon, _cost, _isFunnel, _powerType, castTime, minRange, maxRange = GetSpellInfo(spellId)\n"
        "    if name then\n"
        "        self:SetText(name, 1, 1, 1)\n"
        "        if rank and rank ~= '' then self:AddLine(rank, 0.5, 0.5, 0.5) end\n"
        // The cost comes from GetSpellInfo, which is where 3.3.5 puts it.
        //
        // This called GetSpellPowerCost and read two scalars off it. That
        // binding exists, but it answers the *retail* shape - a list of
        // tables, {{type=, cost=, name=}} - so `cost` was a table and
        // `cost > 0` raised "attempt to compare number with table" on every
        // spell hovered in the book. Two places holding one fact and
        // disagreeing about it; the guard `cost and` does not help, because a
        // table is perfectly truthy.
        //
        // GetSpellPowerCost is a Cataclysm API and nothing in this interface
        // calls it. GetSpellInfo's fourth and sixth values are the cost and
        // its power type, and they were already being captured here and
        // thrown away.
        "        -- Mana cost\n"
        "        local cost, costType = _cost, _powerType\n"
        "        if cost and cost > 0 then\n"
        "            local powerNames = {[0]='Mana',[1]='Rage',[2]='Focus',[3]='Energy',[6]='Runic Power'}\n"
        "            self:AddLine(cost..' '..(powerNames[costType] or 'Mana'), 1, 1, 1)\n"
        "        end\n"
        "        -- Range\n"
        "        if maxRange and maxRange > 0 then\n"
        "            self:AddDoubleLine(string.format('%.0f yd range', maxRange), '', 1,1,1, 1,1,1)\n"
        "        end\n"
        "        -- Cast time\n"
        "        if castTime and castTime > 0 then\n"
        "            self:AddDoubleLine(string.format('%.1f sec cast', castTime / 1000), '', 1,1,1, 1,1,1)\n"
        "        else\n"
        "            self:AddDoubleLine('Instant', '', 1,1,1, 1,1,1)\n"
        "        end\n"
        "        -- Description\n"
        "        local desc = GetSpellDescription(spellId)\n"
        "        if desc and desc ~= '' then\n"
        "            self:AddLine(desc, 1, 0.82, 0)\n"
        "        end\n"
        "        -- Cooldown\n"
        "        local start, dur = GetSpellCooldown(spellId)\n"
        "        if dur and dur > 0 then\n"
        "            local rem = start + dur - GetTime()\n"
        "            if rem > 0.1 then self:AddLine(string.format('%.0f sec cooldown', rem), 1, 0, 0) end\n"
        "        end\n"
        "        self.__spellId = spellId\n"
        "    end\n"
        "end\n"
        // Answers whether it filled anything, because the caller asks:
        // ActionButton_SetTooltip is `if (GameTooltip:SetAction(self.action))`,
        // and returning nothing sent every action button down its no-tooltip
        // branch however much the tooltip itself could do.
        "function __WoweeFrameMT:SetAction(slot)\n"
        "    self:ClearLines()\n"
        "    if not slot then return false end\n"
        "    local actionType, id = GetActionInfo(slot)\n"
        "    if actionType == 'spell' and id and id > 0 then\n"
        "        self:SetSpellByID(id)\n"
        "        return self:NumLines() > 0\n"
        "    elseif actionType == 'item' and id and id > 0 then\n"
        "        _WoweePopulateItemTooltip(self, id)\n"
        "        return self:NumLines() > 0\n"
        "    end\n"
        "    return false\n"
        "end\n"
        "function __WoweeFrameMT:FadeOut() end\n"
        // SetFrameStrata is a real binding; a no-op here would shadow it and
        // leave the tooltip in whatever stratum it inherited, under the frames
        // it is meant to sit above.
        // Not a no-op here, for the reason given directly above: SetClampedToScreen
        // is a real binding, and the empty one that used to sit on this line
        // shadowed it. Nothing then ever set the flag - so the clamp in
        // layoutWidget, which exists precisely to keep a tooltip anchored near
        // an edge from running off it, could not fire, because its condition
        // was never true. GameTooltipTemplate declares clampedToScreen="true"
        // and the emitter turns that into the very call that was being
        // swallowed.
        // On the frame metatable rather than on GameTooltip, because a tooltip
        // is not always that one - item comparison uses ShoppingTooltip1 and 2
        // - and a copy on the table itself would shadow this for no gain.
        // Named in full rather than through the local `mt`, which belongs to a
        // different bootstrap chunk: referring to it here left the chunk
        // failing to load, so these two never existed - and with them removed
        // from the no-op list at the same time, nothing answered IsOwned at
        // all. FrameXML calls it from CursorOnUpdate, so it raised every frame
        // until the device went down.
        "function __WoweeFrameMT:GetOwner() return rawget(self, '__owner') end\n"
        "function __WoweeFrameMT:IsOwned(f) return rawget(self, '__owner') == f end\n"
        // ShoppingTooltip: used by comparison tooltips
        "ShoppingTooltip1 = CreateFrame('Frame', 'ShoppingTooltip1')\n"
        "ShoppingTooltip2 = CreateFrame('Frame', 'ShoppingTooltip2')\n"
        // Error handling stubs (used by many addons)
        "local _errorHandler = function(err) return err end\n"
        "function geterrorhandler() return _errorHandler end\n"
        "function seterrorhandler(fn) if type(fn)=='function' then _errorHandler=fn end end\n"
        "function debugstack(start, count1, count2) return '' end\n"
        // A name is as valid as a function here, and FrameXML mostly passes a
        // name: UIDropDownMenu_Initialize does
        // securecall("UIDropDownMenu_InitializeHelper", frame), and the helper
        // is what sets UIDROPDOWNMENU_INIT_MENU and zeroes every list's
        // numButtons. Accepting only a function meant that call did nothing at
        // all, silently, and eight files died further on indexing what it
        // should have set.
        //
        // rawget, so a name this client does not have stays nil rather than
        // becoming the missing-API object, which is not callable as a function.
        "function securecall(fn, ...)\n"
        "    if type(fn) == 'string' then fn = rawget(_G, fn) end\n"
        "    if type(fn) == 'function' then return fn(...) end\n"
        "end\n"
        // WoW's own names for indexing the global table, which predate _G being
        // exposed and are still what a good deal of 3.3.5 code is written with.
        // Blizzard_DebugTools calls getglobal at file scope and failed to load
        // whole without it.
        "function getglobal(n) return _G[n] end\n"
        "function setglobal(n, v) _G[n] = v end\n"
        // The rest of the taint vocabulary, which is a no-op here for the same
        // reason issecure is: nothing in this client is tainted, so nothing
        // needs protecting from it. Both are called for their effect and their
        // return value respectively, and both were missing - forceinsecure at
        // nine call sites and scrub at ten.
        //
        // /dump found it. DevTools_DumpCommand calls forceinsecure() on its
        // first line, so the command raised rather than dumping - which only
        // became reachable at all once Blizzard_DebugTools started loading.
        "function forceinsecure() end\n"
        // scrub keeps what can cross the secure boundary and drops the rest:
        // strings, numbers and booleans pass, everything else becomes nil.
        // The count has to survive, because securehandlers.lua returns it
        // straight out of a handler - dropping a value would shift every
        // argument after it.
        "function scrub(...)\n"
        "    local n = select('#', ...)\n"
        "    local out = {}\n"
        "    for i = 1, n do\n"
        "        local v = select(i, ...)\n"
        "        local t = type(v)\n"
        "        if t == 'string' or t == 'number' or t == 'boolean' then out[i] = v end\n"
        "    end\n"
        "    return unpack(out, 1, n)\n"
        "end\n"
        // Iterating a table the secure way, which for our purposes is next.
        "SecureNext = next\n"
        // Secure, because nothing here is tainted.
        //
        // These answered false, which is the opposite of the premise written
        // above them: taint is what makes code insecure, no addon here taints
        // anything, so every call site is secure and WoW would say so. False
        // told the interface it was running tainted and sent it down the
        // defensive branch everywhere.
        //
        // The cost was a reported bug with no error behind it.
        // ActionButton_ShowGrid increments its counter only `if (issecure())`
        // and shows the button only once that counter reaches one - so the
        // empty slots of the action bar never appeared while something was on
        // the cursor. Dragging a spell to the bar therefore had nothing to
        // land on: the press went to MainMenuBar, which is the strip behind
        // them and takes no drop, and the spell stayed on the cursor. Bag to
        // bag worked the whole time, because those buttons are always shown.
        "function issecurevariable(...) return true end\n"
        "function issecure() return true end\n"
        // GetCVarBool wraps C-side GetCVar (registered in table) for boolean queries
        // Misc compatibility stubs
        // GetScreenWidth, GetScreenHeight, GetNumLootItems are now C functions
        // GetFramerate is now a C function
        "function IsLoggedIn() return true end\n"
        "function StaticPopup_Show() end\n"
        "function StaticPopup_Hide() end\n"
        // UI Panel management - Show/Hide standard WoW panels
        "UIPanelWindows = {}\n"
        "function ShowUIPanel(frame, force)\n"
        "    if frame and frame.Show then frame:Show() end\n"
        "end\n"
        "function HideUIPanel(frame)\n"
        "    if frame and frame.Hide then frame:Hide() end\n"
        "end\n"
        "function ToggleFrame(frame)\n"
        "    if frame then\n"
        "        if frame:IsShown() then frame:Hide() else frame:Show() end\n"
        "    end\n"
        "end\n"
        "function GetUIPanel(which) return nil end\n"
        "function CloseWindows(ignoreCenter) return false end\n"
        // TEXT localization stub - returns input string unchanged
        "function TEXT(text) return text end\n"
        // Faux scroll frame helpers (used by many list UIs)
        "function FauxScrollFrame_GetOffset(frame)\n"
        "    return frame and frame.offset or 0\n"
        "end\n"
        "function FauxScrollFrame_Update(frame, numItems, numVisible, valueStep, button, smallWidth, bigWidth, highlightFrame, smallHighlightWidth, bigHighlightWidth)\n"
        "    if not frame then return false end\n"
        "    frame.offset = frame.offset or 0\n"
        "    local showScrollBar = numItems > numVisible\n"
        "    return showScrollBar\n"
        "end\n"
        "function FauxScrollFrame_SetOffset(frame, offset)\n"
        "    if frame then frame.offset = offset or 0 end\n"
        "end\n"
        "function FauxScrollFrame_OnVerticalScroll(frame, value, itemHeight, updateFunction)\n"
        "    if not frame then return end\n"
        "    frame.offset = math.floor(value / (itemHeight or 1) + 0.5)\n"
        "    if updateFunction then updateFunction() end\n"
        "end\n"
        // SecureCmdOptionParse - parses conditional macros like [target=focus]
        "function SecureCmdOptionParse(options)\n"
        "    if not options then return nil end\n"
        "    -- Simple: return the unconditional fallback (text after last semicolon or the whole string)\n"
        "    local result = options:match(';%s*(.-)$') or options:match('^%[.*%]%s*(.-)$') or options\n"
        "    return result\n"
        "end\n"
        // ChatFrame message group stubs
        "function ChatFrame_AddMessageGroup(frame, group) end\n"
        "function ChatFrame_RemoveMessageGroup(frame, group) end\n"
        "function ChatFrame_AddChannel(frame, channel) end\n"
        "function ChatFrame_RemoveChannel(frame, channel) end\n"
        // CreateTexture/CreateFontString are now C frame methods in the metatable
        "do\n"
        "  local function cc(r,g,b)\n"
        "    local t = {r=r, g=g, b=b}\n"
        "    t.colorStr = string.format('%02x%02x%02x', math.floor(r*255+0.5), math.floor(g*255+0.5), math.floor(b*255+0.5))\n"
        "    function t:GenerateHexColor() return '|cff' .. self.colorStr end\n"
        "    function t:GenerateHexColorMarkup() return '|cff' .. self.colorStr end\n"
        "    return t\n"
        "  end\n"
        "  RAID_CLASS_COLORS = {}\n"
        "  __WoweeClassColor = cc\n"
        "end\n"
        // GetClassColor(className) - returns r, g, b, colorString
        //
        // The hex is computed rather than read off the entry. FrameXML's
        // constants.lua assigns RAID_CLASS_COLORS its own plain table, replacing
        // the one built above and the colorStr on every entry with it - so
        // reading the field answered nil for every class the moment the
        // interface loaded. The fallback's string was eight hex digits where the
        // success path's was six, which an addon writing '|cff' .. str renders
        // as a colour code with a stray f after it.
        "function GetClassColor(className)\n"
        "    local c = RAID_CLASS_COLORS and RAID_CLASS_COLORS[className]\n"
        "    if not c then return 1, 1, 1, 'ffffff' end\n"
        "    return c.r, c.g, c.b,\n"
        "        c.colorStr or string.format('%02x%02x%02x',\n"
        "            math.floor(c.r * 255 + 0.5), math.floor(c.g * 255 + 0.5), math.floor(c.b * 255 + 0.5))\n"
        "end\n"
        // QuestDifficultyColors table for quest level coloring
        "QuestDifficultyColors = {\n"
        "    impossible = {r=1.0,g=0.1,b=0.1,font='QuestDifficulty_Impossible'},\n"
        "    verydifficult = {r=1.0,g=0.5,b=0.25,font='QuestDifficulty_VeryDifficult'},\n"
        "    difficult = {r=1.0,g=1.0,b=0.0,font='QuestDifficulty_Difficult'},\n"
        "    standard = {r=0.25,g=0.75,b=0.25,font='QuestDifficulty_Standard'},\n"
        "    trivial = {r=0.5,g=0.5,b=0.5,font='QuestDifficulty_Trivial'},\n"
        "    header = {r=1.0,g=0.82,b=0.0,font='QuestDifficulty_Header'},\n"
        "}\n"
        // Money as WoW writes it: the amount and the coin's picture, not the
        // amount and a letter.
        //
        // This answered "19g 81s 56c", and that string is what put letters
        // beside the values wherever it is used - the backpack's money among
        // them. A real client writes each amount followed by an inline texture
        // escape, which is what GOLD_AMOUNT_TEXTURE and its two siblings in
        // globalstrings.lua spell out, and there is no letter anywhere in it.
        //
        // The letters are still what the colourblind setting asks for, and
        // GOLD_AMOUNT_SYMBOL is still where they live - this is not that.
        "function GetCoinTextureString(copper)\n"
        "    copper = math.floor(copper or 0)\n"
        "    local g = math.floor(copper / 10000)\n"
        "    local s = math.floor(math.fmod(copper, 10000) / 100)\n"
        "    local c = math.fmod(copper, 100)\n"
        "    local r = ''\n"
        "    if g > 0 then r = r .. format(GOLD_AMOUNT_TEXTURE, g, 0, 0) .. ' ' end\n"
        "    if s > 0 then r = r .. format(SILVER_AMOUNT_TEXTURE, s, 0, 0) .. ' ' end\n"
        "    if c > 0 or r == '' then r = r .. format(COPPER_AMOUNT_TEXTURE, c, 0, 0) end\n"
        "    return r\n"
        "end\n"
        "GetCoinText = GetCoinTextureString\n"
    );

}

/// RAID_CLASS_COLORS, the dropdown framework, UISpecialFrames, and the
/// C_ChatInfo table.
///
/// RAID_CLASS_COLORS is filled from the same table this client colours a
/// name with rather than written out a second time in Lua: two copies that
/// agree are one class recolour away from a party list and a chat line
/// disagreeing about the same player.
void LuaEngine::registerUiCompatLua() {
    // RAID_CLASS_COLORS, filled from the same table this client colours a name
    // with rather than written out a second time in Lua. The two copies did
    // agree, which is the only reason it was never a bug; a class recoloured on
    // one side and not the other would have shown as a party list and a chat
    // line disagreeing about the same player.
    {
        std::string classColors = "do local cc = __WoweeClassColor\n";
        for (uint8_t classId = 1; classId < std::size(kLuaClassTokens); ++classId) {
            const char* token = kLuaClassTokens[classId];
            if (!token || !*token) continue;  // 10 is unused, and 0 is no class
            const ImVec4 c = ui::getClassColor(classId);
            char line[160];
            std::snprintf(line, sizeof(line), "RAID_CLASS_COLORS.%s = cc(%.6g,%.6g,%.6g)\n",
                          token, c.x, c.y, c.z);
            classColors += line;
        }
        classColors += "__WoweeClassColor = nil end\n";
        bootstrap(classColors.c_str());
    }

    // UIDropDownMenu framework - minimal compat for addons using dropdown menus
    bootstrap(
        "UIDROPDOWNMENU_MENU_LEVEL = 1\n"
        "UIDROPDOWNMENU_MENU_VALUE = nil\n"
        "UIDROPDOWNMENU_OPEN_MENU = nil\n"
        "local _ddMenuList = {}\n"
        "function UIDropDownMenu_Initialize(frame, initFunc, displayMode, level, menuList)\n"
        "    if frame then frame.__initFunc = initFunc end\n"
        "end\n"
        "function UIDropDownMenu_CreateInfo() return {} end\n"
        "function UIDropDownMenu_AddButton(info, level) table.insert(_ddMenuList, info) end\n"
        "function UIDropDownMenu_SetWidth(frame, width) end\n"
        "function UIDropDownMenu_SetButtonWidth(frame, width) end\n"
        "function UIDropDownMenu_SetText(frame, text)\n"
        "    if frame then frame.__text = text end\n"
        "end\n"
        "function UIDropDownMenu_GetText(frame)\n"
        "    return frame and frame.__text or ''\n"
        "end\n"
        "function UIDropDownMenu_SetSelectedID(frame, id) end\n"
        "function UIDropDownMenu_SetSelectedValue(frame, value) end\n"
        "function UIDropDownMenu_GetSelectedID(frame) return 1 end\n"
        "function UIDropDownMenu_GetSelectedValue(frame) return nil end\n"
        "function UIDropDownMenu_JustifyText(frame, justify) end\n"
        "function UIDropDownMenu_EnableDropDown(frame) end\n"
        "function UIDropDownMenu_DisableDropDown(frame) end\n"
        "function CloseDropDownMenus() end\n"
        "function ToggleDropDownMenu(level, value, frame, anchor, xOfs, yOfs) end\n"
    );

    // UISpecialFrames: frames in this list close on Escape key
    bootstrap(
        "UISpecialFrames = {}\n"
        // Shared font objects, carrying the height and colour a FontString takes
        // from them. They were empty tables, so inheriting one changed nothing
        // and every label came out the same size in the same colour - and
        // FrameXML inherits one more than three thousand times.
        //
        // The colours are Blizzard's: normal is the familiar gold, highlight is
        // white, disabled grey, and the quest fonts near-black on parchment.
        // ...and carrying the methods a font object answers, because FrameXML
        // asks a font object questions rather than reading its fields. The
        // options panels do it on every control they enable:
        //
        //     local fontObject = text:GetFontObject()
        //     text:SetTextColor(fontObject:GetTextColor())
        //
        // A bare table has no GetTextColor, so that indexes nil and the enable
        // path raises - for every checkbox, slider and dropdown in the options
        // panels, which is where a control most needs to come back to life.
        "local fontMT = { __index = {\n"
        "    GetTextColor = function(self) return self.r, self.g, self.b, self.a end,\n"
        "    SetTextColor = function(self, r, g, b, a)\n"
        "        self.r, self.g, self.b, self.a = r, g, b, a or 1\n"
        "    end,\n"
        "    GetFont = function(self)\n"
        "        return self.font or 'Fonts\\\\FRIZQT__.TTF', self.height, self.outline or ''\n"
        "    end,\n"
        "    SetFont = function(self, f, h, flags)\n"
        "        self.font, self.height, self.outline = f, h, flags\n"
        "    end,\n"
        "    GetFontHeight = function(self) return self.height end,\n"
        "    GetShadowColor = function(self) return 0, 0, 0, 1 end,\n"
        "    GetShadowOffset = function(self) return 0, 0 end,\n"
        "    GetSpacing = function(self) return 0 end,\n"
        "    GetJustifyH = function(self) return self.justifyH or 'LEFT' end,\n"
        "    GetJustifyV = function(self) return self.justifyV or 'MIDDLE' end,\n"
        "    GetObjectType = function(self) return 'Font' end,\n"
        // A font object is its own font object, which is what SetFontObject
        // being handed one relies on.
        "    GetFontObject = function(self) return self end,\n"
        "} }\n"
        // Exposed, because the emitter has to put it back. A <Font> in the
        // interface with inherits= is built as a fresh table copying the base's
        // fields, and pairs() carries no metatable - so fontstyles.xml
        // redefining GameFontNormal replaced the object below with one that
        // answers no methods at all, and every fontObject:GetTextColor() in the
        // options panels raised on a table with nothing behind it.
        "__WoweeFontMT = fontMT\n"
        "local function font(h, r, g, b)\n"
        "    return setmetatable({ height = h, r = r, g = g, b = b, a = 1 }, fontMT)\n"
        "end\n"
        "GameFontNormal            = font(12, 1.00, 0.82, 0.00)\n"
        "GameFontNormalSmall       = font(10, 1.00, 0.82, 0.00)\n"
        "GameFontNormalLarge       = font(16, 1.00, 0.82, 0.00)\n"
        "GameFontNormalHuge        = font(20, 1.00, 0.82, 0.00)\n"
        "GameFontHighlight         = font(12, 1.00, 1.00, 1.00)\n"
        "GameFontHighlightSmall    = font(10, 1.00, 1.00, 1.00)\n"
        "GameFontHighlightLarge    = font(16, 1.00, 1.00, 1.00)\n"
        "GameFontDisable           = font(12, 0.50, 0.50, 0.50)\n"
        "GameFontDisableSmall      = font(10, 0.50, 0.50, 0.50)\n"
        "GameFontDisableLarge      = font(16, 0.50, 0.50, 0.50)\n"
        "GameFontWhite             = font(12, 1.00, 1.00, 1.00)\n"
        "GameFontRed               = font(12, 1.00, 0.13, 0.13)\n"
        "GameFontGreen             = font(12, 0.13, 1.00, 0.13)\n"
        "NumberFontNormal          = font(12, 1.00, 1.00, 1.00)\n"
        "NumberFontNormalSmall     = font(10, 1.00, 1.00, 1.00)\n"
        "NumberFontNormalLarge     = font(16, 1.00, 1.00, 1.00)\n"
        "ChatFontNormal            = font(12, 1.00, 1.00, 1.00)\n"
        "SystemFont                = font(12, 1.00, 0.82, 0.00)\n"
        "SystemFontSmall           = font(10, 1.00, 0.82, 0.00)\n"
        "QuestFont                 = font(13, 0.18, 0.12, 0.06)\n"
        "QuestFontNormalSmall      = font(11, 0.18, 0.12, 0.06)\n"
        "QuestTitleFont            = font(15, 0.00, 0.00, 0.00)\n"
        "Tooltip_Med               = font(12, 1.00, 1.00, 1.00)\n"
        "Tooltip_Small             = font(10, 1.00, 1.00, 1.00)\n"
        // InterfaceOptionsFrame: addons register settings panels here
        "InterfaceOptionsFrame = CreateFrame('Frame', 'InterfaceOptionsFrame')\n"

        "InterfaceOptionsFramePanelContainer = CreateFrame('Frame', 'InterfaceOptionsFramePanelContainer')\n"
        "function InterfaceOptions_AddCategory(panel) end\n"
        "function InterfaceOptionsFrame_OpenToCategory(panel) end\n"
        // Commonly expected global tables
        "SLASH_RELOAD1 = '/reload'\n"
        "SLASH_RELOADUI1 = '/reloadui'\n"
        "GRAY_FONT_COLOR = {r=0.5,g=0.5,b=0.5}\n"
        "NORMAL_FONT_COLOR = {r=1.0,g=0.82,b=0.0}\n"
        "HIGHLIGHT_FONT_COLOR = {r=1.0,g=1.0,b=1.0}\n"
        "GREEN_FONT_COLOR = {r=0.1,g=1.0,b=0.1}\n"
        "RED_FONT_COLOR = {r=1.0,g=0.1,b=0.1}\n"
        // C_ChatInfo - addon message prefix API used by some addons
        "C_ChatInfo = C_ChatInfo or {}\n"
        "C_ChatInfo.RegisterAddonMessagePrefix = RegisterAddonMessagePrefix\n"
        "C_ChatInfo.IsAddonMessagePrefixRegistered = IsAddonMessagePrefixRegistered\n"
        "C_ChatInfo.SendAddonMessage = SendAddonMessage\n"
    );

}

/// Action bar constants, the tContains/tInvert family, and the bit library.
///
/// Named by WoW rather than by Lua - tContains is not table.contains - so
/// they are what an addon written against the real client will call.
void LuaEngine::registerAddonUtilityLua() {
    // Action bar constants and functions used by action bar addons
    bootstrap(
        "NUM_ACTIONBAR_BUTTONS = 12\n"
        "NUM_ACTIONBAR_PAGES = 6\n"
        "ACTION_BUTTON_SHOW_GRID_REASON_CVAR = 1\n"
        "ACTION_BUTTON_SHOW_GRID_REASON_EVENT = 2\n"
        // Action bar page tracking
        // GetActionBarPage and ChangeActionBarPage are bindings, and they
        // share their storage there. A second pair here against a local of its
        // own meant the two could disagree about what page the bar was on.
        // These names have real bindings, registered before this runs. A stub
        // here does not sit beside one - it replaces it, because the bootstrap
        // is later. GetPetActionInfo, GetNumShapeshiftForms and the rest had
        // working implementations that never ran once.
        //
        // Binding functions
        "function GetCurrentBindingSet() return 1 end\n"
        // Macro functions
        "function GetMacroBody(id) return nil end\n"
        "function GetMacroIndexByName(name) return 0 end\n"
        // Stance bar
        // Pet action bar
        "NUM_PET_ACTION_SLOTS = 10\n"
        // Common WoW constants used by many addons
        "MAX_TALENT_TABS = 3\n"
        // Values taken from the shipped FrameXML, which is the authority for
        // 3.3.5 - talentframebase.lua and spellbookframe.lua respectively.
        //
        // These are pre-set here so an addon has them before the interface
        // loads, and on master the interface does not load at all, so these
        // are the only values there are. Both book types were numbers where
        // 3.3.5 uses strings, which is the quietest kind of wrong: every
        // comparison against them is false rather than an error, so a
        // spellbook that asked "is this the pet book" always heard no.
        "MAX_NUM_TALENTS = 40\n"
        "BOOKTYPE_SPELL = 'spell'\n"
        "BOOKTYPE_PET = 'pet'\n"
        "MAX_PARTY_MEMBERS = 4\n"
        "MAX_RAID_MEMBERS = 40\n"
        "MAX_ARENA_TEAMS = 3\n"
        "INVSLOT_FIRST_EQUIPPED = 1\n"
        "INVSLOT_LAST_EQUIPPED = 19\n"
        "NUM_BAG_SLOTS = 4\n"
        "NUM_BANKBAGSLOTS = 7\n"
        // 19, from constants.lua, where it is described as what PutItemInBag
        // adds to a bag index. Zero made that arithmetic name the backpack.
        "CONTAINER_BAG_OFFSET = 19\n"
        "MAX_SKILLLINE_TABS = 8\n"
        "TRADE_ENCHANT_SLOT = 7\n"
        "function GetPetActionsUsable() return false end\n"
    );

    // WoW table/string utility functions used by many addons
    bootstrap(
        // Table utilities
        "function tContains(tbl, item)\n"
        "    for _, v in pairs(tbl) do if v == item then return true end end\n"
        "    return false\n"
        "end\n"
        "function tInvert(tbl)\n"
        "    local inv = {}\n"
        "    for k, v in pairs(tbl) do inv[v] = k end\n"
        "    return inv\n"
        "end\n"
        "function CopyTable(src)\n"
        "    if type(src) ~= 'table' then return src end\n"
        "    local copy = {}\n"
        "    for k, v in pairs(src) do copy[k] = CopyTable(v) end\n"
        "    return setmetatable(copy, getmetatable(src))\n"
        "end\n"
        "function tDeleteItem(tbl, item)\n"
        "    for i = #tbl, 1, -1 do if tbl[i] == item then table.remove(tbl, i) end end\n"
        "end\n"
        // Mixin pattern - used by modern addons for OOP-style object creation
        "function Mixin(obj, ...)\n"
        "    for i = 1, select('#', ...) do\n"
        "        local mixin = select(i, ...)\n"
        "        for k, v in pairs(mixin) do obj[k] = v end\n"
        "    end\n"
        "    return obj\n"
        "end\n"
        "function CreateFromMixins(...)\n"
        "    return Mixin({}, ...)\n"
        "end\n"
        "function CreateAndInitFromMixin(mixin, ...)\n"
        "    local obj = CreateFromMixins(mixin)\n"
        "    if obj.Init then obj:Init(...) end\n"
        "    return obj\n"
        "end\n"
        "function MergeTable(dest, src)\n"
        "    for k, v in pairs(src) do dest[k] = v end\n"
        "    return dest\n"
        "end\n"
        // String utilities (WoW globals that alias Lua string functions)
        "strupper = string.upper\n"
        "strlower = string.lower\n"
        "strfind = string.find\n"
        "strsub = string.sub\n"
        "strlen = string.len\n"
        "strrep = string.rep\n"
        "strbyte = string.byte\n"
        "strchar = string.char\n"
        "strgfind = string.gmatch\n"
        "function tostringall(...)\n"
        "    local n = select('#', ...)\n"
        "    if n == 0 then return end\n"
        "    local r = {}\n"
        "    for i = 1, n do r[i] = tostring(select(i, ...)) end\n"
        "    return unpack(r, 1, n)\n"
        "end\n"
        "strrev = string.reverse\n"
        "gsub = string.gsub\n"
        "gmatch = string.gmatch\n"
        "strjoin = function(delim, ...)\n"
        "    return table.concat({...}, delim)\n"
        "end\n"
        // Math utilities
        "function Clamp(val, lo, hi) return math.min(math.max(val, lo), hi) end\n"
        "function Round(val) return math.floor(val + 0.5) end\n"
        // Bit operations (WoW provides these; Lua 5.1 doesn't have native bit ops)
        "bit = bit or {}\n"
        "bit.band = bit.band or function(a, b) local r,m=0,1 for i=0,31 do if a%2==1 and b%2==1 then r=r+m end a=math.floor(a/2) b=math.floor(b/2) m=m*2 end return r end\n"
        "bit.bor = bit.bor or function(a, b) local r,m=0,1 for i=0,31 do if a%2==1 or b%2==1 then r=r+m end a=math.floor(a/2) b=math.floor(b/2) m=m*2 end return r end\n"
        "bit.bxor = bit.bxor or function(a, b) local r,m=0,1 for i=0,31 do if (a%2==1)~=(b%2==1) then r=r+m end a=math.floor(a/2) b=math.floor(b/2) m=m*2 end return r end\n"
        "bit.bnot = bit.bnot or function(a) return 4294967295 - a end\n"
        "bit.lshift = bit.lshift or function(a, n) return a * (2^n) end\n"
        "bit.rshift = bit.rshift or function(a, n) return math.floor(a / (2^n)) end\n"
    );
}

// ---- Event System ----
// Lua-side: WoweeEvents table holds { ["EVENT_NAME"] = { handler1, handler2, ... } }
// RegisterEvent("EVENT", handler) adds a handler function
// UnregisterEvent("EVENT", handler) removes it


static int lua_RegisterEvent(lua_State* L) {
    const char* eventName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Get or create the WoweeEvents table
    lua_getglobal(L, "__WoweeEvents");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "__WoweeEvents");
    }

    // Get or create the handler list for this event
    lua_getfield(L, -1, eventName);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, eventName);
    }

    // Append the handler function to the list
    int len = static_cast<int>(lua_objlen(L, -1));
    lua_pushvalue(L, 2);  // push the handler function
    lua_rawseti(L, -2, len + 1);

    lua_pop(L, 2);  // pop handler list + WoweeEvents
    return 0;
}

static int lua_UnregisterEvent(lua_State* L) {
    const char* eventName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_getglobal(L, "__WoweeEvents");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return 0; }

    lua_getfield(L, -1, eventName);
    if (lua_isnil(L, -1)) { lua_pop(L, 2); return 0; }

    // Remove matching handler from the list
    int len = static_cast<int>(lua_objlen(L, -1));
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_rawequal(L, -1, 2)) {
            lua_pop(L, 1);
            // Shift remaining elements down
            for (int j = i; j < len; j++) {
                lua_rawgeti(L, -1, j + 1);
                lua_rawseti(L, -2, j);
            }
            lua_pushnil(L);
            lua_rawseti(L, -2, len);
            break;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    return 0;
}

void LuaEngine::registerEventAPI() {
    lua_pushcfunction(L_, lua_RegisterEvent);
    lua_setglobal(L_, "RegisterEvent");

    lua_pushcfunction(L_, lua_UnregisterEvent);
    lua_setglobal(L_, "UnregisterEvent");

    // Create the events table
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweeEvents");
}

namespace {
/// Pushes an event argument with the type WoW gives it.
///
/// Every argument crosses this boundary as a string, and some of them are
/// numbers on the other side: ChatFrame_MessageEventHandler does
/// `if (arg8 > 0)`, and comparing a string with a number is not false in Lua,
/// it is an error that takes the handler down - which for chat is every
/// message. Only a plain integer converts, so a name, a unit id and a hex guid
/// all stay strings.
void pushEventArg(lua_State* L, const std::string& arg) {
    // The one argument that is not text: see kEventNil. A false boolean has no
    // spelling as a string, and Lua treats every string and every number as
    // true, so without this an event could not carry one.
    if (arg == wowee::game::kEventNil) { lua_pushnil(L); return; }
    if (!arg.empty() && arg.size() < 12) {
        size_t at = (arg[0] == '-') ? 1 : 0;
        if (at < arg.size()) {
            bool digits = true;
            // One decimal point is still a number. UPDATE_TICKET carries ages
            // and wait times measured in days, all of them fractions of one,
            // and the help frame compares them against zero - as a string that
            // is the same error this exists to avoid, not a wrong answer.
            int points = 0;
            for (size_t i = at; i < arg.size(); ++i) {
                if (arg[i] == '.' && points == 0 && i != at && i + 1 < arg.size()) {
                    ++points;
                    continue;
                }
                if (arg[i] < '0' || arg[i] > '9') { digits = false; break; }
            }
            // "007" is not a number anyone meant; a leading zero is a string,
            // except the one in front of a decimal point.
            const bool canonical = digits &&
                (arg.size() - at == 1 || arg[at] != '0' ||
                 (points == 1 && arg[at + 1] == '.'));
            if (canonical) {
                lua_pushnumber(L, std::stod(arg));
                return;
            }
        }
    }
    lua_pushstring(L, arg.c_str());
}
}  // namespace

void LuaEngine::fireEvent(const std::string& eventName,
                           const std::vector<std::string>& args) {
    if (!L_) return;

    // The trade skill list may change shape now, and only now. It is held still
    // between these events because the frame reads it many times to draw one
    // selection and its shape depends on item data that arrives while those
    // reads run - see tradeSkillRows in lua_quest_api.cpp. Released here so a
    // late reply lands at a redraw rather than between two lookups of the same
    // index.
    if (eventName == "TRADE_SKILL_UPDATE" || eventName == "TRADE_SKILL_SHOW") {
        invalidateTradeSkillRows();
    }

    // An event handler may cause another event, which is ordinary and has to
    // keep working - but a cycle between two of them recurses through both this
    // stack and Lua's, inside one frame, until the process dies. Reporting a
    // script error used to be such a cycle: the report fired an event, the
    // handler for it errored, and the error was reported the same way.
    //
    // Deep enough that no legitimate chain reaches it, and it says which event
    // it stopped, because the name is the only clue to which cycle it was.
    constexpr int kMaxEventDepth = 8;
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& v) : d(v) { ++d; }
        ~DepthGuard() { --d; }
    } depthGuard{eventDepth_};
    if (eventDepth_ > kMaxEventDepth) {
        LOG_WARNING("Event '", eventName, "' is ", eventDepth_,
                    " deep and was dropped - handlers are triggering each other");
        return;
    }

    // Addon-side handlers, where there are any.
    //
    // Their absence is not a reason to stop. FrameXML registers through
    // frame:RegisterEvent, which fills __WoweeFrameEvents - a different table
    // entirely - and returning here meant every event no addon happened to
    // want was never delivered to the interface at all. PLAYER_TARGET_CHANGED
    // is one: fifty-four frames were registered for it, the client fired it,
    // and not one of them ever heard it, which is why the target frame stayed
    // hidden with a target that existed and resolved.
    lua_getglobal(L_, "__WoweeEvents");
    if (lua_istable(L_, -1)) {
        lua_getfield(L_, -1, eventName.c_str());
        if (lua_istable(L_, -1)) {
            int handlerCount = static_cast<int>(lua_objlen(L_, -1));
            // Iterate a copy, for the same reason the frame dispatch below
            // does: a handler may unregister while it runs, UnregisterEvent
            // shifts the tail down, and the handler that moved into the
            // vacated index is then stepped over and never hears the event.
            // The fix was made there and not here.
            lua_createtable(L_, handlerCount, 0);
            for (int i = 1; i <= handlerCount; ++i) {
                lua_rawgeti(L_, -2, i);
                lua_rawseti(L_, -2, i);
            }
            lua_remove(L_, -2);   // drop the live list; the copy stands in
            for (int i = 1; i <= handlerCount; i++) {
                lua_rawgeti(L_, -1, i);
                if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); continue; }

                // Push arguments: event name first, then extra args
                lua_pushstring(L_, eventName.c_str());
                for (const auto& arg : args) {
                    pushEventArg(L_, arg);
                }

                int nargs = 1 + static_cast<int>(args.size());
                if (lua_pcall(L_, nargs, 0, 0) != 0) {
                    const char* err = lua_tostring(L_, -1);
                    std::string errStr = err ? err : "(unknown)";
                    LOG_ERROR("LuaEngine: event '", eventName, "' handler error: ", errStr);
                    noteLuaError(errStr);
        if (luaErrorCallback_) luaErrorCallback_(errStr);
                    lua_pop(L_, 1);
                }
            }
        }
        lua_pop(L_, 1);   // handler list, or whatever was there instead
    }
    lua_pop(L_, 1);       // __WoweeEvents, or whatever was there instead

    // Also dispatch to frames that registered for this event via frame:RegisterEvent()
    //
    // WOWEE_EVENT_TRACE names events to report, comma separated. An event that
    // does not arrive and an event nobody listens for look identical from
    // outside - the frame simply does not change - and they need opposite
    // fixes, so the count of frames that received it is the thing worth
    // knowing. Reported every time, because these are rare enough to read and
    // the ones worth tracing are the ones that are not arriving.
    static const std::set<std::string> traced = [] {
        std::set<std::string> out;
        const char* raw = std::getenv("WOWEE_EVENT_TRACE");
        if (!raw || !*raw) return out;
        std::string v(raw);
        size_t at = 0;
        while (at <= v.size()) {
            const size_t comma = v.find(',', at);
            std::string one = v.substr(at, comma == std::string::npos
                                               ? std::string::npos : comma - at);
            if (!one.empty()) out.insert(one);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return out;
    }();

    lua_getglobal(L_, "__WoweeFrameEvents");
    if (lua_istable(L_, -1)) {
        lua_getfield(L_, -1, eventName.c_str());
        if (lua_istable(L_, -1)) {
            int frameCount = static_cast<int>(lua_objlen(L_, -1));
            if (traced.count(eventName)) {
                LOG_WARNING("EventTrace: ", eventName, " (",
                            args.empty() ? "" : args[0], ") reached ",
                            frameCount, " frames");
            }
            // Iterate a copy, because a handler is allowed to unregister while
            // it runs and several do - answering an event by deciding you no
            // longer want it is ordinary. UnregisterEvent shifts the tail down,
            // so the frame that moved into the vacated index was stepped over
            // and never heard that event at all. The list is a handful of
            // entries, and this only copies references.
            lua_createtable(L_, frameCount, 0);
            for (int i = 1; i <= frameCount; ++i) {
                lua_rawgeti(L_, -2, i);
                lua_rawseti(L_, -2, i);
            }
            lua_remove(L_, -2);   // drop the live list; the copy stands in
            for (int i = 1; i <= frameCount; i++) {
                lua_rawgeti(L_, -1, i);
                if (!lua_istable(L_, -1)) { lua_pop(L_, 1); continue; }

                // Get the frame's OnEvent script
                lua_getfield(L_, -1, "__scripts");
                if (lua_istable(L_, -1)) {
                    lua_getfield(L_, -1, "OnEvent");
                    if (lua_isfunction(L_, -1)) {
                        lua_pushvalue(L_, -3);  // self (frame)
                        lua_pushstring(L_, eventName.c_str());
                        for (const auto& arg : args) pushEventArg(L_, arg);
                        int nargs = 2 + static_cast<int>(args.size());
                        if (pcallScript(L_, "OnEvent", nargs, 0) != 0) {
                            const char* ferr = lua_tostring(L_, -1);
                            std::string ferrStr = ferr ? ferr : "(unknown)";
                            LOG_ERROR("LuaEngine: frame OnEvent error: ", ferrStr);
                            noteLuaError(ferrStr);
        if (luaErrorCallback_) luaErrorCallback_(ferrStr);
                            lua_pop(L_, 1);
                        }
                    } else {
                        lua_pop(L_, 1); // pop non-function
                    }
                }
                lua_pop(L_, 2); // pop __scripts + frame
            }
        } else if (traced.count(eventName)) {
            LOG_WARNING("EventTrace: ", eventName, " (",
                        args.empty() ? "" : args[0],
                        ") - no frame has registered for it");
        }
        lua_pop(L_, 1); // pop event frame list
    }
    lua_pop(L_, 1); // pop __WoweeFrameEvents
}

namespace {
/// Defined with the other pcall helpers further down; declared here because
/// callFrameScript needs it and comes first.
int luaTracebackHandler(lua_State* L);
}  // namespace

/// "OnClick on GameMenuFrame" - what a recorded error needs to be actionable.
///
/// The traceback names the file and line inside FrameXML, which says what
/// broke but not what was being poked when it broke. A raise in a shared
/// handler is the common case and the frame is the only thing that
/// distinguishes one caller from another.
std::string scriptOrigin(const wowee::ui::WidgetTree& widgets, uint32_t wid,
                         const char* script) {
    std::string out = script ? script : "script";
    if (const auto* w = widgets.get(wid); w && !w->name.empty()) {
        out += " on ";
        out += w->name;
    }
    return out;
}

int LuaEngine::beginFrameScript(uint32_t wid, const char* script) {
    if (!L_ || wid == 0) return 0;
    lua_getglobal(L_, "__WoweeFramesByWid");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return 0; }
    lua_pushinteger(L_, static_cast<lua_Integer>(wid));
    lua_rawget(L_, -2);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 2); return 0; }

    lua_getfield(L_, -1, "__scripts");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 3); return 0; }
    // The traceback handler has to sit below the function it is handling for,
    // so it goes on before the script is fetched. A handler that fails now says
    // where it was called from, the same as one that fails during the load.
    lua_pushcfunction(L_, luaTracebackHandler);
    const int handlerIdx = lua_gettop(L_);
    lua_getfield(L_, handlerIdx - 1, script);
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 5); return 0; }

    lua_pushvalue(L_, handlerIdx - 2);  // self
    return handlerIdx;
}

void LuaEngine::finishFrameScript(uint32_t wid, const char* script,
                                  int handlerIdx, int nargs) {
    if (pcallScript(L_, script, nargs, handlerIdx) != 0) {
        const char* err = lua_tostring(L_, -1);
        const std::string where = scriptOrigin(widgets_, wid, script);
        LOG_ERROR("LuaEngine: ", where, " error: ", err ? err : "?");
        noteLuaError(where + ": " + (err ? err : "script error"));
        if (luaErrorCallback_) luaErrorCallback_(err ? err : "script error");
        lua_pop(L_, 1);
    }
    // Four, not three: the traceback handler is still below.
    lua_pop(L_, 4);
}

void LuaEngine::callFrameScript(uint32_t wid, const char* script,
                                const char* arg) {
    const int handlerIdx = beginFrameScript(wid, script);
    if (handlerIdx == 0) return;
    int nargs = 1;
    if (arg) { lua_pushstring(L_, arg); ++nargs; }
    finishFrameScript(wid, script, handlerIdx, nargs);
}


bool LuaEngine::frameHasScript(uint32_t wid, const char* script) {
    if (!L_ || wid == 0) return false;
    lua_getglobal(L_, "__WoweeFramesByWid");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }
    lua_pushinteger(L_, static_cast<lua_Integer>(wid));
    lua_rawget(L_, -2);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 2); return false; }
    lua_getfield(L_, -1, "__scripts");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 3); return false; }
    lua_getfield(L_, -1, script);
    const bool has = lua_isfunction(L_, -1);
    lua_pop(L_, 4);
    return has;
}

void LuaEngine::callFrameScript3(uint32_t wid, const char* script,
                                 const char* a, const char* b, const char* c) {
    const int handlerIdx = beginFrameScript(wid, script);
    if (handlerIdx == 0) return;
    lua_pushstring(L_, a ? a : "");
    lua_pushstring(L_, b ? b : "");
    lua_pushstring(L_, c ? c : "");
    finishFrameScript(wid, script, handlerIdx, 4);
}

/// Where the caret sits, as the interface's own scrolling edit boxes want it.
///
/// WoW fires OnCursorChanged(self, x, y, width, height) whenever the caret
/// moves, and ScrollingEdit_OnCursorChanged is the only thing that ever sets
/// `self.cursorOffset`. Nothing here fired it, so that field stayed nil - and
/// ScrollingEdit_OnTextChanged, which every multi-line box in the interface
/// calls, ends by running ScrollingEdit_OnUpdate, which opens with
///
///     cursorOffset = -self.cursorOffset;
///
/// So the first character typed into the mail body, the macro editor or the
/// guild information box raised there, before anything was drawn.
///
/// y is negative going down and is the only value the interface reads besides
/// the height: it scrolls the frame to keep the caret's line in view. Measured
/// in lines rather than in laid-out pixels, because a line is what the caret
/// moves by and the box has one height for all of them.
void LuaEngine::fireCursorChanged(uint32_t wid) {
    const auto* w = widgets_.get(wid);
    if (!w || !w->isEditBox) return;
    const size_t upTo = std::min(w->cursorPos, w->editText.size());
    size_t line = 0;
    for (size_t i = 0; i < upTo; ++i) {
        if (w->editText[i] == '\n') ++line;
    }
    const double lineH = wowee::ui::interfaceFontSize(w->fontHeight);
    callFrameScript4Numbers(wid, "OnCursorChanged", 0.0,
                            -static_cast<double>(line) * lineH, 0.0, lineH);
}

void LuaEngine::callFrameScript4Numbers(uint32_t wid, const char* script,
                                        double a, double b, double c, double d) {
    const int handlerIdx = beginFrameScript(wid, script);
    if (handlerIdx == 0) return;
    lua_pushnumber(L_, a);
    lua_pushnumber(L_, b);
    lua_pushnumber(L_, c);
    lua_pushnumber(L_, d);
    finishFrameScript(wid, script, handlerIdx, 5);
}

void LuaEngine::callFrameScriptNumber(uint32_t wid, const char* script, double arg) {
    const int handlerIdx = beginFrameScript(wid, script);
    if (handlerIdx == 0) return;
    // A number, not a numeric string: OnMouseWheel bodies compare the delta
    // against zero, and Lua raises on comparing a string with a number even
    // where it would happily add them.
    lua_pushnumber(L_, arg);
    finishFrameScript(wid, script, handlerIdx, 2);
}

void LuaEngine::callFrameScriptColor(uint32_t wid, const char* script,
                                     const float rgb[3]) {
    const int handlerIdx = beginFrameScript(wid, script);
    if (handlerIdx == 0) return;
    // r, g and b as three arguments, which is what OnColorSelect names them:
    // colorpickerframe.xml's body is ColorSwatch:SetTexture(r, g, b).
    for (int i = 0; i < 3; ++i) lua_pushnumber(L_, rgb[i]);
    finishFrameScript(wid, script, handlerIdx, 4);
}

void LuaEngine::installMissingApiFallback() {
    // Off unless asked for. With it on, every unknown global answers, so code
    // that checks whether a function exists before using it - which addons do
    // constantly - sees everything as present and takes branches meant for a
    // newer client. That is the right trade for bringing FrameXML up, where the
    // point is to get past a missing name and find out what actually matters,
    // and the wrong one for everyday addon loading.
    auto isSet = [](const char* name) {
        const char* v = std::getenv(name);
        return v && *v && std::string(v) != "0";
    };
    // Loading FrameXML implies it. FrameXML cannot get through its own load
    // without the fallback, so two separate switches where one is useless
    // without the other is only a way to be handed a wall of failures for
    // setting the obvious one.
    //
    // Said explicitly, though, the setting wins either way. The fallback is not
    // free - it makes every feature check read as present - and now that the
    // real gaps are closing it is worth being able to ask what it is still
    // buying, which needs a way to turn it off with FrameXML on.
    const char* explicitSetting = std::getenv("WOWEE_LUA_API_FALLBACK");
    const char* loadSetting = std::getenv("WOWEE_LOAD_FRAMEXML");
    const bool frameXmlOn = loadSetting ? (std::string(loadSetting) != "0") : true;
    const bool enabled = (explicitSetting && *explicitSetting)
                             ? std::string(explicitSetting) != "0"
                             : frameXmlOn;
    (void)isSet;
    if (!enabled) return;

    lua_pushcfunction(L_, lua_RecordMissingApi);
    lua_setglobal(L_, "__WoweeRecordMissingApi");

    // Counting functions answer zero rather than nothing.
    //
    // A missing name is usually survivable - the guard around it fails and the
    // branch behind it does not run. A missing count is not: FrameXML writes
    // `for id = 1, GetNumTrackingTypes() do`, and nil as a loop limit is not a
    // loop that runs no times, it is "'for' limit must be a number" and the
    // whole file is lost. That is exactly what took minimap.xml down the moment
    // the minimap started being built at all.
    //
    // Thirty-seven of these are called across FrameXML with nothing behind
    // them. Zero is the honest answer for a feature this client does not model
    // - no titles, no companions, no arena teams - and where it does model one,
    // a real implementation replaces the stub by simply existing: the loop
    // below skips any name already defined.
    //
    // Recorded under a "count:" prefix so they stay in the missing-API report.
    // Defining them would otherwise hide them from it, which is the one thing
    // that report is for.
    bootstrap(
        "local counting = {\n"
        "  'GetCurrencyListSize','GetFieldSize','GetInventoryItemCount',\n"
        "  'GetLFDLockPlayerCount','GetNumArenaTeamMembers',\n"

        "  'GetNumBattlefields','GetNumBuybackItems',\n"
        "  'GetNumCompanions',\n"
        "  'GetNumGuildBankTabs','GetNumGuildEvents','GetNumLanguages',\n"
        "  'GetNumMessages','GetNumMutes',\n"
        "  'GetNumPoints','GetNumQuestItemDrops','GetNumQuestItems',\n"
        "  'GetNumQuestLogRewardFactions',\n"
        "  'GetNumRandomDungeons',\n"
        "  'GetNumStationeries',\n"
        "  'GetNumTitles','GetNumTooltips','GetNumTrackingTypes',\n"
        "  'GetNumVoiceSessionMembersBySessionID',\n"
        "  'GetNumDungeonMapLevels','GetNumMapOverlays','GetNumVoiceSessions',\n"
        "  'BNGetNumConversationMembers',\n"
        // The ignore list asks for all three of these before it draws a row,
        // and compares each against zero without checking. There is no
        // Battle.net here, so none of them is an unknown quantity being guessed
        // at - nobody can have a Battle.net block or a pending invite, and zero
        // is what is true. Without them, opening the Ignore tab raised.
        "  'BNGetNumBlocked','BNGetNumBlockedToons','BNGetNumFriendInvites',\n"
        // Both are counts of something that cannot exist here: nothing tracks
        // achievements, and no battleground port is ever pending. The
        // achievement panel compares the first against its own limit before
        // adding a tracker, and the battlefield frame the second against zero.
        "  'GetNumTrackedAchievements','GetBattlefieldPortExpiration',\n"
        "}\n"
        "local told = {}\n"
        "for _, name in ipairs(counting) do\n"
        "  if rawget(_G, name) == nil then\n"
        "    _G[name] = function()\n"
        "      if not told[name] then\n"
        "        told[name] = true\n"
        "        __WoweeRecordMissingApi('count:' .. name)\n"
        "      end\n"
        "      return 0\n"
        "    end\n"
        "  end\n"
        "end\n");

    // A name in SCREAMING_SNAKE_CASE is a constant, and handing back a function
    // where a number or a string was wanted turns a missing value into a
    // confusing type error further away. Those stay nil. UpperCamelCase is a
    // function, and gets one that does nothing.
    bootstrap(
        // Callable, and every field of it is a method answering nil.
        //
        // A bare function was not enough. FrameXML looks frames up by name as
        // often as it calls functions - local t = _G[name.."PrefixText"] - and
        // it guards them properly, with if (t) then t:GetText(). A function
        // passes that guard and then dies on the indexing, so the correct check
        // was worse than no check at all: eleven files went down on that one
        // line. Answering nil from every method lets the guarded branch run and
        // come to nothing, which is what a missing frame should look like.
        // Methods answer; data fields do not.
        //
        // Answering everything made feature checks on a missing frame's own
        // state read as present: FCFMin_UpdateColors tests
        // minFrame.selectedColorTable and takes the branch that dereferences
        // it. The same convention the fallback already uses for names -
        // PascalCase is a method, anything else is data - applies inside the
        // object too, so a field is nil and the guard around it works.
        "local missing = setmetatable({}, {\n"
        "  __call = function() end,\n"
        "  __index = function(_, k)\n"
        "    if type(k) == 'string' and string.find(k, '^%u') then\n"
        "      return function() return nil end\n"
        "    end\n"
        "    return nil\n"
        "  end,\n"
        "})\n"
        "__WoweeLoadOnDemandFrames = {\n"
        "  AchievementFrame = true, ArenaEnemyFrames = true,\n"
        "  BattlefieldMinimap = true, BattlefieldMinimapTab = true,\n"
        "  KeyBindingFrame = true, MacroFrame = true, PlayerTalentFrame = true,\n"
        "  StopwatchTicker = true, TimeManagerClockButton = true,\n"
        "  TimeManagerFrame = true,\n"
        "}\n"
        "local seen = {}\n"
        "setmetatable(_G, { __index = function(_, k)\n"
        "  if type(k) ~= 'string' then return nil end\n"
        "  if not string.find(k, '^%u') then return nil end\n"
        "  if string.find(k, '^[A-Z][A-Z0-9_]*$') then return nil end\n"
        // A digit in the name means an instance, not an API function, and an
        // instance that does not exist must read as absent. FrameXML looks
        // frames up by building the name - _G["ChatFrame"..id.."Minimized"] -
        // and then guards the result properly with if (frame). Answering makes
        // that guard pass and the branch behind it runs against nothing.
        //
        // Measured rather than assumed: of the 4,100 distinct names FrameXML
        // calls as functions, four contain a digit, and three of those it
        // defines itself. Being wrong here costs a no-op for one API name,
        // which is where this started.
        "  if string.find(k, '%d') then return nil end\n"
        // An addon's namespace table, which is absent because the addon is not
        // loaded - and FrameXML feature-detects exactly these:
        // `if ( not Blizzard_CombatLog_Filters )`. Answering makes the guard
        // pass and the branch behind it indexes a table that has no fields, so
        // the panel's whole update dies on a nil length.
        "  if string.find(k, '^Blizzard_') then return nil end\n"
        // The panels that load on demand, which FrameXML asks for by name
        // before deciding to load them: `if ( not AchievementFrame ) then
        // AchievementFrame_LoadUI() end`. Answering with the no-op made every
        // one of those guards read as "already loaded", so the panel was never
        // asked for and the branch behind the guard ran against a stand-in -
        // watchframe goes straight on to `AchievementFrame:IsShown()`, which
        // answered a no-op too, so tracking an achievement did nothing at all.
        //
        // These ten are a floor, for an install with no Blizzard addons
        // extracted at all. The list that does the work is built by
        // declareDeferredGlobals from what the addons on disk actually define,
        // which reproduces all ten and found five more the hand list had
        // missed - the combat-text options among them, whose whole purpose is
        // the `else` branch that loads Blizzard_CombatText.
        //
        // A list rather than a rule about the shape of the name, because the
        // shape does not separate them: PlayerArrowEffectFrame and
        // WorldMapBlobFrame are addressed with no guard at all, and answering
        // nil for those would take the world map's OnLoad down. These ten are
        // the ones FrameXML feature-detects, and every use of them sits behind
        // that test.
        "  if __WoweeLoadOnDemandFrames[k] then return nil end\n"
        // Punctuation means this is not an API name at all. A Lua identifier
        // cannot contain a hyphen, so _G["KEY_-"] is a table lookup built by
        // concatenation - GetBindingText does exactly that for the key bound
        // to action button eleven - and the answer is nil, not an object that
        // the caller then tries to concatenate. Two files died on that one.
        "  if string.find(k, '[^%w_]') then return nil end\n"
        "  if not seen[k] then seen[k] = true; __WoweeRecordMissingApi(k) end\n"
        "  return missing\n"
        "end })\n");

    LOG_WARNING("LuaEngine: missing-API fallback is ON - unknown globals answer "
                "with a no-op, so feature detection will read as present");
}

void LuaEngine::declareDeferredGlobals(const std::vector<std::string>& names) {
    if (!L_ || names.empty()) return;
    lua_getglobal(L_, "__WoweeLoadOnDemandFrames");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    int added = 0;
    for (const auto& name : names) {
        if (name.empty()) continue;
        lua_getfield(L_, -1, name.c_str());
        const bool already = !lua_isnil(L_, -1);
        lua_pop(L_, 1);
        if (already) continue;
        lua_pushboolean(L_, 1);
        lua_setfield(L_, -2, name.c_str());
        ++added;
    }
    lua_pop(L_, 1);
    LOG_INFO("LuaEngine: ", added, " name(s) an unloaded addon defines will "
             "read as absent until it loads");
}

void LuaEngine::reportMissingApi() const {
    // Written again here so the counts are the session's final ones; the first
    // sighting of each error already put the file on disk.
    writeLuaErrorReport();
    if (!luaErrors_.empty()) {
        LOG_WARNING("LuaEngine: ", luaErrors_.size(),
                    " distinct Lua error(s) this session - see ",
                    core::getConfigRoot(), "/lua_errors.txt");
    }
    // Every name is checked against _G before being reported, so without a
    // state there is nothing to say. Guarded here as well as at the call site
    // because the crash this cost was a null state, not an empty list.
    if (!L_) return;
    const auto& names = missingApiNames();
    if (names.empty()) return;
    // At warning level, because release builds drop INFO and this is the whole
    // point of recording them: the list is the measured gap, once per session,
    // and it was being written where nobody could read it.
    // A name recorded here was missing when it was read, which is not the same
    // as missing. FrameXML reads a global before the file that defines it has
    // loaded all the time - a frame asks for another panel's frame in its
    // OnLoad, a font object is defined as `X = X or {}` - and every one of
    // those was landing in a list whose only value is that everything in it is
    // real. Ask again now, at the end, and keep what is still absent.
    // Widget methods that answered with a no-op. Not globals, so the test
    // below does not apply to them, and worth their own list: each is a call
    // FrameXML makes that does nothing, which is the surface of unimplemented
    // behaviour and reads as working from every other angle.
    std::vector<std::string> noops;
    std::vector<std::string> globals;
    // Three kinds, not two. A "widget:" entry is a *field* read off a widget
    // that answered nothing - which FrameXML does on purpose and constantly:
    // it reads a field before anything has set it, either to test whether it
    // has been set or because the thing that sets it runs later. TextString is
    // assigned by SetTextStatusBarText and read behind `if ( self.TextString )`;
    // BGindex is put on a battleground row as the list is built and read when
    // one is clicked. Neither is a missing API, and counting them beside the
    // names nothing implements makes that number wrong in the direction that
    // wastes someone's afternoon.
    //
    // The recorder cannot tell a field from a method at the point it fires -
    // both arrive as an index on the widget - so the separation is here, where
    // the prefix already says which is which.
    std::vector<std::string> widgetFields;
    for (const auto& n : names) {
        if (n.rfind("noop:", 0) == 0) noops.push_back(n.substr(5));
        else if (n.rfind("widget:", 0) == 0) widgetFields.push_back(n.substr(7));
        else globals.push_back(n);
    }

    std::vector<std::string> absent;
    // Read while nothing answered for them, and answered by the time the run
    // ended. Counted before this and never named, which made the number the
    // one thing in this report nobody could act on: a name here is either a
    // file that loads after its first reader - harmless, and most of them -
    // or a value captured at load and kept, which is a permanent nil that
    // nothing ever reports again.
    std::vector<std::string> definedLater;
    absent.reserve(globals.size());
    for (const auto& n : globals) {
        lua_pushstring(L_, n.c_str());
        lua_rawget(L_, LUA_GLOBALSINDEX);
        const bool defined = !lua_isnil(L_, -1);
        lua_pop(L_, 1);
        if (!defined) absent.push_back(n);
        else definedLater.push_back(n);
    }
    // Only the globals section has nothing to say. The no-op list is a
    // separate question and returning here took it down with it: once the
    // global surface went clean - which is the whole point of the transition -
    // the report stopped being written at all, and every widget method
    // answering with a no-op went quiet with it.
    //
    // That is how five real tooltips hid. SetUnitAura was serving a no-op on
    // every buff hover, recording itself faithfully each time, into a report
    // that was never produced.
    if (absent.empty() && noops.empty()) return;

    // A name built from an existing frame's is a part that frame may or may
    // not have, not an API that is missing.
    //
    // FrameXML asks for these constantly - uipaneltemplates.lua does
    // _G[self:GetName() .. "Top"] and guards the result with if(top and
    // bottom) - because a scroll frame only has border art if its own XML
    // declared it. On one session 125 of 222 names were exactly this, all of
    // them correctly absent, which is a report whose number means the opposite
    // of what it says. They are counted apart rather than dropped: a genuinely
    // missing sub-frame would hide here too, and the count is where it shows.
    // Three names the interface asks for that the interface itself never
    // provides, and that nothing here should.
    //
    // Checked by grepping Data/interface for a definition of each: there is
    // none, so they are nil in the real client too and Blizzard's own code is
    // written around that.
    //
    //   CaptureBar_Hide                 worldstateframe.lua does
    //                                   `onHide = CaptureBar_Hide`, which
    //                                   stores nil. A nil handler is a handler
    //                                   nobody calls.
    //   OptionsFrame_ToggleSubCategories  assigned to self.toggleSubCategories
    //                                   in framexml, called in gluexml - the
    //                                   glue side is the login screen, which
    //                                   this client does not run.
    //   ZonePVPType                     not a function at all. zonetext.lua
    //                                   declares `ZonePVPType = nil` at the top
    //                                   and assigns it later; reading it before
    //                                   then is reading a variable that has not
    //                                   been set, which is what it is for.
    //
    // Counted apart rather than dropped, for the same reason the frame parts
    // are: the headline number is meant to be actionable, and three permanent
    // entries in it teach whoever reads it that the number is always three.
    static constexpr const char* kNeverDefinedByBlizzard[] = {
        "CaptureBar_Hide", "OptionsFrame_ToggleSubCategories", "ZonePVPType",
    };
    std::vector<std::string> partsOfFrames;
    std::vector<std::string> blizzardsOwn;
    std::vector<std::string> realGaps;
    for (const auto& n : absent) {
        bool never = false;
        for (const char* known : kNeverDefinedByBlizzard) {
            if (n == known) { never = true; break; }
        }
        if (never) { blizzardsOwn.push_back(n); continue; }
        bool isPart = false;
        // The suffixes in play are short - Top, Middle, Bottom, Count, Text -
        // and the frame they hang off is never tiny.
        for (size_t suffix = 1; suffix <= 16 && n.size() > suffix + 5; ++suffix) {
            if (widgets_.findByName(std::string_view(n).substr(0, n.size() - suffix))) {
                isPart = true;
                break;
            }
        }
        (isPart ? partsOfFrames : realGaps).push_back(n);
    }

    if (!noops.empty()) {
        std::sort(noops.begin(), noops.end());
        std::string all;
        for (const auto& n : noops) { all += n; all += ' '; }
        LOG_WARNING("LuaEngine: ", noops.size(), " widget methods answered with a "
                    "no-op: ", all);
    }

    if (!realGaps.empty() || !partsOfFrames.empty() || !blizzardsOwn.empty()) {
    LOG_WARNING("LuaEngine: ", realGaps.size(), " distinct API names were called "
                "and are still not defined (", globals.size() - absent.size(),
                " more were read before whatever defines them had loaded, and ",
                partsOfFrames.size(), " were optional parts of frames that do "
                "exist, and ", widgetFields.size(), " were fields read off a "
                "widget before anything set them, and ", blizzardsOwn.size(),
                " the interface asks for and never defines itself)");
    }
    std::string line;
    for (const auto& n : realGaps) {
        line += n;
        line += ' ';
        if (line.size() > 900) { LOG_WARNING("  missing: ", line); line.clear(); }
    }
    if (!line.empty()) LOG_WARNING("  missing: ", line);

    // And to a file of its own, one name per line.
    //
    // This is the most useful measurement a session produces and the log is
    // the worst place to keep it: the next run truncates it, and a list of two
    // hundred names is what gets lost. A file beside the log survives, sorts,
    // and diffs against the last run - which is the question worth asking of
    // it anyway, since what matters is what changed.
    const std::string path = core::getConfigRoot() + "/missing_api.txt";
    if (std::ofstream out(path); out) {
        for (const auto& n : realGaps) out << n << "\n";
        out << "\n-- the interface asks for these and never defines them; "
               "they are nil in the real client too --\n";
        for (const auto& n : blizzardsOwn) out << n << "\n";
        out << "\n-- optional parts of frames that exist, correctly absent --\n";
        for (const auto& n : partsOfFrames) out << n << "\n";
        out << "\n-- fields read off a widget before anything set them --\n";
        for (const auto& n : widgetFields) out << n << "\n";
        out << "\n-- read before whatever defines them had loaded --\n";
        for (const auto& n : definedLater) out << n << "\n";
        out << "\n-- widget methods that answered with a no-op --\n";
        for (const auto& n : noops) out << n << "\n";
        LOG_WARNING("LuaEngine: the full list is in ", path);
    }
}

void LuaEngine::noteLuaError(const std::string& message) {
    if (message.empty()) return;
    auto [it, inserted] = luaErrors_.emplace(message, 0u);
    ++it->second;
    // Written the first time each distinct error is seen, not only at
    // shutdown. The errors worth reading are often the ones just before a
    // crash, and a report written on a clean quit is exactly the report that
    // loses them. Rewriting on a repeat would put a file write inside an
    // OnUpdate that raises every frame, so repeats only bump the count and the
    // shutdown pass writes the final tally.
    if (inserted) writeLuaErrorReport();
}

void LuaEngine::writeLuaErrorReport() const {
    if (luaErrors_.empty()) return;
    const std::string path = core::getConfigRoot() + "/lua_errors.txt";
    std::ofstream out(path);
    if (!out) return;
    out << "-- Lua errors from the interface, most frequent first.\n"
           "--\n"
           "-- Each line is a handler that stopped part way through. What it had\n"
           "-- not done yet did not happen, which is why the symptom is usually a\n"
           "-- panel that is present and does nothing rather than an error on\n"
           "-- screen. The traceback names the file and line.\n\n";
    std::vector<std::pair<uint64_t, const std::string*>> byCount;
    byCount.reserve(luaErrors_.size());
    for (const auto& [msg, count] : luaErrors_) byCount.emplace_back(count, &msg);
    std::sort(byCount.begin(), byCount.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return *a.second < *b.second;
              });
    for (const auto& [count, msg] : byCount) {
        out << "[" << count << (count == 1 ? " time] " : " times] ") << *msg << "\n\n";
    }
}

/// Whether a frame asked for this button's clicks.
///
/// WoW gives a button LeftButtonUp and nothing else unless it says otherwise,
/// and FrameXML says otherwise exactly where a context menu is wanted. Without
/// the check every frame would answer a right-click, which is a menu opening
/// under a cursor that never asked for one.
bool LuaEngine::frameAcceptsClick(uint32_t wid, const char* button) {
    lua_getglobal(L_, "__WoweeFramesByWid");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }
    lua_pushinteger(L_, static_cast<lua_Integer>(wid));
    lua_rawget(L_, -2);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 2); return false; }

    lua_getfield(L_, -1, "__clicks");
    bool accepts;
    if (lua_istable(L_, -1)) {
        // Registered explicitly: either edge counts, since this only models
        // the release. "Any" means any button, which is what every action
        // button in the interface registers - ActionButton_OnLoad calls
        // RegisterForClicks("AnyUp"), and matching only LeftButtonUp meant no
        // action button on the bar ever received a click.
        const std::string names[] = {
            std::string(button) + "Up", std::string(button) + "Down",
            "AnyUp", "AnyDown"
        };
        accepts = false;
        for (const std::string& n : names) {
            lua_getfield(L_, -1, n.c_str());
            accepts = lua_toboolean(L_, -1) != 0;
            lua_pop(L_, 1);
            if (accepts) break;
        }
    } else {
        accepts = (std::strcmp(button, "LeftButton") == 0);
    }
    lua_pop(L_, 3);
    return accepts;
}

namespace {

}  // namespace

int lua_EditBox_SetFocus(lua_State* L) {
    if (auto* e = engineFrom(L)) e->setEditFocus(widgetIdOf(L, 1));
    return 0;
}
int lua_EditBox_ClearFocus(lua_State* L) {
    if (auto* e = engineFrom(L)) e->setEditFocus(0);
    return 0;
}
int lua_EditBox_HasFocus(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->editFocused ? 1 : 0);
    return 1;
}

void LuaEngine::setEditFocus(uint32_t wid) {
    if (focusedWid_ == wid) return;
    if (focusedWid_ != 0) {
        if (auto* old = widgets_.get(focusedWid_)) {
            old->editFocused = false;
            // And its selection, for the same reason: a box that is no longer
            // being typed into must not keep a run that the next visit would
            // overwrite.
            old->hasSelection = false;
        }
        callFrameScript(focusedWid_, "OnEditFocusLost");
    }
    focusedWid_ = wid;
    if (focusedWid_ != 0) {
        if (auto* w = widgets_.get(focusedWid_)) w->editFocused = true;
        callFrameScript(focusedWid_, "OnEditFocusGained");
    }
}

void LuaEngine::dispatchText(const char* utf8) {
    if (!L_ || focusedWid_ == 0 || !utf8) return;
    auto* w = widgets_.get(focusedWid_);
    if (!w || !w->isEditBox) return;

    std::string add(utf8);
    if (add.empty()) return;
    // Typing leaves the history, for the same reason SetText does.
    w->editHistoryPos = -1;
    // A numeric box takes digits and nothing else, which is what stops a
    // quantity field filling with letters.
    if (w->editNumeric) {
        add.erase(std::remove_if(add.begin(), add.end(),
                                 [](unsigned char c) { return std::isdigit(c) == 0; }),
                  add.end());
        if (add.empty()) return;
    }
    // Counted the way the box asked to be counted. A limit is in characters
    // shown, not bytes held, unless countInvisibleLetters says otherwise: the
    // chat box declares letters="255" and nothing else, so the escapes a
    // shift-clicked item link brings - about sixty bytes for eighteen visible
    // characters - must not be charged against it.
    const int held = w->countInvisibleLetters
                         ? static_cast<int>(w->editText.size())
                         : static_cast<int>(ui::visibleLength(w->editText));
    const int adding = w->countInvisibleLetters
                           ? static_cast<int>(add.size())
                           : static_cast<int>(ui::visibleLength(add));
    if (w->editMaxLetters > 0 && held + adding > w->editMaxLetters) {
        const int room = w->editMaxLetters - held;
        if (room <= 0) return;
        add.resize(static_cast<size_t>(room));
    }

    // A selected run is replaced by what is typed, which is the whole point of
    // having one: autocomplete highlights the name it completed so the next
    // character overwrites it rather than following it.
    w->cursorPos = ui::replaceSelection(
        w->editText, w->cursorPos, {w->hasSelection, w->selStart, w->selEnd});
    w->hasSelection = false;
    const size_t at = std::min(w->cursorPos, w->editText.size());
    w->editText.insert(at, add);
    w->cursorPos = at + add.size();
    // The handler that tells a search field to filter, and a chat box to look
    // for a channel prefix.
    fireCursorChanged(focusedWid_);
                callFrameScript(focusedWid_, "OnTextChanged");
    // A space is its own handler, and the chat box is what wants it: typing
    // "/w Bob " is how a whisper gets its target, and ChatEdit_OnSpacePressed
    // is what reads the name out and turns the box into a whisper with a
    // header. Without it the slash command stayed literal text until it was
    // sent.
    if (add.find(' ') != std::string::npos) {
        callFrameScript(focusedWid_, "OnSpacePressed");
    }
}

std::string LuaEngine::bindingCommandFor(int sdlKeycode, bool shift, bool ctrl,
                                         bool alt) {
    return bindingCommandForName(ui::wowKeyNameFromKeycode(sdlKeycode), shift, ctrl, alt);
}

std::string LuaEngine::bindingCommandForName(std::string key, bool shift,
                                             bool ctrl, bool alt) {
    if (!L_ || key.empty()) return "";
    // WoW's own spelling, and the order is part of it: ALT before CTRL before
    // SHIFT, because that is how the binding tables are keyed and a prefix in
    // any other order matches nothing at all.
    if (shift) key = "SHIFT-" + key;
    if (ctrl)  key = "CTRL-"  + key;
    if (alt)   key = "ALT-"   + key;

    // Which command the key runs, and then the command's script - both asked
    // of the interface's own tables rather than restated here. GetBindingAction
    // already honours SetBinding, so a key the player rebound in game answers
    // to whatever they moved it to without this knowing anything about it.
    lua_getglobal(L_, "GetBindingAction");
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return ""; }
    lua_pushstring(L_, key.c_str());
    if (lua_pcall(L_, 1, 1, 0) != 0) {
        LOG_WARNING("GetBindingAction(", key, ") failed: ",
                    luaL_optstring(L_, -1, "?"));
        lua_pop(L_, 1);
        return "";
    }
    const char* got = lua_tostring(L_, -1);
    std::string command = got ? got : "";
    lua_pop(L_, 1);
    return command;
}

bool LuaEngine::dispatchBindingKey(int sdlKeycode, bool shift, bool ctrl,
                                   bool alt, bool down) {
    const std::string command = down
        ? bindingCommandFor(sdlKeycode, shift, ctrl, alt)
        : std::string();
    return dispatchResolvedBinding(sdlKeycode, command, down);
}

bool LuaEngine::dispatchBindingMouseButton(int sdlButton, bool shift, bool ctrl,
                                           bool alt, bool down) {
    const std::string key = ui::wowMouseButtonName(sdlButton);
    if (key.empty()) return false;
    const int pressKey = ui::mouseBindingPressKey(ui::wowMouseButton(sdlButton));

    const bool wasHeld = bindingPresses_.contains(pressKey);
    const std::string command =
        down ? bindingCommandForName(key, shift, ctrl, alt) : std::string();
    dispatchResolvedBinding(pressKey, command, down);

    // Claimed when the button is bound at all, rather than when a binding
    // script answered: the caller uses this to keep the camera off the click,
    // and a camera handed one half of a press starts a mouse-look it is never
    // told to end.
    return down ? bindingPresses_.contains(pressKey) : wasHeld;
}

bool LuaEngine::dispatchResolvedBinding(int physicalKey, std::string command,
                                        bool down) {
    if (!L_) return false;
    if (down) {
        if (command.empty()) return false;
        bindingPresses_.press(physicalKey, command);
    } else {
        // Release exactly what this physical key pressed. Looking the command
        // up with today's modifiers loses CTRL-X when Ctrl came up before X;
        // looking it up after a rebind can release an entirely different one.
        auto active = bindingPresses_.release(physicalKey);
        if (!active) return false;
        command = std::move(*active);
    }

    // Left alone if the client performs it. Its held and press-edge state is
    // still recorded here, from the same resolved binding FrameXML displays;
    // camera movement and native action handling consume that state instead of
    // polling hardcoded keys.
    if (clientActsOnBinding(command)) {
        core::Input::getInstance().setBindingCommandHeld(command, down);
        return false;
    }

    lua_getglobal(L_, "__WoweeBindingScripts");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }
    lua_getfield(L_, -1, command.c_str());
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 2); return false; }
    lua_pushstring(L_, down ? "down" : "up");
    if (lua_pcall(L_, 1, 0, 0) != 0) {
        LOG_WARNING("Binding ", command, " failed: ",
                    luaL_optstring(L_, -1, "?"));
        lua_pop(L_, 1);
        lua_pop(L_, 1);
        return false;
    }
    lua_pop(L_, 1);
    return true;
}

bool LuaEngine::dispatchFrameKey(int sdlKeycode, bool down) {
    if (!L_) return false;
    const std::string key = ui::wowKeyNameFromKeycode(sdlKeycode);
    if (key.empty()) return false;

    // The topmost frame that is both visible and listening. Everything that
    // declares a key handler in the interface is a dialog that is hidden until
    // it is wanted, so in ordinary play there is nothing here and the key goes
    // straight through to the game.
    const ui::Widget* best = nullptr;
    for (size_t id = 1; id < widgets_.size(); ++id) {
        const ui::Widget* w = widgets_.get(static_cast<uint32_t>(id));
        if (!w || !w->keyboardEnabled || !w->visible) continue;
        if (!best) { best = w; continue; }
        if (w->effStrata > best->effStrata ||
            (w->effStrata == best->effStrata && w->effLevel >= best->effLevel)) {
            best = w;
        }
    }
    if (!best) return false;

    callFrameScript(best->id, down ? "OnKeyDown" : "OnKeyUp", key.c_str());
    // Consumed unless the frame asked for the key to carry on, which is WoW's
    // default and the reason a dialog stops the character walking.
    return !best->propagateKeys;
}

void LuaEngine::dispatchKey(int sdlKeycode, bool ctrlHeld) {
    if (!L_ || focusedWid_ == 0) return;
    auto* w = widgets_.get(focusedWid_);
    if (!w || !w->isEditBox) return;
    (void)ctrlHeld;

    // Keycodes are SDL's, which is what the window reports; the caller does not
    // translate them so this stays the only place that knows.
    constexpr int kBackspace = '\b';
    constexpr int kReturn    = '\r';
    constexpr int kEscape    = 27;
    constexpr int kDelete    = 0x4000004C;  // SDLK_DELETE
    constexpr int kLeft      = 0x40000050;
    constexpr int kRight     = 0x4000004F;
    constexpr int kHome      = 0x4000004A;
    constexpr int kEnd       = 0x4000004D;
    constexpr int kTab       = '\t';
    // Same convention as the four above: SDL's scancode with its keycode bit.
    constexpr int kUp        = 0x40000052;
    constexpr int kDown      = 0x40000051;

    const size_t len = w->editText.size();
    switch (sdlKeycode) {
        // Erasing a character means erasing what draws as one. A link is
        // fifty-odd bytes, so taking a byte off either end leaves a mangled
        // escape behind - half a payload, drawing as whatever the parser makes
        // of the wreckage. The same step the caret moves by is the span to
        // remove, which keeps the two from ever disagreeing.
        case kBackspace:
            // The selection first, if there is one: backspace over a
            // highlighted run removes the run, not the character before it.
            if (w->hasSelection) {
                const size_t was = w->cursorPos;
                w->cursorPos = ui::replaceSelection(
                    w->editText, w->cursorPos,
                    {true, w->selStart, w->selEnd});
                w->hasSelection = false;
                if (w->cursorPos != was || w->selEnd > w->selStart) {
                    fireCursorChanged(focusedWid_);
                    callFrameScript(focusedWid_, "OnTextChanged");
                    break;
                }
            }
            if (w->cursorPos > 0 && len > 0) {
                const size_t from = ui::caretStepLeft(w->editText, w->cursorPos);
                w->editText.erase(from, w->cursorPos - from);
                w->cursorPos = from;
                fireCursorChanged(focusedWid_);
                callFrameScript(focusedWid_, "OnTextChanged");
            }
            break;
        case kDelete:
            if (w->cursorPos < len) {
                const size_t to = ui::caretStepRight(w->editText, w->cursorPos);
                w->editText.erase(w->cursorPos, to - w->cursorPos);
                fireCursorChanged(focusedWid_);
                callFrameScript(focusedWid_, "OnTextChanged");
            }
            break;
        // Back and forward through what was typed here before. Stepping off
        // the newest end empties the box rather than sticking on the last
        // line, which is what leaving the history means.
        case kUp:
            if (!w->editHistory.empty()) {
                const int last = static_cast<int>(w->editHistory.size()) - 1;
                w->editHistoryPos = (w->editHistoryPos < 0)
                    ? last : std::max(0, w->editHistoryPos - 1);
                w->editText = w->editHistory[static_cast<size_t>(w->editHistoryPos)];
                w->cursorPos = w->editText.size();
                fireCursorChanged(focusedWid_);
                callFrameScript(focusedWid_, "OnTextChanged");
            }
            break;
        case kDown:
            if (w->editHistoryPos >= 0) {
                const int last = static_cast<int>(w->editHistory.size()) - 1;
                if (w->editHistoryPos >= last) {
                    w->editHistoryPos = -1;
                    w->editText.clear();
                } else {
                    ++w->editHistoryPos;
                    w->editText = w->editHistory[static_cast<size_t>(w->editHistoryPos)];
                }
                w->cursorPos = w->editText.size();
                fireCursorChanged(focusedWid_);
                callFrameScript(focusedWid_, "OnTextChanged");
            }
            break;
        // Declared ignoreArrows means these belong to the game rather than to
        // the cursor, which is how someone turns while the chat box is open.
        // A step is one drawn character, not one byte. An item link is fifty
        // bytes and eighteen characters, and stepping through it a byte at a
        // time leaves the caret apparently stuck.
        case kLeft:
            if (!w->editIgnoreArrows && w->cursorPos > 0)
                w->cursorPos = ui::caretStepLeft(w->editText, w->cursorPos);
            break;
        case kRight:
            if (!w->editIgnoreArrows && w->cursorPos < len)
                w->cursorPos = ui::caretStepRight(w->editText, w->cursorPos);
            break;
        case kHome:  w->cursorPos = 0; break;
        case kEnd:   w->cursorPos = len; break;
        case kReturn:
            // A box declared multiLine takes the return as a line break rather
            // than as "done". The mail body says multiLine="true" and letters
            // ="500"; reading the limit and not the flag left a letter that
            // stops at five hundred characters and still cannot hold two
            // paragraphs.
            //
            // Blizzard's own boxes rely on this split: the chat box has no
            // multiLine and submits, the mail body has it and does not, and
            // both are the same OnEnterPressed handler.
            if (w->editMultiLine) {
                // The break counts against the limit like any other character;
                // inserting it directly would let a full box grow by one every
                // time return was pressed.
                if (w->editMaxLetters > 0 &&
                    static_cast<int>(w->editText.size()) >= w->editMaxLetters) {
                    break;
                }
                const size_t at = std::min(w->cursorPos, w->editText.size());
                w->editText.insert(at, 1, '\n');
                w->cursorPos = at + 1;
                fireCursorChanged(focusedWid_);
                callFrameScript(focusedWid_, "OnTextChanged");
                break;
            }
            // The handler decides what to do with it, including whether to let
            // go of focus - a chat box does, a search field does not.
            callFrameScript(focusedWid_, "OnEnterPressed");
            break;
        case kEscape:
            callFrameScript(focusedWid_, "OnEscapePressed");
            setEditFocus(0);
            break;
        case kTab:
            // Focus stays where it is unless the handler moves it. Twelve
            // boxes in the interface declare this, and every one of them
            // reaches for the next field by name - which is the frame's own
            // business, not something to guess at from here.
            callFrameScript(focusedWid_, "OnTabPressed");
            break;
        default: break;
    }
}

void LuaEngine::reportEventListenersOnce() {
    if (!L_ || eventListenersReported_) return;
    // Counted from when there is an interface to count, not from startup:
    // FrameXML loads on entering the world, and a report timed from the
    // client's first frame ran before any of it existed and said zero for
    // everything - including events that demonstrably work.
    if (widgets_.size() < 200) return;
    if (++eventReportFrames_ < 120) return;
    eventListenersReported_ = true;

    lua_getglobal(L_, "__WoweeFrameEvents");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    static const char* kWatched[] = {
        "UNIT_HEALTH", "UNIT_MAXHEALTH", "UNIT_MANA", "UNIT_MAXMANA",
        "UNIT_RAGE", "UNIT_ENERGY", "UNIT_DISPLAYPOWER", "PLAYER_ENTERING_WORLD",
        // The frames that have gone wrong most recently, so that "nothing is
        // listening" is ruled in or out before anything else is looked at.
        "PLAYER_TARGET_CHANGED", "UNIT_AURA", "PLAYER_XP_UPDATE", "BAG_UPDATE",
    };
    std::string line;
    for (const char* name : kWatched) {
        lua_getfield(L_, -1, name);
        const int n = lua_istable(L_, -1)
            ? static_cast<int>(lua_objlen(L_, -1)) : 0;
        lua_pop(L_, 1);
        line += name;
        line += '=';
        line += std::to_string(n);
        line += ' ';
    }
    lua_pop(L_, 1);
    LOG_WARNING("Event listeners: ", line);
}

/// Fire OnSizeChanged for anything whose rect changed since the last frame.
///
/// Declared by FrameXML and reached for constantly by addons, which resize a
/// panel and expect the pieces inside it to be told. Noticed here rather than
/// fired from SetWidth, because a frame is far more often resized by its
/// anchors than by anyone calling a setter - a scroll child stretched by its
/// parent never goes near SetWidth.
void LuaEngine::updateSizeChanges() {
    if (!L_) return;
    for (size_t id = 1; id < widgets_.size(); ++id) {
        const ui::Widget* wp = widgets_.get(static_cast<uint32_t>(id));
        if (!wp) continue;
        const ui::Widget& w = *wp;
        if (w.id == 0 || w.kind != ui::WidgetKind::Frame) continue;
        const bool known = (w.lastReportedW >= 0.0f);
        if (known && w.lastReportedW == w.rectW && w.lastReportedH == w.rectH) {
            continue;
        }
        // Written through the tree, since the loop reads a const view.
        if (auto* mut = widgets_.get(w.id)) {
            mut->lastReportedW = w.rectW;
            mut->lastReportedH = w.rectH;
        }
        // Nothing is fired the first time a frame is measured: every frame in
        // the interface would report a change on the first layout, which says
        // nothing and runs 3000 handlers to say it.
        if (!known) continue;
        if (w.rectW <= 0.0f || w.rectH <= 0.0f) continue;
        lua_getglobal(L_, "__WoweeFramesByWid");
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
        lua_pushinteger(L_, static_cast<lua_Integer>(w.id));
        lua_rawget(L_, -2);
        if (lua_istable(L_, -1)) {
            lua_getfield(L_, -1, "OnSizeChanged");
            if (lua_isfunction(L_, -1)) {
                lua_pushvalue(L_, -2);
                lua_pushnumber(L_, w.rectW);
                lua_pushnumber(L_, w.rectH);
                if (pcallScript(L_, "OnSizeChanged", 3, 0) != 0) {
                    LOG_WARNING("OnSizeChanged error: ",
                                luaL_optstring(L_, -1, "?"));
                    lua_pop(L_, 1);
                }
            } else {
                lua_pop(L_, 1);
            }
        }
        lua_pop(L_, 2);
    }
}

void LuaEngine::updateVisibility() {
    if (!L_) return;
    // By index and re-fetched each time: a handler is free to create frames,
    // and OnShow very often does.
    // `visible` here and `visibleChain` in the OnUpdate pump, which is a
    // deliberate difference rather than an oversight.
    //
    // WoW fires OnShow on becoming shown-with-ancestors-shown, with no regard
    // for anchors, so visibleChain is the faithful answer. Using it would mean
    // frames that are never drawn start running their OnShow and OnHide, and
    // those handlers do real work - LootFrame_OnHide calls CloseLoot, which
    // releases the loot on the server, and UIParent's OnShow calls
    // CloseAllWindows.
    //
    // What that faithfulness would buy was measured rather than guessed: six
    // frames in the whole interface declare an OnShow with no anchors and no
    // setAllPoints - KnowledgeBaseFrameCancel, ChannelListDropDown,
    // HelpFrameOpenTicketEditBox, the two calendar context menus and
    // GMSurveyFrameComment - and not one of them is anchored from Lua either,
    // anywhere. None can ever be drawn or used, so none of their OnShow
    // handlers is worth the risk of waking the rest.
    //
    // (UIParent looks like a seventh and is not: it carries setAllPoints.)
    for (uint32_t id = 1; id < widgets_.size(); ++id) {
        auto* w = widgets_.get(id);
        if (!w || w->id == 0) continue;
        const uint8_t toggles = w->shownToggles;
        w->shownToggles = 0;
        if (w->visible == w->reportedVisible) {
            // Nothing changed overall - but if Lua hid this and showed it again
            // before anything looked, something *did* happen and both handlers
            // are owed. `panel:Hide(); panel:Show()` is the interface asking a
            // panel to rebuild itself, and it is the entirety of QuestFrame's
            // handler for QUEST_DETAIL, QUEST_PROGRESS, QUEST_COMPLETE and
            // QUEST_GREETING - the four NPC dialogs, and the reason the vendor
            // was the only one that still filled in. Everything those panels
            // show is positioned by QuestInfo_Display, which runs from OnShow
            // and from nowhere else, so with no OnShow the text kept the place
            // its XML gave it: outside the scroll frame that clips it, drawn
            // and clipped away. A blank parchment with working buttons.
            //
            // Only for a frame that is on screen at the end of it. A pair of
            // calls on something that cannot be drawn is not a rebuild anyone
            // can see, and waking those handlers is the risk this pass was
            // written to avoid.
            if (toggles < 2 || !w->visible) continue;
            callFrameScript(id, "OnHide");
            callFrameScript(id, "OnShow");
            // Deliberately both, and in the order they happened. OnHide is
            // where a panel drops what it was holding, and skipping it would
            // leave the second OnShow building on top of the first one's state.
            continue;
        }
        w->reportedVisible = w->visible;
        // Show() runs OnShow itself when the frame became shown under shown
        // ancestors, so the change is recorded here without being announced
        // twice. The flag is cleared either way: a frame that was shown and
        // then anchored later arrives here once, and only once.
        const bool already = w->onShowFired;
        w->onShowFired = false;
        if (!(already && w->visible)) {
            callFrameScript(id, w->visible ? "OnShow" : "OnHide");
        }
        // A box that asked for the keyboard takes it as it appears, and gives
        // it up when it goes. Only the two that ask: the rest say autoFocus
        // ="false" precisely so that opening a panel does not swallow the
        // player's next keystroke.
        //
        // Taking it is the autoFocus box's privilege; giving it up on the way
        // out is every box's duty. Only the two that ask took it and only the
        // two that ask released it, so a box focused by name - which is how
        // the chat box and every dialog's box get it - kept the keyboard after
        // it was hidden. Nothing on screen had focus and focusedWid_ was not
        // zero, and that flag is what the client asks before it looks at a key
        // at all: escape stopped opening the menu and every character typed
        // went into a box that was not there.
        if (w->isEditBox) {
            if (w->visible) {
                if (w->editAutoFocus) setEditFocus(id);
            } else if (focusedWid_ == id) {
                setEditFocus(0);
            }
        }
    }
}

bool LuaEngine::editBoxHasFocus() const {
    if (focusedWid_ == 0) return false;
    const auto* w = widgets_.get(focusedWid_);
    // An edit box, and not merely whatever holds the focus. This answer is
    // what every keystroke in the client is gated on - bindings, the camera,
    // the sheathe key and the Escape chain all defer to it - so a frame that
    // is not a text field holding the focus would claim the keyboard outright
    // and nothing would say why. The mouse path already refuses to focus
    // anything else; SetFocus from Lua does not, and this is the same rule on
    // the reading side.
    return w != nullptr && w->id != 0 && w->isEditBox && w->visible;
}

void LuaEngine::updateScrollRanges() {
    if (!L_) return;
    for (uint32_t id : widgets_.scrollFrames()) {
        auto* w = widgets_.get(id);
        if (!w) continue;
        float rangeX = 0.0f, rangeY = 0.0f;
        if (const auto* child = widgets_.get(w->scrollChild)) {
            // The child's contents, not its declared size - see
            // scrollContentExtent. The two agree for a frame that sizes its own
            // child and differ sharply for one the client is expected to size.
            float contentW = child->rectW, contentH = child->rectH;
            widgets_.scrollContentExtent(w->scrollChild, contentW, contentH);
            rangeX = contentW - w->rectW;
            rangeY = contentH - w->rectH;
            if (rangeX < 0.0f) rangeX = 0.0f;
            if (rangeY < 0.0f) rangeY = 0.0f;
        }
        if (rangeX == w->reportedRangeX && rangeY == w->reportedRangeY) continue;
        w->reportedRangeX = rangeX;
        w->reportedRangeY = rangeY;

        // Both ranges, in WoW's order. ScrollFrame_OnScrollRangeChanged reads
        // the second and hides the bar when it is zero, which is how a list
        // that fits shows no scroll bar at all.
        lua_getglobal(L_, "__WoweeFramesByWid");
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); continue; }
        lua_pushinteger(L_, static_cast<lua_Integer>(id));
        lua_rawget(L_, -2);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 2); continue; }
        lua_getfield(L_, -1, "__scripts");
        if (lua_istable(L_, -1)) {
            lua_getfield(L_, -1, "OnScrollRangeChanged");
            if (lua_isfunction(L_, -1)) {
                lua_pushvalue(L_, -3);          // self
                lua_pushnumber(L_, rangeX);
                lua_pushnumber(L_, rangeY);
                if (pcallScript(L_, "OnScrollRangeChanged", 3, 0) != 0) {
                    const char* err = lua_tostring(L_, -1);
                    LOG_ERROR("LuaEngine: OnScrollRangeChanged error: ",
                              err ? err : "?");
                    lua_pop(L_, 1);
                }
            } else {
                lua_pop(L_, 1);
            }
        }
        lua_pop(L_, 3);
    }
}

bool LuaEngine::dispatchMouseWheel(float x, float y, float delta) {
    if (!L_) return false;
    const float s = widgets_.uiScale();
    if (s > 0.0f) { x /= s; y /= s; }

    // One notch, whatever the mouse said. WoW's OnMouseWheel delta is exactly
    // 1 or -1 and FrameXML is written against that: hybridscrollframe.lua:46
    // is `if ( delta == 1 ) then scroll up else scroll down end`, so a wheel
    // that reported 2 or 3 - which any brisk scroll on a trackpad or a
    // free-spinning wheel does - fell through to the else and scrolled *down*
    // while the hand moved up.
    //
    // Down never showed it. Every negative delta fails that test too and lands
    // in the same branch, which is the branch it wanted, so down worked at any
    // speed and up worked only when the wheel happened to send a bare 1.
    //
    // Sign only, and not clamped elsewhere: the camera keeps the magnitude,
    // because how far a zoom travels is a different question from which way a
    // list moves.
    delta = (delta > 0.0f) ? 1.0f : (delta < 0.0f ? -1.0f : 0.0f);
    if (delta == 0.0f) return false;

    // Up from whatever is under the cursor to the first frame that asked for
    // the wheel. WoW works the same way: a scroll frame's child fills it and
    // takes the hit, and the scroll frame above is what handles the wheel.
    // The wheel's own hit test. A scroll frame enables the wheel and not the
    // mouse - UIPanelScrollFrameTemplate declares OnMouseWheel and never calls
    // EnableMouse - so the plain hit test could not see one, and this walk
    // started from whatever mouse-enabled child happened to be under the
    // cursor or, over the empty parts of a panel, from nothing at all. The
    // talent tree is all empty parts between its buttons.
    uint32_t wid = widgets_.hitTestWheel(x, y);
    while (wid != 0) {
        const auto* w = widgets_.get(wid);
        if (!w) break;
        if (w->wheelEnabled) {
            callFrameScriptNumber(wid, "OnMouseWheel", delta);
            return true;
        }
        wid = w->parent;
    }
    return false;
}

bool LuaEngine::holdsMousePress() const {
    for (bool down : buttonDown_) {
        if (down) return true;
    }
    // A drag or a moved frame counts even with nothing held, because that is
    // precisely the state a stranded drag leaves behind and it has to stay
    // reachable long enough for the release to clear it.
    return draggingWid_ != 0 || widgets_.movingWidget() != 0 ||
           widgets_.sizingWidget() != 0;
}

void LuaEngine::releaseMouseHover() {
    if (!L_) return;
    if (hoverWid_ != 0) {
        callFrameScript(hoverWid_, "OnLeave");
        hoverWid_ = 0;
    }
    widgets_.setInteraction(0, 0);
    // Nothing is under the cursor and nothing is holding it, which is the
    // camera's cue that it may turn again.
    ui::frameXmlNoteMouseOwned(false);
    // The next position the tree hears is a fresh one rather than the far end
    // of however far the cursor travelled while it was not listening.
    haveCursor_ = false;
}

void LuaEngine::dispatchMouse(float x, float y, float screenH, MouseButtons buttons) {
    if (!L_) return;
    // The cursor arrives in window pixels measured from the top, and the tree
    // is in interface units measured from the bottom. ui::mouseToTreeSpace is
    // the one definition of that conversion - the link rects a draw pass files
    // go through its other half, and when the two were written separately the
    // hyperlink hit test missed by the scale factor and the screen height.
    ui::mouseToTreeSpace(x, y, screenH, widgets_.uiScale());
    const uint32_t hit = widgets_.hitTest(x, y);
    lastMouseHit_ = hit;
    // Kept so IsMouseOver can answer from a frame's own rect. Hover alone is
    // not enough: it names the mouse-enabled frame that was hit, and a
    // container the cursor is plainly inside is often neither.
    sLastMouseX_ = x;
    sLastMouseY_ = y;

    // Throttled, and only while there is something to hit. Whether the mouse
    // reaches the widget tree at all is otherwise invisible: a frame that never
    // lights up looks the same whether the dispatch is not running, the
    // coordinates are wrong, or the frame is not taking the mouse.
    static double lastReport = 0.0;
    const double now = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    if (now - lastReport >= 1.0) {
        size_t mouseFrames = 0;
        for (uint32_t id = 1; id < widgets_.size(); ++id) {
            const auto* w = widgets_.get(id);
            if (w && w->mouseEnabled && w->visible) ++mouseFrames;
        }
        // Reported whenever anything is on screen at all, not only when
        // something is mouse-enabled. Gating on that hid the one case that was
        // actually happening: no frame took the mouse, so the count was zero,
        // so nothing was logged, so the silence looked like the dispatch never
        // running. A diagnostic must not go quiet in the state it exists to
        // report.
        size_t visibleFrames = 0;
        for (uint32_t id = 1; id < widgets_.size(); ++id) {
            const auto* w = widgets_.get(id);
            if (w && w->visible) ++visibleFrames;
        }
        if (visibleFrames > 0) {
            lastReport = now;
            LOG_INFO("WidgetInput: mouse=(", x, ",", y, ") hit=", hit,
                     " hover=", hoverWid_, " mouseEnabled=", mouseFrames,
                     " visible=", visibleFrames);
        }
    }

    // What a press landed on, once a second while one is held. The question
    // "did my click reach anything" has no other answer from outside.
    if (buttons.left) {
        static double lastPress = 0.0;
        const double now = core::appTimeSeconds();
        if (now - lastPress > 1.0) {
            lastPress = now;
            const auto* w = widgets_.get(hit);
            LOG_WARNING("WidgetInput: press at (", x, ",", y, ") hit ",
                        hit == 0 ? "nothing"
                                 : (w && !w->name.empty() ? w->name.c_str() : "(unnamed)"));
        }
    }

    // Told before anything else reads it: which of a button's textures to draw
    // depends on both, and the draw order is collected during layout, which has
    // already happened by the time this runs - so this frame's press shows on
    // the next, which at sixty frames a second is not a wait anyone sees.
    widgets_.setInteraction(hit, buttonDown_[0] ? pressedWid_[0] : 0);

    // Tell the rest of the client the interface has the cursor, so the camera
    // does not turn while a bag item is being clicked or dragged. A press keeps
    // it owned even once the cursor has left the frame, because letting go of
    // the interface halfway through a drag would hand the rest of the drag to
    // the camera.
    ui::frameXmlNoteMouseOwned(hit != 0 || pressedWid_[0] != 0 ||
                               pressedWid_[1] != 0 || pressedWid_[2] != 0);

    // Hover first, so a frame that appears under a stationary cursor still gets
    // its OnEnter rather than waiting for the mouse to move.
    //
    // A disabled button hears nothing unless its XML asked to, which is WoW's
    // rule and the reason the attribute exists at all. Without this every
    // greyed control answered the mouse - a tooltip on a spellbook slot for a
    // spell that is not known, and on every micro button whose panel is shut.
    // The five templates that declare it keep theirs, which is the point: that
    // is how a greyed control says why it is greyed.
    const auto hearsMotion = [this](uint32_t id) {
        const auto* w = widgets_.get(id);
        return w && (w->enabled || w->motionScriptsWhileDisabled);
    };
    if (hit != hoverWid_) {
        if (hoverWid_ != 0 && hearsMotion(hoverWid_)) callFrameScript(hoverWid_, "OnLeave");
        hoverWid_ = hit;
        if (hoverWid_ != 0 && hearsMotion(hoverWid_)) callFrameScript(hoverWid_, "OnEnter");
    }

    // A panel that closes under the cursor leaves its tooltip behind: the
    // button it belonged to is hidden with the panel, so it never receives the
    // OnLeave that would have hidden the tooltip. Looting the last item does
    // exactly this - the loot window closes itself - and the item's
    // description stayed on screen over nothing.
    widgets_.hideOrphanedTooltips();

    // How far the cursor has travelled since last frame, which is what carries
    // a frame that is being moved and what tells a drag from a click.
    const float dx = haveCursor_ ? (x - cursorX_) : 0.0f;
    const float dy = haveCursor_ ? (y - cursorY_) : 0.0f;
    cursorX_ = x;
    cursorY_ = y;
    haveCursor_ = true;

    // A frame that StartMoving picked up follows the cursor until something
    // puts it down.
    //
    // With nothing held it is put down here, whatever else went wrong. A frame
    // stuck to the cursor follows it forever and there is no way back from it
    // in the running client, so this does not rely on the release path having
    // matched the drag correctly.
    const bool anyHeld = buttonDown_[0] || buttonDown_[1] || buttonDown_[2];
    if (!anyHeld && (widgets_.movingWidget() != 0 || widgets_.sizingWidget() != 0 ||
                     draggingWid_ != 0)) {
        if (draggingWid_ != 0) callFrameScript(draggingWid_, "OnDragStop", "LeftButton");
        widgets_.setMovingWidget(0);
        widgets_.setSizingWidget(0, "");
        draggingWid_ = 0;
        draggingButton_ = -1;
    }
    if (const uint32_t moving = widgets_.movingWidget()) {
        if (dx != 0.0f || dy != 0.0f) widgets_.nudge(moving, dx, dy);
    }
    if (const uint32_t sizing = widgets_.sizingWidget()) {
        if (dx != 0.0f || dy != 0.0f)
            widgets_.resizeBy(sizing, widgets_.sizingPoint(), dx, dy);
    }

    // Begin a drag once the cursor has left the button it went down on. WoW
    // draws the same line: below the threshold it is a click, above it the
    // frame's OnDragStart runs and the click is abandoned.
    if (draggingWid_ == 0) {
        constexpr float kDragThreshold = 4.0f;   // interface units
        for (int i = 0; i < kMouseButtons; ++i) {
            if (!buttonDown_[i] || pressedWid_[i] == 0) continue;
            // Up through the parents until something is registered for this
            // button, because a drag belongs to the nearest frame that asked
            // for one rather than to whatever the press happened to land on.
            // PaperDollFrame covers the whole character sheet and takes the
            // mouse without taking drags, so every press on the sheet stopped
            // there and it could not be moved.
            // A slider stops that walk. It is the one widget whose whole
            // purpose is to be dragged, and it is already following the cursor
            // for as long as the button is held - so the press belongs to it
            // and must not be offered to anything above it. Without this, a
            // scroll bar sits inside a movable window and dragging its grip
            // found the window instead: the list scrolled and the whole panel
            // came away with the cursor.
            //
            // Checked along the walk rather than only at the press, because a
            // scroll bar's up and down buttons are its children - a drag off one
            // of those would otherwise pass straight through the bar to the
            // window behind it.
            uint32_t owner = 0;
            bool stoppedAtSlider = false;
            for (uint32_t id = pressedWid_[i]; id != 0;) {
                const auto* cand = widgets_.get(id);
                if (!cand) break;
                const bool takes = (i == 0) ? cand->dragLeft
                                 : (i == 1) ? cand->dragRight
                                            : false;
                if (takes) { owner = id; break; }
                if (cand->isSlider) { stoppedAtSlider = true; break; }
                id = cand->parent;
            }
            const float mx = x - pressX_[i], my = y - pressY_[i];
            const bool movedEnough =
                mx * mx + my * my >= kDragThreshold * kDragThreshold;
            if (owner == 0) {
                // The silent case, and the one with nothing to show for it.
                // A press that travels far enough to be a drag and finds
                // nothing registered for one looks exactly like a press the
                // interface never saw - and the two have opposite causes.
                // Said once a second so holding the button does not flood it.
                // Not for a slider: it took the press deliberately, and the
                // drag it is doing is its own.
                if (movedEnough && !stoppedAtSlider) {
                    const double now = wowee::core::appTimeSeconds();
                    if (now - lastNoDragOwnerSaid_ > 1.0) {
                        lastNoDragOwnerSaid_ = now;
                        const auto* pw = widgets_.get(pressedWid_[i]);
                        LOG_WARNING("WidgetInput: dragged from '",
                                    pw && !pw->name.empty() ? pw->name.c_str()
                                                            : "(unnamed)",
                                    "', but neither it nor anything above it "
                                    "is registered for drag on this button");
                    }
                }
                continue;
            }
            if (!movedEnough) continue;
            draggingWid_ = owner;
            draggingButton_ = i;
            callFrameScript(draggingWid_, "OnDragStart",
                            i == 0 ? "LeftButton" : "RightButton");
            const auto* dw = widgets_.get(draggingWid_);
            LOG_WARNING("WidgetInput: drag started on ",
                        dw && !dw->name.empty() ? dw->name.c_str() : "(unnamed)");
            break;
        }
    }

    // A slider follows the cursor for as long as it is held, which is the only
    // widget where what happens between press and release is the point. The
    // frame keeps the grab even when the cursor leaves it, because letting go
    // of a scroll bar by sliding sideways is not what anyone means.
    if (buttonDown_[0] && pressedWid_[0] != 0) {
        if (auto* w = widgets_.get(pressedWid_[0]); w && w->isSlider) {
            const float span = w->barMax - w->barMin;
            if (span > 0.0f) {
                // Vertical sliders run top to bottom, and the tree's y grows
                // upward, so the fraction is measured from the far edge.
                const float extent = w->barVertical ? w->rectH : w->rectW;
                float f = 0.0f;
                if (extent > 0.0f) {
                    f = w->barVertical ? (w->bottom + w->rectH - y) / extent
                                       : (x - w->left) / extent;
                }
                f = std::clamp(f, 0.0f, 1.0f);
                float value = w->barMin + f * span;
                if (w->sliderStep > 0.0f) {
                    value = w->barMin +
                            std::round((value - w->barMin) / w->sliderStep) * w->sliderStep;
                    value = std::clamp(value, w->barMin, w->barMax);
                }
                if (value != w->barValue) {
                    w->barValue = value;
                    // With the value, because that is the argument the handler
                    // names and uses: UIPanelScrollBarTemplate's body is
                    // self:GetParent():SetVerticalScroll(value), and a nil
                    // there scrolls to zero - so dragging a scroll bar snapped
                    // the view back to the top instead of moving it.
                    callFrameScriptNumber(pressedWid_[0], "OnValueChanged", value);
                }
            }
        }
        // A colour picker follows the cursor the same way, and for the same
        // reason: dragging off the wheel and letting go there should not throw
        // the drag away. Which of the two it is tracking was decided on the
        // press, so sliding from the wheel onto the bar does not switch.
        if (auto* w = widgets_.get(pressedWid_[0]);
            w && w->objectType == "ColorSelect" && pickerPart_ != 0) {
            const wowee::ui::Widget* part = widgets_.get(pickerPart_);
            if (part) {
                float hsv[3] = {w->pickerHSV[0], w->pickerHSV[1], w->pickerHSV[2]};
                if (part->colorRole == wowee::ui::Widget::ColorRole::Wheel) {
                    // Hue is the angle and saturation the distance out, both
                    // measured from the middle of the wheel. Past the rim is
                    // fully saturated rather than nothing, so a drag that
                    // leaves the disc keeps choosing a hue.
                    const float cx = part->left + part->rectW * 0.5f;
                    const float cy = part->bottom + part->rectH * 0.5f;
                    const float radius = std::min(part->rectW, part->rectH) * 0.5f;
                    const float dx = x - cx, dy = y - cy;
                    if (radius > 0.0f) {
                        float h = std::atan2(dy, dx) / 6.2831853f;
                        hsv[0] = h - std::floor(h);
                        hsv[1] = std::clamp(std::sqrt(dx * dx + dy * dy) / radius,
                                            0.0f, 1.0f);
                    }
                } else if (part->colorRole == wowee::ui::Widget::ColorRole::Value) {
                    const float extent = part->rectH;
                    if (extent > 0.0f) {
                        hsv[2] = std::clamp((y - part->bottom) / extent, 0.0f, 1.0f);
                    }
                }
                if (hsv[0] != w->pickerHSV[0] || hsv[1] != w->pickerHSV[1] ||
                    hsv[2] != w->pickerHSV[2]) {
                    w->pickerHSV[0] = hsv[0];
                    w->pickerHSV[1] = hsv[1];
                    w->pickerHSV[2] = hsv[2];
                    wowee::ui::hsvToRgb(hsv, w->pickerColor);
                    // The same script SetColorRGB fires, because the swatch
                    // and the caller's func hang off it and neither knows or
                    // cares which end the colour came from.
                    callFrameScriptColor(pressedWid_[0], "OnColorSelect",
                                         w->pickerColor);
                }
            }
        }
    }

    // The names WoW uses, in the order the state arrays are indexed.
    struct Button { const char* name; bool down; };
    const Button pressed[kMouseButtons] = {
        {"LeftButton",   buttons.left},
        {"RightButton",  buttons.right},
        {"MiddleButton", buttons.middle},
    };

    for (int i = 0; i < kMouseButtons; ++i) {
        const Button& b = pressed[i];
        if (b.down && !buttonDown_[i]) {
            buttonDown_[i] = true;
            pressedWid_[i] = clickOwnerOf(hit, b.name);
            // Bring the window to the front, the way clicking one does in WoW.
            // The nearest frame at or above what was hit that asked to be
            // toplevel - a click lands on a button inside the window, not on
            // the window itself, so raising only what was hit would raise
            // nothing. Done on the press so the frame is already in front
            // while the click is still being held.
            for (uint32_t id = hit; id != 0;) {
                const auto* cand = widgets_.get(id);
                if (!cand) break;
                if (cand->topLevel) { widgets_.raise(id); break; }
                id = cand->parent;
            }
            pressX_[i] = x;
            pressY_[i] = y;
            // Which half of a colour picker this press landed on, decided once
            // here. The wheel and the bar are textures, and a texture does not
            // take the mouse - the press lands on the ColorSelect frame itself
            // - so the only way to know is to ask which of its regions the
            // cursor is inside.
            if (i == 0) {
                pickerPart_ = 0;
                const auto* pw = pressedWid_[0] ? widgets_.get(pressedWid_[0]) : nullptr;
                if (pw && pw->objectType == "ColorSelect") {
                    for (uint32_t id : pw->children) {
                        const auto* c = widgets_.get(id);
                        if (!c) continue;
                        const bool pickable =
                            c->colorRole == wowee::ui::Widget::ColorRole::Wheel ||
                            c->colorRole == wowee::ui::Widget::ColorRole::Value;
                        if (!pickable) continue;
                        if (x >= c->left && x <= c->left + c->rectW &&
                            y >= c->bottom && y <= c->bottom + c->rectH) {
                            pickerPart_ = id;
                            break;
                        }
                    }
                }
            }
            // Clicking into an edit box takes focus; clicking anywhere else
            // gives it up, which is what makes a chat box stop eating keys.
            if (i == 0) {
                const auto* hw = hit ? widgets_.get(hit) : nullptr;
                setEditFocus(hw && hw->isEditBox ? hit : 0);
            }
            if (pressedWid_[i] != 0)
                callFrameScript(pressedWid_[i], "OnMouseDown", b.name);
        } else if (!b.down && buttonDown_[i]) {
            buttonDown_[i] = false;
            if (pressedWid_[i] != 0) {
                callFrameScript(pressedWid_[i], "OnMouseUp", b.name);
                // A click is press and release on the same frame, which is what
                // lets a player slide off a button to change their mind.
                const auto* pressed = widgets_.get(pressedWid_[i]);
                // A disabled button is greyed and takes no clicks. Setting
                // enabled without honouring it here would be the same shape of
                // half-feature as drawing a scroll frame's clip without
                // clipping its hit test: a scroll arrow at the end of its
                // range would still scroll.
                // A drag that ends is not also a click. The frame that was
                // dragged is told to stop, and whatever the cursor was let go
                // over is offered what is being carried - which is how an item
                // moves from one bag slot to another.
                const bool wasDragged = (draggingWid_ != 0 && draggingButton_ == i);
                if (wasDragged) {
                    callFrameScript(draggingWid_, "OnDragStop", b.name);
                    widgets_.setMovingWidget(0);
                    // Up through the parents to whatever is listening, the same
                    // way a click resolves through clickOwnerOf.
                    //
                    // The two paths disagreeing is the whole of "the spell
                    // stays on the cursor and never drops on the bar". A click
                    // has walked up since it was written, so pressing a button
                    // works however its rect is covered; the drop used the raw
                    // hit, so a drag ended on whatever frame happened to be
                    // topmost there and offered it to something with no
                    // OnReceiveDrag. Nothing raises: the handler is absent, the
                    // cursor keeps what it is carrying, and the bar looks like
                    // it refused the spell.
                    const uint32_t dropOn = dropOwnerOf(hit);
                    // Said out loud, the way the click below it is. A click
                    // reports which of its three conditions refused it because
                    // "the button looks right and does nothing" is otherwise
                    // unreadable; a drop has exactly the same problem and was
                    // saying nothing at all. Dragging a spell to the bar and
                    // having it stay on the cursor was reported, and the log it
                    // came with had a line for the press, a line for the drag
                    // starting and a line for the release - and nothing about
                    // where what was being carried actually went.
                    if (const auto* target = dropOn ? widgets_.get(dropOn) : nullptr) {
                        LOG_WARNING("WidgetInput: drop on ",
                                    target->name.empty() ? "(unnamed)"
                                                         : target->name.c_str(),
                                    dropOn == draggingWid_
                                        ? " - the frame it was dragged from, so nothing was offered"
                                        : " - OnReceiveDrag ran");
                    } else {
                        const auto* under = hit ? widgets_.get(hit) : nullptr;
                        LOG_WARNING("WidgetInput: drop over ",
                                    under && !under->name.empty() ? under->name.c_str()
                                                                  : "nothing",
                                    " - nothing at or above it takes a drop, so the "
                                    "cursor keeps what it is carrying");
                    }
                    if (dropOn != 0 && dropOn != draggingWid_) {
                        callFrameScript(dropOn, "OnReceiveDrag", b.name);
                    } else if (dropOn == 0 && L_) {
                        // Let go over the world, which is how an action is
                        // taken off a bar: the slot emptied when it was picked
                        // up, so clearing the cursor is what removes it. A drop
                        // on a frame that does not accept drags keeps it, as
                        // the real client does, so only the world counts.
                        const auto* under = hit ? widgets_.get(hit) : nullptr;
                        const bool onWorld = !under || under->name == "UIParent" ||
                                             under->name == "WorldFrame";
                        if (onWorld) {
                            lua_getglobal(L_, "ClearCursor");
                            if (lua_isfunction(L_, -1)) {
                                if (lua_pcall(L_, 0, 0, 0) != 0) lua_pop(L_, 1);
                            } else {
                                lua_pop(L_, 1);
                            }
                        }
                    }
                    const auto* target = dropOn ? widgets_.get(dropOn)
                                                : (hit ? widgets_.get(hit) : nullptr);
                    // Which frame took it, and - when none did - which one was
                    // under the cursor, since those are different questions and
                    // the second is what says why nothing happened.
                    const auto* under = hit ? widgets_.get(hit) : nullptr;
                    LOG_WARNING("WidgetInput: drag dropped on ",
                                target && !target->name.empty() ? target->name.c_str()
                                                                : "nothing",
                                dropOn == 0 && hit != 0
                                    ? " - nothing there or above it takes a drop" : "",
                                // The frame actually under the cursor, always,
                                // because "nothing" has two causes and they
                                // need different fixes: no frame there at all,
                                // or a frame there that no one up its chain
                                // listens for.
                                ", cursor was over ",
                                under && !under->name.empty() ? under->name.c_str()
                                       : (hit ? "(unnamed)" : "no frame"));
                    draggingWid_ = 0;
                    draggingButton_ = -1;
                }

                // Which button this is, for the whole of the dispatch below:
                // a modified-click binding names a button as well as a
                // modifier, and IsModifiedClick has no other way to know.
                struct ClickButtonScope {
                    explicit ClickButtonScope(const char* name) {
                        wowee::addons::currentClickButton() = name ? name : "";
                    }
                    ~ClickButtonScope() { wowee::addons::currentClickButton().clear(); }
                } clickButtonScope{b.name};

                const bool takesIt = frameAcceptsClick(pressedWid_[i], b.name);
                // Resolved the same way the press was, or a press on a bar and
                // a release on the same bar would compare an ancestor against a
                // child and never match.
                const uint32_t releasedOn = clickOwnerOf(hit, b.name);

                // A link is not the frame's click to take.
                //
                // This used to sit inside the block below, which needs the
                // frame under the cursor to accept an ordinary click. A chat
                // window does not: ChatFrameTemplate declares OnHyperlinkClick
                // and FloatingChatFrameTemplate sets enableMouse="false" over
                // it, because a chat window is click-through by design. So the
                // press landed on no widget, the gate never opened, and no item
                // link in chat could be clicked - while the same link in a
                // tooltip or a quest frame, which do take clicks, worked.
                //
                // FrameXML puts the handler on the chat frame rather than on
                // the font string the text is drawn in, so it is looked for up
                // the parent chain. A frame that declares none lets the click
                // carry on as an ordinary one.
                //
                // A frame can turn its own links off without giving up the
                // handler: FCF_SetUninteractable does exactly that to a window
                // made click-through, and the GM chat addon does it while a
                // ticket is being written. The click then carries on to
                // whatever is underneath, as it would if the link were not
                // there.
                bool tookLink = false;
                if (!wasDragged) {
                    if (const ui::LinkRect* hitLink = widgets_.linkAt(x, y)) {
                        const uint32_t owner = ui::findScriptOwner(
                            widgets_, hitLink->widget, [this](uint32_t w) {
                                return frameHasScript(w, "OnHyperlinkClick");
                            });
                        const ui::Widget* ownerW = owner ? widgets_.get(owner) : nullptr;
                        // Only when the link is what the mouse is actually on.
                        //
                        // A link rect is filed wherever its text was drawn and
                        // says nothing about what has been opened over the top
                        // of it since. Hoisting this check out of the block
                        // below took that away with the gate: a window drawn
                        // over the chat left the chat still answering clicks
                        // through it, because the rect was still there.
                        //
                        // Nothing under the cursor at all is the chat's own
                        // case - the window is click-through, so the hit test
                        // finds no widget and the link is the only thing there.
                        const bool linkIsWhatIsUnderTheCursor =
                            hit == 0 || ui::isSelfOrDescendantOf(widgets_, hit, owner);
                        if (owner != 0 && ownerW && ownerW->hyperlinksEnabled &&
                            linkIsWhatIsUnderTheCursor) {
                            callFrameScript3(owner, "OnHyperlinkClick",
                                             hitLink->link.c_str(),
                                             hitLink->text.c_str(), b.name);
                            tookLink = true;
                        }
                    }
                }

                if (!tookLink && !wasDragged && pressedWid_[i] == releasedOn &&
                    (!pressed || pressed->enabled) && takesIt) {
                    // PreClick and PostClick bracket the click. Secure buttons
                    // use them to set up and tear down around an action, and
                    // an addon that only has PostClick would otherwise never
                    // hear that its button was used.
                    // A check button flips itself before its handler runs,
                    // and nothing here did it - checked moved only when
                    // Lua called SetChecked. Handlers that read their own
                    // state to decide what a click meant therefore saw the
                    // same answer every time.
                    //
                    // The mail inbox is the clearest of them:
                    // InboxFrame_OnClick opens the letter when
                    // self:GetChecked() and hides OpenMailFrame when not,
                    // so every click on a message took the closing branch
                    // and no mail could be read. Every options checkbox
                    // reading GetChecked in its OnClick had the same fault.
                    //
                    // Before PreClick, which is where WoW does it, so a
                    // handler calling SetChecked itself still has the last
                    // word.
                    if (auto* cb = widgets_.get(pressedWid_[i]);
                        cb && cb->enabled && cb->objectType == "CheckButton") {
                        cb->checked = !cb->checked;
                    }
                    callFrameScript(pressedWid_[i], "PreClick", b.name);
                    callFrameScript(pressedWid_[i], "OnClick", b.name);
                    callFrameScript(pressedWid_[i], "PostClick", b.name);

                    // A second click on the same frame, soon enough after the
                    // first, is also a double click - WoW sends both, and the
                    // frames that care about one usually care about the other.
                    const double now = core::appTimeSeconds();
                    if (pressedWid_[i] == lastClickWid_ &&
                        (now - lastClickTime_) <= kDoubleClickSeconds) {
                        callFrameScript(pressedWid_[i], "OnDoubleClick", b.name);
                        // Cleared, or a third click reads as a second double.
                        lastClickWid_ = 0;
                        lastClickTime_ = 0.0;
                    } else {
                        lastClickWid_ = pressedWid_[i];
                        lastClickTime_ = now;
                    }
                }
                // The other half of the press report: a click that lands and a
                // click that is handled are different things, and the gap
                // between them is where a button that looks right does
                // nothing. Says which of the three conditions refused it.
                if (pressedWid_[i] != 0 && pressed && !pressed->name.empty()) {
                    LOG_WARNING("WidgetInput: release on ", pressed->name,
                                pressedWid_[i] != releasedOn ? " - cursor had moved off it"
                                : !pressed->enabled  ? " - the frame is disabled"
                                : !takesIt           ? " - it did not register for this button"
                                                     : " - OnClick ran");
                }
            }
            pressedWid_[i] = 0;
        }
    }
}

/// The frame a click on `wid` belongs to: the nearest one, itself or above it,
/// that registered for this button.
///
/// A click lands on the topmost frame taking the mouse, which is not always the
/// one meant to answer it. A unit frame's health bar takes the mouse so it can
/// show its numbers on hover, and it sits over the button that does the
/// targeting - so without this, clicking a target frame anywhere but its border
/// did nothing at all. Drags already resolve their owner this way.
///
/// Falls back to the frame that was hit when nothing above it wants the button,
/// so the refusal is still reported against the frame the player actually
/// clicked rather than against UIParent.
/// The frame a drop belongs to: the first at or above `wid` with an
/// OnReceiveDrag.
///
/// Zero when nothing up the chain takes one, which is a real answer - dropping
/// on empty screen is how an item is destroyed, and that path needs to know the
/// difference between "nobody wanted it" and "it went to the frame under the
/// cursor".
uint32_t LuaEngine::dropOwnerOf(uint32_t wid) {
    for (uint32_t id = wid; id != 0;) {
        const auto* cand = widgets_.get(id);
        if (!cand) break;
        if (frameHasScript(id, "OnReceiveDrag")) return id;
        id = cand->parent;
    }
    return 0;
}

uint32_t LuaEngine::clickOwnerOf(uint32_t wid, const char* button) {
    for (uint32_t id = wid; id != 0;) {
        const auto* cand = widgets_.get(id);
        if (!cand) break;
        if (frameAcceptsClick(id, button)) return id;
        id = cand->parent;
    }
    return wid;
}

/// Ask the interface what it can see, in its own words.
///
/// The widget report says whether a frame is shown; this says whether it should
/// be. A hidden target frame is correct with no target and a fault with one, and
/// from the tree alone those are the same line.
void LuaEngine::runInterfaceProbe() {
    if (!L_) return;
    const bool ok = executeString(
        "local function yn(v) return v and 'yes' or 'no' end\n"
        "local auras = 0\n"
        "for i = 1, 40 do if not UnitAura('player', i) then break end auras = i end\n"
        "__WoweeWarn('[fxcheck] target=' .. yn(UnitExists('target')) ..\n"
        "      ' name=' .. tostring(UnitExists('target') and UnitName('target')) ..\n"
        "      ' | TargetFrame shown=' .. yn(TargetFrame and TargetFrame:IsShown()) ..\n"
        "      ' unit=' .. tostring(TargetFrame and TargetFrame.unit) ..\n"
        "      ' | player auras=' .. auras ..\n"
        "      ' | XP=' .. tostring(UnitXP('player')) .. '/' .. tostring(UnitXPMax('player')) ..\n"
        "      ' | bag0 slots=' .. tostring(GetContainerNumSlots(0)))\n"
        // The player frame's top-left icon comes from these three, and which
        // one is answering wrongly is not visible from the widget tree: the
        // texture is set from Lua, so an icon that should not be there looks
        // exactly like one that should.
        "__WoweeWarn('[fxcheck] pvp=' .. yn(UnitIsPVP('player')) ..\n"
        "      ' ffa=' .. yn(UnitIsPVPFreeForAll('player')) ..\n"
        "      ' faction=' .. tostring(UnitFactionGroup('player')) ..\n"
        "      ' | PlayerPVPIcon shown=' ..\n"
        "      yn(PlayerPVPIcon and PlayerPVPIcon:IsShown()))\n"
        // The state icons share one sheet and one corner with the PvP icon, so
        // "a badge at the top left" does not say which of them it is.
        "__WoweeWarn('[fxcheck] resting=' .. yn(IsResting()) ..\n"
        "      ' combat=' .. yn(PlayerFrame.inCombat) ..\n"
        "      ' | rest icon=' .. yn(PlayerRestIcon and PlayerRestIcon:IsShown()) ..\n"
        "      ' attack icon=' .. yn(PlayerAttackIcon and PlayerAttackIcon:IsShown()) ..\n"
        "      ' status=' .. yn(PlayerStatusTexture and PlayerStatusTexture:IsShown()))\n");
    if (!ok) LOG_WARNING("interface probe did not run: ", lastError());
}

/// Age every message frame's lines and fade the ones whose time is up.
///
/// A frame with no declared duration keeps its lines lit for good. Two declare
/// one: UIErrorsFrame at five seconds, and the chat template at a hundred and
/// twenty. Both attributes were dropped, so every server refusal stayed on
/// screen until a hundred and twenty-eight had piled up behind it.
///
/// Faded rather than removed. A chat line that has gone quiet is still in the
/// history and comes back when the frame is scrolled, which is why nothing is
/// erased here and why scrolling lights everything again - the player reading
/// back wants what was said, not what is still fresh.
void LuaEngine::expireMessages(float elapsed) {
    for (size_t id = 1; id < widgets_.size(); ++id) {
        auto* w = widgets_.get(static_cast<uint32_t>(id));
        if (!w || !w->isMessageFrame || w->messageDuration <= 0.0f) continue;
        const bool readingBack = w->messageScroll > 0;
        const float life = w->messageDuration;
        const float fade = w->messageFadeDuration > 0.0f ? w->messageFadeDuration : 0.0f;
        for (auto& m : w->messages) {
            m.age += elapsed;
            if (readingBack) { m.color[3] = 1.0f; continue; }
            const float left = life - m.age;
            m.color[3] = (left <= 0.0f) ? 0.0f
                       : (fade > 0.0f && left < fade) ? (left / fade)
                       : 1.0f;
        }
    }
}

/// Run the OnTextChanged handlers owed by text set from code since the last
/// frame.
///
/// The queue is taken and cleared before anything runs, so a handler that sets
/// text again is queued for the next frame rather than extending this drain -
/// which is what stops MoneyInputFrame's own edits from chasing their own tail.
/// Run the OnAnimFinished handlers owed by frames shown to play an animation
/// this client does not play.
void LuaEngine::drainPendingAnimFinished() {
    if (!L_) return;
    lua_getglobal(L_, "__WoweePendingAnimFinished");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    const int count = static_cast<int>(lua_objlen(L_, -1));
    if (count == 0) { lua_pop(L_, 1); return; }
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweePendingAnimFinished");
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L_, -1, i);
        if (lua_istable(L_, -1)) callScriptOnTable(L_, lua_gettop(L_), "OnAnimFinished", 0);
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

void LuaEngine::drainPendingTextChanged() {
    if (!L_) return;
    drainPendingAnimFinished();
    lua_getglobal(L_, "__WoweePendingTextChanged");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    const int count = static_cast<int>(lua_objlen(L_, -1));
    if (count == 0) { lua_pop(L_, 1); return; }

    // Swap in a fresh table first: a handler that sets text lands in the new
    // one and waits a frame.
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweePendingTextChanged");

    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L_, -1, i);
        if (lua_istable(L_, -1)) {
            // The caret first. Setting text moves it to the end, which in WoW
            // is a cursor change, and ScrollingEdit_OnCursorChanged is the only
            // thing that ever sets self.cursorOffset - which
            // ScrollingEdit_OnTextChanged then negates. Firing the text change
            // without the cursor change reaches `-nil` on the first code-set
            // text in every multi-line box: the mail body, the macro editor,
            // the guild information pane.
            if (const auto* w = widgetOf(L_, lua_gettop(L_))) {
                fireCursorChanged(w->id);
            }
            callScriptOnTable(L_, lua_gettop(L_), "OnTextChanged", 0);
        }
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

void LuaEngine::dispatchOnUpdate(float elapsed) {
    // Asked for by the check, and answered here because only this side can ask
    // the interface anything.
    if (ui::frameXmlTakeProbeRequest()) runInterfaceProbe();

    expireMessages(elapsed);

    if (!L_) return;

    drainPendingTextChanged();

    // Animations first, so a frame's own OnUpdate sees this frame's values
    // rather than the previous one's.
    lua_getglobal(L_, "__WoweeTickAnimations");
    if (lua_isfunction(L_, -1)) {
        lua_pushnumber(L_, elapsed);
        if (lua_pcall(L_, 1, 0, 0) != 0) {
            LOG_WARNING("animation tick error: ", luaL_optstring(L_, -1, "?"));
            lua_pop(L_, 1);
        }
    } else {
        lua_pop(L_, 1);
    }

    lua_getglobal(L_, "__WoweeOnUpdateFrames");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }

    int count = static_cast<int>(lua_objlen(L_, -1));
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L_, -1, i);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); continue; }

        // Visible, meaning shown with every ancestor shown too - which is what
        // decides whether WoW runs an OnUpdate at all.
        //
        // This read __visible, a field beside the frame that Show and Hide
        // write, so it answered whether the frame itself had been shown and
        // said nothing about its parents. Alt-Z hides UIParent and the movie
        // frame hides it too; with the UI off, all hundred and ten of
        // FrameXML's OnUpdate handlers went on running every frame, doing work
        // for a screen nobody could see and advancing timers that should have
        // stopped.
        //
        // The widget already carries the answer: layout() resolves `visible`
        // for the whole tree each frame, so this costs a lookup rather than a
        // walk up the parents.
        // A frame with no widget behind it falls back to the field, so
        // anything built outside the tree keeps working rather than going
        // silently still.
        bool visible;
        if (const auto* uw = widgetOf(L_, lua_gettop(L_))) {
            // The chain, not `visible`: an unanchored frame is not drawn and is
            // still running, which is exactly what FrameXML's driver frames
            // are. frameFadeManager, frameFlashManager and AnimUpdateFrame are
            // each a CreateFrame that is never positioned and carries nothing
            // but an OnUpdate - asking whether they would be drawn stops every
            // fade, flash and animation in the interface.
            visible = uw->visibleChain;
        } else {
            lua_getfield(L_, -1, "__visible");
            visible = lua_toboolean(L_, -1);
            lua_pop(L_, 1);
        }
        if (!visible) { lua_pop(L_, 1); continue; }

        // Get OnUpdate script
        lua_getfield(L_, -1, "__scripts");
        if (lua_istable(L_, -1)) {
            // Below the function, so a handler that fails every frame says
            // where it was reached from rather than only which line broke.
            lua_pushcfunction(L_, luaTracebackHandler);
            const int hIdx = lua_gettop(L_);
            lua_getfield(L_, hIdx - 1, "OnUpdate");
            if (lua_isfunction(L_, -1)) {
                lua_pushvalue(L_, hIdx - 2);  // self (frame)
                lua_pushnumber(L_, static_cast<double>(elapsed));
                if (pcallScript(L_, "OnUpdate", 2, hIdx) != 0) {
                    const char* uerr = lua_tostring(L_, -1);
                    std::string uerrStr = uerr ? uerr : "(unknown)";
                    lua_pop(L_, 1);

                    // A handler that fails once will fail every frame, and this
                    // runs every frame: five broken OnUpdates produced five and
                    // a half thousand identical errors in one session, which
                    // costs time and buries everything else in the log.
                    //
                    // After a few tries the handler is unhooked and said so
                    // once. The frame keeps working - it simply stops being
                    // asked to do the thing it cannot do.
                    // Indexed from the handler rather than the top: hIdx - 1
                    // is __scripts, and the traceback handler now sits above
                    // it, so the old relative offsets pointed at the wrong
                    // table.
                    constexpr int kMaxConsecutiveFailures = 5;
                    const int scriptsIdx = hIdx - 1;
                    lua_getfield(L_, scriptsIdx, "__onUpdateFailures");
                    const int failures = static_cast<int>(lua_tointeger(L_, -1)) + 1;
                    lua_pop(L_, 1);
                    lua_pushinteger(L_, failures);
                    lua_setfield(L_, scriptsIdx, "__onUpdateFailures");

                    // Which frame, by name. Disabling an OnUpdate stops
                    // whatever that frame drives, for the rest of the session,
                    // and the symptom is never an error message - it is a
                    // thing on screen that has stopped moving. Blizzard_
                    // CombatText is the shape of it: its OnUpdate is the only
                    // thing that ages a floating message out, so a handler
                    // that dies leaves every message that has been queued
                    // sitting there for ever with nothing to say why.
                    const auto* fw = widgetOf(L_, hIdx - 2);
                    const char* fname = (fw && !fw->name.empty())
                                            ? fw->name.c_str() : "(unnamed)";
                    if (failures >= kMaxConsecutiveFailures) {
                        lua_pushnil(L_);
                        lua_setfield(L_, scriptsIdx, "OnUpdate");
                        LOG_ERROR("LuaEngine: OnUpdate disabled on '", fname,
                                  "' after ", failures, " failures - whatever "
                                  "it drives has stopped: ", uerrStr);
                        noteLuaError(uerrStr);
        if (luaErrorCallback_) luaErrorCallback_(uerrStr);
                    } else if (failures == 1) {
                        LOG_ERROR("LuaEngine: OnUpdate error on '", fname,
                                  "': ", uerrStr);
                        noteLuaError(uerrStr);
        if (luaErrorCallback_) luaErrorCallback_(uerrStr);
                    }
                } else {
                    // Consecutive, so a handler that recovers is not punished
                    // for an early stumble.
                    lua_pushinteger(L_, 0);
                    lua_setfield(L_, hIdx - 1, "__onUpdateFailures");
                }
            } else {
                lua_pop(L_, 1);   // the OnUpdate field, which was not a function
            }
            lua_pop(L_, 1);       // the traceback handler
        }
        lua_pop(L_, 2); // pop __scripts + frame
    }
    lua_pop(L_, 1); // pop __WoweeOnUpdateFrames
}

bool LuaEngine::dispatchSlashCommand(const std::string& command, const std::string& args) {
    if (!L_) return false;

    // Check each SlashCmdList entry: for key NAME, check SLASH_NAME1, SLASH_NAME2, etc.
    lua_getglobal(L_, "SlashCmdList");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }

    std::string cmdLower = command;
    toLowerInPlace(cmdLower);

    lua_pushnil(L_);
    while (lua_next(L_, -2) != 0) {
        // Stack: SlashCmdList, key, handler
        if (!lua_isfunction(L_, -1) || !lua_isstring(L_, -2)) {
            lua_pop(L_, 1);
            continue;
        }
        const char* name = lua_tostring(L_, -2);

        // Check SLASH_<NAME>1 through SLASH_<NAME>9
        for (int i = 1; i <= 9; i++) {
            std::string globalName = "SLASH_" + std::string(name) + std::to_string(i);
            lua_getglobal(L_, globalName.c_str());
            if (lua_isstring(L_, -1)) {
                std::string slashStr = lua_tostring(L_, -1);
                toLowerInPlace(slashStr);
                if (slashStr == cmdLower) {
                    lua_pop(L_, 1); // pop global
                    // Call the handler with args
                    lua_pushvalue(L_, -1); // copy handler
                    lua_pushstring(L_, args.c_str());
                    if (lua_pcall(L_, 1, 0, 0) != 0) {
                        const char* serr = lua_tostring(L_, -1);
                        LOG_ERROR("LuaEngine: SlashCmdList['", name, "'] error: ",
                                  serr ? serr : "?");
                        noteLuaError(std::string("/") + name + ": " + (serr ? serr : "?"));
                        lua_pop(L_, 1);
                    }
                    lua_pop(L_, 3); // pop handler, key, SlashCmdList
                    return true;
                }
            }
            lua_pop(L_, 1); // pop global
        }
        lua_pop(L_, 1); // pop handler, keep key for next iteration
    }
    lua_pop(L_, 1); // pop SlashCmdList
    return false;
}

// ---- SavedVariables serialization ----

static void serializeLuaValue(lua_State* L, int idx, std::string& out, int indent);

static void serializeLuaTable(lua_State* L, int idx, std::string& out, int indent) {
    out += "{\n";
    std::string pad(indent + 2, ' ');
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        out += pad;
        // Key
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* k = lua_tostring(L, -2);
            out += "[\"";
            for (const char* p = k; *p; ++p) {
                if (*p == '"' || *p == '\\') out += '\\';
                out += *p;
            }
            out += "\"] = ";
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
            out += "[" + std::to_string(static_cast<long long>(lua_tonumber(L, -2))) + "] = ";
        } else {
            lua_pop(L, 1);
            continue;
        }
        // Value
        serializeLuaValue(L, lua_gettop(L), out, indent + 2);
        out += ",\n";
        lua_pop(L, 1);
    }
    out += std::string(indent, ' ') + "}";
}

static void serializeLuaValue(lua_State* L, int idx, std::string& out, int indent) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:     out += "nil"; break;
        case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; break;
        case LUA_TNUMBER: {
            double v = lua_tonumber(L, idx);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.17g", v);
            out += buf;
            break;
        }
        case LUA_TSTRING: {
            const char* s = lua_tostring(L, idx);
            out += "\"";
            for (const char* p = s; *p; ++p) {
                if (*p == '"' || *p == '\\') out += '\\';
                else if (*p == '\n') { out += "\\n"; continue; }
                else if (*p == '\r') continue;
                out += *p;
            }
            out += "\"";
            break;
        }
        case LUA_TTABLE:
            serializeLuaTable(L, idx, out, indent);
            break;
        default:
            out += "nil"; // Functions, userdata, etc. can't be serialized
            break;
    }
}

void LuaEngine::setAddonList(const std::vector<TocFile>& addons) {
    if (!L_) return;
    lua_pushnumber(L_, static_cast<double>(addons.size()));
    lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_addon_count");

    lua_newtable(L_);
    for (size_t i = 0; i < addons.size(); i++) {
        lua_newtable(L_);
        lua_pushstring(L_, addons[i].addonName.c_str());
        lua_setfield(L_, -2, "name");
        lua_pushstring(L_, addons[i].getTitle().c_str());
        lua_setfield(L_, -2, "title");
        auto notesIt = addons[i].directives.find("Notes");
        lua_pushstring(L_, notesIt != addons[i].directives.end() ? notesIt->second.c_str() : "");
        lua_setfield(L_, -2, "notes");
        // Store all TOC directives for GetAddOnMetadata
        lua_newtable(L_);
        for (const auto& [key, val] : addons[i].directives) {
            lua_pushstring(L_, val.c_str());
            lua_setfield(L_, -2, key.c_str());
        }
        lua_setfield(L_, -2, "metadata");
        // Marked, because this list doubles as the answer to IsAddOnLoaded and
        // a load-on-demand addon is listed long before it is loaded. Without
        // the flag, adding them here would report every one of them as loaded
        // from the moment the client started.
        lua_pushboolean(L_, addons[i].isLoadOnDemand() ? 1 : 0);
        lua_setfield(L_, -2, "loadOnDemand");
        lua_rawseti(L_, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_addon_info");
}

bool LuaEngine::loadSavedVariables(const std::string& path) {
    if (!L_) return false;
    std::ifstream f(path);
    if (!f.is_open()) return false; // No saved data yet - not an error
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty()) return true;
    int err = luaL_dostring(L_, content.c_str());
    if (err != 0) {
        LOG_WARNING("LuaEngine: error loading saved variables from '", path, "': ",
                    lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaEngine::saveSavedVariables(const std::string& path, const std::vector<std::string>& varNames) {
    if (!L_ || varNames.empty()) return false;
    std::string output;
    for (const auto& name : varNames) {
        lua_getglobal(L_, name.c_str());
        if (!lua_isnil(L_, -1)) {
            output += name + " = ";
            serializeLuaValue(L_, lua_gettop(L_), output, 0);
            output += "\n";
        }
        lua_pop(L_, 1);
    }
    if (output.empty()) return true;

    // Ensure directory exists
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, lastSlash), ec);
    }

    // Written to a temporary and renamed over the destination.
    //
    // Opening the real file truncates it before the first byte is written, so a
    // kill during logout - or a full disk - left an empty or half-written Lua
    // file where the addon's settings used to be, and the next launch parsed
    // that instead. The whole point of this file is that it survives the
    // session; losing it to the write that was meant to preserve it is the one
    // outcome worth engineering against.
    const std::string tempPath = path + ".tmp";
    std::error_code ec;

    {
        std::ofstream f(tempPath, std::ios::trunc);
        if (!f.is_open()) {
            LOG_WARNING("LuaEngine: cannot write saved variables to '", path, "'");
            return false;
        }
        f << output;
        f.flush();
        if (!f.good()) {
            LOG_WARNING("LuaEngine: failed writing saved variables for '", path,
                        "'; keeping the previous file");
            f.close();
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        LOG_WARNING("LuaEngine: could not replace '", path, "': ", ec.message());
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        return false;
    }

    LOG_INFO("LuaEngine: saved variables to '", path, "' (", output.size(), " bytes)");
    return true;
}

namespace {

/// Appends the Lua call stack to an error message.
///
/// An error says where it happened; the interesting part is nearly always how
/// it got there. "dropdownMenu is nil at unitpopup.lua:484" cost several rounds
/// of reading to trace back to the OnLoad that started it, and the stack was
/// there the whole time - it just was not being asked for. Installed as the
/// message handler so it runs before the stack unwinds.
///
/// Written by hand rather than through debug.traceback because the debug
/// library is deliberately not opened.
int luaTracebackHandler(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    std::string out = msg ? msg : "(error)";
    for (int level = 1; level < 12; ++level) {
        lua_Debug ar;
        if (!lua_getstack(L, level, &ar)) break;
        if (!lua_getinfo(L, "Sln", &ar)) break;
        out += "\n      at ";
        out += (ar.short_src[0] ? ar.short_src : "?");
        out += ":" + std::to_string(ar.currentline);
        if (ar.name) { out += " in "; out += ar.name; }
    }
    lua_pushstring(L, out.c_str());
    return 1;
}

/// Loads and runs a chunk with the traceback handler in place. Returns the
/// same non-zero-on-error convention as luaL_dostring.
int runChunk(lua_State* L, const char* chunk, size_t len, const char* name) {
    const int base = lua_gettop(L);
    lua_pushcfunction(L, luaTracebackHandler);
    if (luaL_loadbuffer(L, chunk, len, name) != 0) {
        // A syntax error has no stack to walk; leave the message where the
        // caller expects it and drop the handler underneath it.
        lua_remove(L, base + 1);
        return 1;
    }
    const int rc = lua_pcall(L, 0, 0, base + 1);
    lua_remove(L, base + 1);
    return rc;
}

/// When the running chunk must give up. Wall clock rather than a count of VM
/// instructions: the runaway this was written for spends nearly all its time
/// inside one C binding - a table rehash that grows with every call - so it
/// executes very few Lua instructions per second and a generous instruction
/// budget never came due while the client sat frozen.
std::chrono::steady_clock::time_point gChunkDeadline{};

/// Reports where the VM actually is - the Lua source and line - which a C++
/// backtrace cannot tell you: that only names the binding being called, not
/// the loop calling it.
void runawayHook(lua_State* L, lua_Debug*) {
    if (std::chrono::steady_clock::now() < gChunkDeadline) return;

    std::string where = "unknown";
    lua_Debug info;
    if (lua_getstack(L, 0, &info) && lua_getinfo(L, "Sl", &info)) {
        where = std::string(info.short_src[0] ? info.short_src : "?") + ":" +
                std::to_string(info.currentline);
    }
    // Several levels of it, because the innermost line is often a helper and
    // the loop that will not end is the caller.
    for (int level = 1; level < 6; ++level) {
        lua_Debug up;
        if (!lua_getstack(L, level, &up) || !lua_getinfo(L, "Sln", &up)) break;
        LOG_ERROR("LuaEngine:   called from ",
                  up.short_src[0] ? up.short_src : "?", ":", up.currentline,
                  up.name ? " in " : "", up.name ? up.name : "");
    }
    // Off before unwinding, or it fires again inside the error path.
    lua_sethook(L, nullptr, 0, 0);
    LOG_ERROR("LuaEngine: runaway script aborted at ", where);
    luaL_error(L, "runaway script aborted at %s", where.c_str());
}

/// Installs the deadline for one chunk and takes it off again however that
/// chunk leaves - including by error, which is the case that matters.
struct BudgetGuard {
    lua_State* L;
    explicit BudgetGuard(lua_State* state, unsigned long long ms) : L(state) {
        if (L && ms > 0) {
            gChunkDeadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(ms);
            // Every few hundred instructions. A deadline is only as sharp as
            // how often it is looked at, and a loop whose every iteration sits
            // in a slow C call executes very few per second: at 10,000
            // this overran 5s by 43s and then by 99s before the check came
            // round. The check is a clock read, which costs nothing beside the
            // work it is bounding.
            lua_sethook(L, runawayHook, LUA_MASKCOUNT, 500);
        }
    }
    ~BudgetGuard() { if (L) lua_sethook(L, nullptr, 0, 0); }
};

} // namespace

void LuaEngine::bootstrap(const char* code) {
    if (luaL_dostring(L_, code) == 0) return;
    const char* e = lua_tostring(L_, -1);
    const std::string head(code, std::min<size_t>(70, std::strlen(code)));
    LOG_ERROR("LuaEngine: bootstrap chunk failed: ", e ? e : "?",
              "  [chunk began: ", head, "]");
    // The most consequential of all of them: bootstrap defines what FrameXML
    // is loaded on top of, so a chunk that does not run takes out everything
    // downstream of it and never says so again.
    noteLuaError(std::string("bootstrap chunk failed: ") + (e ? e : "?") +
                 "  [began: " + head + "]");
    lua_pop(L_, 1);
}

bool LuaEngine::executeFile(const std::string& path) {
    if (!L_) return false;

    BudgetGuard guard(L_, chunkTimeoutMs_);
    // Read and run rather than luaL_dofile, so the traceback handler is in
    // place: a file that fails deep inside a handler otherwise reports only
    // the line that broke, never the OnLoad that reached it.
    std::string source;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            lastError_ = "cannot open " + path;
            LOG_ERROR("LuaEngine: cannot open '", path, "'");
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        source = ss.str();
    }
    const std::string chunkName = "@" + path;
    int err = runChunk(L_, source.c_str(), source.size(), chunkName.c_str());
    if (err != 0) {
        const char* errMsg = lua_tostring(L_, -1);
        std::string msg = errMsg ? errMsg : "(unknown error)";
        lastError_ = msg;
        LOG_ERROR("LuaEngine: error loading '", path, "': ", msg);
        noteLuaError(msg);
        if (luaErrorCallback_) luaErrorCallback_(msg);
        if (gameHandler_) {
            game::MessageChatData errChat;
            errChat.type = game::ChatType::SYSTEM;
            errChat.language = game::ChatLanguage::UNIVERSAL;
            errChat.message = "|cffff4040[Lua Error] " + msg + "|r";
            gameHandler_->addLocalChatMessage(errChat);
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

/// Run an expression and answer whether it came out true.
///
/// The stack is left as it was found, whatever the chunk does: this runs from
/// the input path, once per key press, and a value left behind on each one
/// grows until the state is exhausted.
bool LuaEngine::evaluateBoolean(const std::string& expression) {
    if (!L_ || expression.empty()) return false;
    const std::string chunk = "return (" + expression + ")";
    const int base = lua_gettop(L_);
    BudgetGuard guard(L_, chunkTimeoutMs_);
    if (runChunk(L_, chunk.c_str(), chunk.size(), chunk.c_str()) != 0) {
        const char* err = lua_tostring(L_, -1);
        LOG_WARNING("LuaEngine: ", expression, " - ", err ? err : "(unknown)");
        // Recorded, not only logged. This answers questions the client asks
        // the interface, and a false is what it gets whether the answer was no
        // or the question raised - the two are indistinguishable from the call
        // site. Escape runs through here: it asks whether FrameXML closed a
        // window before deciding to open the game menu, so a raise inside
        // CloseAllWindows reads as "nothing was open" and the chain moves on
        // as though nothing were wrong.
        noteLuaError("evaluating '" + expression + "': " + (err ? err : "?"));
        lua_settop(L_, base);
        return false;
    }
    const bool truth = lua_toboolean(L_, -1) != 0;
    lua_settop(L_, base);
    return truth;
}

bool LuaEngine::executeString(const std::string& code) {
    if (!L_) return false;

    BudgetGuard guard(L_, chunkTimeoutMs_);
    int err = runChunk(L_, code.c_str(), code.size(), code.c_str());
    if (err != 0) {
        const char* errMsg = lua_tostring(L_, -1);
        std::string msg = errMsg ? errMsg : "(unknown error)";
        lastError_ = msg;
        LOG_ERROR("LuaEngine: script error: ", msg);
        noteLuaError(msg);
        if (luaErrorCallback_) luaErrorCallback_(msg);
        if (gameHandler_) {
            game::MessageChatData errChat;
            errChat.type = game::ChatType::SYSTEM;
            errChat.language = game::ChatLanguage::UNIVERSAL;
            errChat.message = "|cffff4040[Lua Error] " + msg + "|r";
            gameHandler_->addLocalChatMessage(errChat);
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

} // namespace wowee::addons
