// lua_widget_internal.hpp - the seam between lua_engine.cpp and
// lua_widget_api.cpp.
//
// The widget bindings moved to their own translation unit on 2026-08-26. These
// twelve names are the whole of what the two halves share: five helpers the
// bindings are built on, and the handful of globals registerBaseGlobals still
// installs from the engine side.
//
// They had internal linkage while everything lived in one file. Keeping the
// list short is the point - it is the measure of how separable the widget
// surface is, and anything added here is coupling that was not there before.
#pragma once

#include <set>
#include <string>

#include "ui/widget_tree.hpp"

struct lua_State;

namespace wowee::addons {

class LuaEngine;

/// The widget id on a frame table, or 0 when the value is not a frame.
uint32_t widgetIdOf(lua_State* L, int index);

/// The widget behind a frame table, or null before the UI is up.
wowee::ui::Widget* widgetOf(lua_State* L, int index);

/// The engine behind this state, from the registry.
LuaEngine* engineFrom(lua_State* L);

/// Run one script handler off a frame table's __scripts, with a number.
void callScriptOnTable(lua_State* L, int tableIdx, const char* script, double arg);

/// pcall a handler already on the stack, applying the chunk timeout.
int pcallScript(lua_State* L, const char* script, int nargs, int nresults);

/// Report a handler that raised, once, naming the script.
void recordScriptError(lua_State* L, const char* script);

/// Names asked for and not found while the fallback is on.
std::set<std::string>& missingApiNames();

// Registered by registerBaseGlobals, which stayed with the engine.
int lua_wow_print(lua_State* L);
int lua_wow_message(lua_State* L);
int lua_wowee_warn(lua_State* L);
int lua_wowee_setAnimOffset(lua_State* L);
int lua_GetMouseFocus(lua_State* L);

}  // namespace wowee::addons
