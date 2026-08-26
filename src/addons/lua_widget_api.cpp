// lua_widget_api.cpp - the widget surface: every Region, Frame, Texture,
// FontString, EditBox, StatusBar, Cooldown, Slider, ColorSelect, MessageFrame,
// ScrollFrame and Tooltip binding, and the registration that installs them.
//
// Split out of lua_engine.cpp on 2026-08-26. It is the largest and most
// contract-sensitive part of the Lua API, and the one the per-domain split of
// §5.1 never reached: the bindings had internal linkage, and the registration
// that named them sat two thousand lines away inside registerCoreAPI.
//
// The seam with lua_engine.cpp is twelve names, declared in
// lua_widget_internal.hpp. Everything else in here is private to this file.

#include "addons/lua_engine.hpp"
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
#include "core/macos_platform.hpp"
#include <filesystem>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "addons/lua_widget_internal.hpp"

namespace wowee {
namespace addons {

// Defined in lua_engine.cpp, beside the edit-box focus state they touch.
int lua_EditBox_SetFocus(lua_State* L);
int lua_EditBox_ClearFocus(lua_State* L);
int lua_EditBox_HasFocus(lua_State* L);

namespace {

// Forward declarations that travelled with their definitions.
static int lua_Frame_UnregisterEvent(lua_State* L);
int lua_FontString_SetText(lua_State* L);

/// What the item actually does, appended to the two lines above.
///
/// The name and its quality colour were the whole of the tooltip, which reads
/// as a tooltip that works right up until someone wants to know whether the
/// sword is better than the one they are holding.
///
/// **This is the fallback, not the tooltip.** The bootstrap Lua's
/// _WoweePopulateItemTooltip is the builder every item path now goes through -
/// see fillItemTooltipById(lua_State*, ...) for why that direction - and this
/// runs only when it answers false, which means the interface has no
/// GetItemInfo for the entry yet. Anything added here is therefore seen rarely
/// and briefly; the line to add is almost always the Lua one.
///
/// Ordered as WoW orders it: binding, then what it is and where it goes, then
/// the numbers, then the requirements, then the flavour text last.
static void appendItemStats(wowee::ui::Widget* w, const game::ItemQueryResponseData& info) {
    auto line = [&w](std::string text, float r, float g, float b) {
        wowee::ui::Widget::TooltipLine l;
        l.left = std::move(text);
        l.lc[0] = r; l.lc[1] = g; l.lc[2] = b; l.lc[3] = 1.0f;
        l.rc[0] = l.rc[1] = l.rc[2] = l.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(l));
    };
    constexpr float kW = 1.0f, kGrey = 0.62f, kGold = 1.0f;
    auto white = [&](std::string s) { line(std::move(s), kW, kW, kW); };
    auto grey  = [&](std::string s) { line(std::move(s), kGrey, kGrey, kGrey); };
    auto gold  = [&](std::string s) { line(std::move(s), kGold, 0.82f, 0.0f); };
    auto green = [&](std::string s) { line(std::move(s), 0.0f, 1.0f, 0.0f); };

    // This one used to stop at 3, so a quest item's tooltip said nothing about
    // being one here while the other two tooltips said so.
    if (const char* bindText = game::itemBindText(info.bindType)) white(bindText);
    if (info.maxCount == 1)                     gold("Unique");
    else if (info.itemFlags & 0x1000000u)       gold("Unique-Equipped");

    if (info.containerSlots > 0) {
        white(std::to_string(info.containerSlots) + " Slot Container");
    }

    // A weapon says its damage, its speed and the two multiplied out, because
    // damage alone compares two weapons wrongly whenever their speeds differ.
    if (info.damageMax > 0.0f) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.0f - %.0f Damage",
                      static_cast<double>(info.damageMin), static_cast<double>(info.damageMax));
        white(buf);
        if (info.delayMs > 0) {
            const double speed = info.delayMs / 1000.0;
            std::snprintf(buf, sizeof(buf), "Speed %.2f", speed);
            white(buf);
            const double dps = (info.damageMin + info.damageMax) / 2.0 / speed;
            std::snprintf(buf, sizeof(buf), "(%.1f damage per second)", dps);
            grey(buf);
        }
    }
    if (info.armor > 0) white(std::to_string(info.armor) + " Armor");

    const std::pair<int32_t, const char*> kBase[] = {
        {info.strength,  "Strength"},  {info.agility, "Agility"},
        {info.stamina,   "Stamina"},   {info.intellect, "Intellect"},
        {info.spirit,    "Spirit"},
    };
    for (const auto& [v, name] : kBase) {
        if (v != 0) white((v > 0 ? "+" : "") + std::to_string(v) + " " + name);
    }
    const std::pair<int32_t, const char*> kRes[] = {
        {info.holyRes, "Holy"},   {info.fireRes,   "Fire"},  {info.natureRes, "Nature"},
        {info.frostRes, "Frost"}, {info.shadowRes, "Shadow"}, {info.arcaneRes, "Arcane"},
    };
    for (const auto& [v, name] : kRes) {
        if (v != 0) white("+" + std::to_string(v) + " " + name + " Resistance");
    }
    for (const auto& es : info.extraStats) {
        if (es.statValue == 0) continue;
        if (const char* n = game::itemStatName(es.statType)) {
            green(std::string(es.statValue > 0 ? "+" : "") + std::to_string(es.statValue) +
                  " " + n);
        }
    }

    if (info.requiredLevel > 0) white("Requires Level " + std::to_string(info.requiredLevel));
    if (!info.description.empty()) gold("\"" + info.description + "\"");
}

/// gh is what turns the name into a tooltip: the ItemDef in a bag slot carries
/// the name and the quality, and everything a player actually reads a tooltip
/// for lives in the item cache behind it.
static void fillItemTooltip(wowee::ui::Widget* w, const game::ItemDef& item,
                            game::GameHandler* gh) {
    w->isTooltip = true;
    // A setter that finds something to say also shows the tooltip. That is
    // WoW's behaviour and FrameXML leans on it: ContainerFrameItemButton_OnEnter
    // sets an owner, calls SetBagItem and stops - there is no Show anywhere in
    // it, so a tooltip that only filled itself in stayed hidden and hovering a
    // bag said nothing.
    w->shown = true;
    w->tooltipLines.clear();
    wowee::ui::Widget::TooltipLine title;
    // The suffix an instance rolled, which is part of the name a player reads:
    // "Bracers of the Bear" is one item to them and a base item plus a
    // ItemRandomSuffix.dbc row here. The client's own bag window has appended
    // it all along; the tooltip FrameXML asks for showed the base name, so the
    // same item read differently depending on which window was open.
    title.left = item.name;
    if (item.randomPropertyId != 0 && gh) {
        const std::string suffix = gh->getRandomPropertyName(item.randomPropertyId);
        if (!suffix.empty()) title.left += " " + suffix;
    }
    // WoW's quality colours, which are most of what an item tooltip says at a
    // glance - an epic reads as purple before anyone reads the words.
    // The client's own table rather than a fourth copy. This used to carry its
    // own, and the copies had already drifted: it painted an heirloom cyan
    // where ui_colors paints it the same gold as an artifact. Cyan is a later
    // expansion's token colour; a 3.3.5 heirloom is e6cc80.
    const ImVec4 qc = wowee::ui::getQualityColor(item.quality);
    title.lc[0] = qc.x; title.lc[1] = qc.y; title.lc[2] = qc.z; title.lc[3] = 1.0f;
    title.rc[0] = title.rc[1] = title.rc[2] = title.rc[3] = 1.0f;
    w->tooltipLines.push_back(std::move(title));

    if (!item.subclassName.empty()) {
        wowee::ui::Widget::TooltipLine sub;
        sub.left = item.subclassName;
        sub.lc[0] = sub.lc[1] = sub.lc[2] = 1.0f; sub.lc[3] = 1.0f;
        sub.rc[0] = sub.rc[1] = sub.rc[2] = sub.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(sub));
    }

    if (gh && item.itemId != 0) {
        if (const auto* info = gh->getItemInfo(item.itemId); info && info->valid) {
            appendItemStats(w, *info);
        }
    }
}

/// IsEventRegistered(event) → whether this frame asked for it.
///
/// Answered from the frame's own __events table, which RegisterEvent writes
/// and UnregisterEvent clears - the same table the dispatch reads, so the
/// answer cannot drift from the behaviour.
///
/// One caller in the interface: bnet.lua asks before it adds a conversation
/// to a chat window. Addons use it more, and a no-op answering nothing is the
/// wrong shape for a question - it reads as "no" for a frame that is
/// registered, and re-registering is not always harmless.
static int lua_Frame_IsEventRegistered(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* eventName = luaL_optstring(L, 2, nullptr);
    if (!eventName || !*eventName) { lua_pushboolean(L, 0); return 1; }
    lua_getfield(L, 1, "__events");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 0); return 1; }
    lua_getfield(L, -1, eventName);
    const bool on = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    lua_pushboolean(L, on ? 1 : 0);
    return 1;
}

static int lua_Frame_RegisterEvent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);  // self
    const char* eventName = luaL_checkstring(L, 2);

    // Get frame's registered events table (create if needed)
    lua_getfield(L, 1, "__events");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, 1, "__events");
    }
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, eventName);
    lua_pop(L, 1);

    // Also register in global __WoweeFrameEvents for dispatch
    lua_getglobal(L, "__WoweeFrameEvents");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "__WoweeFrameEvents");
    }
    lua_getfield(L, -1, eventName);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, eventName);
    }
    // Append the frame, but only if it is not already listening.
    //
    // The frame's own __events table is keyed by name and so is already
    // idempotent; this list is not. Registering the same event twice put the
    // frame in it twice and its OnEvent then ran twice for every one of those
    // events - and UnregisterEvent removes the first match and stops, so one
    // unregister could not undo a double register. Nothing about the pair is
    // symmetric unless the insert refuses duplicates.
    const int len = static_cast<int>(lua_objlen(L, -1));
    bool already = false;
    for (int i = 1; i <= len && !already; ++i) {
        lua_rawgeti(L, -1, i);
        already = lua_rawequal(L, -1, 1) != 0;
        lua_pop(L, 1);
    }
    if (!already) {
        lua_pushvalue(L, 1);  // push frame
        lua_rawseti(L, -2, len + 1);
    }
    lua_pop(L, 2);  // pop list + __WoweeFrameEvents
    return 0;
}

// Frame method: frame:UnregisterEvent("EVENT")
/// UnregisterAllEvents() - stop listening to everything at once.
///
/// A no-op before this, which is the dangerous half of the pair: the frame
/// believes it has stopped and goes on being handed every event it ever asked
/// for. PlayerFrame does it to the mana bar when the player takes a vehicle,
/// and the handler that keeps running then reads a bar the vehicle art has
/// already replaced.
///
/// Walks the frame's own table and unregisters each name through the same path
/// a single UnregisterEvent takes, so the global dispatch list is cleaned the
/// way it already knows how rather than by a second copy of that logic.
static int lua_Frame_UnregisterEvent(lua_State* L);
static int lua_Frame_UnregisterAllEvents(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__events");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    // Collected first: unregistering rewrites this table, and a traversal that
    // is being written to is undefined.
    std::vector<std::string> names;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_isstring(L, -2)) names.emplace_back(lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    for (const std::string& n : names) {
        // Through a real call, not by calling the C function in place: it
        // reads its arguments at stack indices one and two, and pushing them
        // on top of this frame's own leaves it reading ours instead.
        lua_pushcfunction(L, lua_Frame_UnregisterEvent);
        lua_pushvalue(L, 1);                 // self
        lua_pushstring(L, n.c_str());        // event
        lua_call(L, 2, 0);
    }
    return 0;
}

static int lua_Frame_UnregisterEvent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* eventName = luaL_checkstring(L, 2);

    // Remove from frame's own events
    lua_getfield(L, 1, "__events");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, eventName);
    }
    lua_pop(L, 1);

    // And from the global list dispatch actually reads. Clearing only the
    // frame's own table left the registration in __WoweeFrameEvents, so a
    // frame went on being handed events it had asked to stop receiving -
    // which is not a missed refresh but an error, because the handler runs in
    // a state its own OnHide has already torn down. The paperdoll's equipment
    // flyout unregisters and nils self.button together, then indexed that nil
    // on the next inventory change.
    lua_getglobal(L, "__WoweeFrameEvents");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, eventName);
        if (lua_istable(L, -1)) {
            const int listeners = lua_objlen(L, -1);
            // Walk backwards so a removal cannot skip the next entry.
            for (int i = listeners; i >= 1; --i) {
                lua_rawgeti(L, -1, i);
                const bool isSelf = lua_rawequal(L, -1, 1);
                lua_pop(L, 1);
                if (!isSelf) continue;
                // table.remove semantics: shift the tail down one.
                for (int j = i; j < listeners; ++j) {
                    lua_rawgeti(L, -1, j + 1);
                    lua_rawseti(L, -2, j);
                }
                lua_pushnil(L);
                lua_rawseti(L, -2, listeners);
                break;
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}

// Frame method: frame:SetScript("handler", func)
static int lua_Frame_SetScript(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* scriptType = luaL_checkstring(L, 2);
    // arg 3 can be function or nil
    lua_getfield(L, 1, "__scripts");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, 1, "__scripts");
    }
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, scriptType);
    lua_pop(L, 1);

    // Track frames with OnUpdate in __WoweeOnUpdateFrames
    //
    // Once each. Clearing an OnUpdate leaves the frame on this list - the
    // dispatcher skips it because the script is gone - so setting one again
    // appended a second entry, and the frame was then ticked twice a frame
    // with the same elapsed. Start-and-stop is the ordinary shape for this
    // handler (UIFrameFade installs one for the fade and clears it at the
    // end), so a frame that faded five times ran its next OnUpdate five times
    // over and every timer driven by elapsed ran that many times too fast.
    if (strcmp(scriptType, "OnUpdate") == 0) {
        lua_getglobal(L, "__WoweeOnUpdateFrames");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
        if (lua_isfunction(L, 3)) {
            const int len = static_cast<int>(lua_objlen(L, -1));
            bool already = false;
            for (int i = 1; i <= len && !already; ++i) {
                lua_rawgeti(L, -1, i);
                already = lua_rawequal(L, -1, 1) != 0;
                lua_pop(L, 1);
            }
            if (!already) {
                lua_pushvalue(L, 1);
                lua_rawseti(L, -2, len + 1);
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}

// Frame method: frame:GetScript("handler")
static int lua_Frame_GetScript(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* scriptType = luaL_checkstring(L, 2);
    lua_getfield(L, 1, "__scripts");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, scriptType);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// Frame method: frame:GetName()
static int lua_Frame_GetName(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__name");
    return 1;
}

/// The same widget, with its rect resolved first.
///
/// For the getters that answer a measurement. A rect used to be whatever the
/// once-a-frame pass had last written, so a frame anchored inside a handler
/// measured as if it had never been placed and every measurement taken during
/// that handler was of the frame's own size sitting at the origin. The real
/// client resolves when asked, and the interface is written expecting that -
/// it anchors a row and immediately reads the edge it landed on.
///
/// Costs nothing when nothing has moved, which is nearly every call: the pass
/// only runs again if something raised the flag since the last one.
wowee::ui::Widget* measuredWidgetOf(lua_State* L, int index) {
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree) return nullptr;
    const uint32_t id = widgetIdOf(L, index);
    tree->resolveWidget(id);
    return tree->get(id);
}

/// A name a script handed to an anchoring call, resolved to a widget.
///
/// $parent is not only markup. FrameXML writes it in runtime strings too -
/// OptionsListButton_OnLoad anchors its own label with
///
///     self.text:SetPoint("RIGHT", "$parentToggle", "LEFT", -2, 0)
///
/// and the token means there what it means in the XML: the parent of the
/// region the call is on. Looked up literally it is nothing, and SetPoint's
/// fallback then anchors to the parent itself - so that label's RIGHT edge
/// landed on the button's LEFT edge, two units inside its own LEFT, and the
/// region came out at negative width. That is every row of the Video, Sound
/// and Interface category lists: registered, selectable, and drawn as an empty
/// box with the panels beside it working perfectly.
///
/// Twenty-two sites across FrameXML and the Blizzard addons write a name this
/// way, the achievement frame's three columns among them.
uint32_t widgetIdByAnchorName(lua_State* L, int selfIndex, const char* name) {
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree || !name) return 0;
    std::string resolved = name;
    if (resolved.compare(0, 7, "$parent") == 0) {
        const wowee::ui::Widget* self = tree->get(widgetIdOf(L, selfIndex));
        const wowee::ui::Widget* parent = self ? tree->get(self->parent) : nullptr;
        if (!parent || parent->name.empty()) return 0;
        resolved = parent->name + resolved.substr(7);
    }
    lua_getglobal(L, resolved.c_str());
    uint32_t id = lua_istable(L, -1) ? widgetIdOf(L, lua_gettop(L)) : 0;
    lua_pop(L, 1);
    // A region named in its markup is published as a global; one named at
    // runtime is not always, and the tree knows both.
    if (id == 0) {
        if (const wowee::ui::Widget* w = tree->findByName(resolved)) id = w->id;
    }
    return id;
}

// SetPoint(point [, relativeTo] [, relativePoint] [, x, y]) - every argument
// after the first is optional and the shapes overlap, so decide by type rather
// than by count.
int lua_Region_SetPoint(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;

    wowee::ui::Anchor a;
    a.point = luaL_optstring(L, 2, "CENTER");
    int argi = 3;
    if (lua_istable(L, argi)) {
        a.relativeTo = widgetIdOf(L, argi);
        ++argi;
    } else if (lua_isstring(L, argi) && !lua_isnumber(L, argi)) {
        // A name rather than the frame itself, $parent included, which is how
        // FrameXML refers to most things.
        a.relativeTo = widgetIdByAnchorName(L, 1, lua_tostring(L, argi));
        ++argi;
    } else if (lua_isnil(L, argi)) {
        // Explicitly nil, which means the parent - and which still occupies its
        // place in the argument list. Skipping over it read the relative point
        // as the relative frame and everything after it moved up one, so
        // SetPoint(point, nil, "BOTTOM") silently became point-to-point on the
        // parent. FrameXML passes nil here constantly, and a name that failed
        // to resolve arrives the same way.
        ++argi;
    }
    // Anchoring to itself is not a position, and a name can resolve to the
    // frame that was just published under it.
    if (a.relativeTo == id) {
        const auto* self = tree->get(id);
        a.relativeTo = self ? self->parent : 0;
    }
    if (lua_isstring(L, argi) && !lua_isnumber(L, argi)) {
        a.relativePoint = lua_tostring(L, argi);
        ++argi;
    } else {
        a.relativePoint = a.point;   // Blizzard's default is the same point
    }
    if (lua_isnumber(L, argi))     a.x = static_cast<float>(lua_tonumber(L, argi));
    if (lua_isnumber(L, argi + 1)) a.y = static_cast<float>(lua_tonumber(L, argi + 1));

    tree->addPoint(id, a);
    return 0;
}

int lua_Region_ClearAllPoints(lua_State* L) {
    if (auto* tree = wowee::addons::getWidgetTree(L)) tree->clearPoints(widgetIdOf(L, 1));
    return 0;
}

int lua_Region_SetAllPoints(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    uint32_t target = 0;
    if (lua_istable(L, 2)) target = widgetIdOf(L, 2);
    else if (lua_isstring(L, 2)) {
        target = widgetIdByAnchorName(L, 1, lua_tostring(L, 2));
    }
    // A frame cannot fill itself. FrameXML's own UIParent is declared
    // setAllPoints and its parent is named UIParent - but CreateFrame publishes
    // the new frame under that name first, so by the time this runs the name
    // means the frame itself. Two identical constraints collapse to no size at
    // the origin, and everything anchored to it lands there too.
    const auto* w = tree->get(id);
    if (target == 0 || target == id) target = w ? w->parent : 0;
    tree->setAllPoints(id, target);
    return 0;
}

int lua_Region_SetSize(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (tree && id != 0) {
        tree->setWidth(id, static_cast<float>(luaL_optnumber(L, 2, 0)));
        tree->setHeight(id, static_cast<float>(luaL_optnumber(L, 3, 0)));
    }
    return 0;
}

// Moving a frame, and picking things up out of one.
//
// All five of these were no-ops, which is why a bag window could not be
// dragged anywhere and an item could not be dragged out of it: the interface
// asked to be moved and nothing was listening.
/// SetRotation(radians) / SetFacing(radians) on a model frame.
///
/// The paperdoll's rotate buttons keep a running total and call this with it,
/// so it is an absolute facing. Unimplemented, the buttons ran their handler
/// and the figure never moved.
int lua_Model_SetFacing(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->modelFacing = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    }
    return 0;
}

int lua_Model_GetFacing(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->modelFacing : 0.0);
    return 1;
}

int lua_Frame_SetMovable(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->movable = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Frame_IsMovable(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->movable);
    return 1;
}

/// RegisterForDrag("LeftButton", ...) - naming none of them turns dragging off,
/// which is how a frame stops being draggable again.
int lua_Frame_RegisterForDrag(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    w->dragLeft = false;
    w->dragRight = false;
    const int n = lua_gettop(L);
    for (int i = 2; i <= n; ++i) {
        const char* b = lua_tostring(L, i);
        if (!b) continue;
        const std::string name(b);
        if (name == "LeftButton")       w->dragLeft = true;
        else if (name == "RightButton") w->dragRight = true;
    }
    return 0;
}

int lua_Frame_StartMoving(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    const auto* w = tree->get(id);
    // WoW raises on a frame that is not movable. Ignoring it loses nothing and
    // keeps a mistake in one frame from taking down the file that asked.
    if (!w || !w->movable) return 0;
    tree->pinToCurrentPosition(id);
    tree->setMovingWidget(id);
    return 0;
}

int lua_Frame_StopMovingOrSizing(lua_State* L) {
    if (auto* tree = wowee::addons::getWidgetTree(L)) {
        tree->setMovingWidget(0);
        tree->setSizingWidget(0, "");
    }
    return 0;
}

/// StartSizing(point) - take hold of a frame by one of its corners.
///
/// The size grabber on the chat window does this on mouse down, and it was a
/// no-op: the grabber lit up, the cursor changed to the resize arrows, and
/// nothing moved. Moving a frame worked the whole time, which made the missing
/// half easy to miss.
int lua_Frame_StartSizing(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    const auto* w = tree->get(id);
    if (!w || !w->resizable) return 0;
    // BOTTOMRIGHT is what the grabber art sits on and what FrameXML passes when
    // it passes nothing.
    const char* point = luaL_optstring(L, 2, "BOTTOMRIGHT");
    tree->pinToCurrentPosition(id);
    tree->setSizingWidget(id, point ? point : "BOTTOMRIGHT");
    return 0;
}

int lua_Frame_SetResizable(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    if (auto* w = tree->get(id)) w->resizable = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Frame_IsResizable(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    const auto* w = (tree && id) ? tree->get(id) : nullptr;
    lua_pushboolean(L, w && w->resizable ? 1 : 0);
    return 1;
}

int lua_Frame_SetMinResize(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    if (auto* w = tree->get(id)) {
        w->minResizeW = static_cast<float>(luaL_optnumber(L, 2, 0));
        w->minResizeH = static_cast<float>(luaL_optnumber(L, 3, 0));
    }
    return 0;
}

int lua_Frame_SetMaxResize(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    if (auto* w = tree->get(id)) {
        w->maxResizeW = static_cast<float>(luaL_optnumber(L, 2, 0));
        w->maxResizeH = static_cast<float>(luaL_optnumber(L, 3, 0));
    }
    return 0;
}

/// A region's name, taken from the tree rather than a Lua field so it is the
/// same name everything else knows it by.
///
/// Regions never had this: GetName fell to the no-op fallback and answered nil,
/// and FrameXML passes the answer straight into SetPoint -
/// bgTextureBottom:SetPoint("TOP", bgTextureMiddle:GetName(), "BOTTOM") anchored
/// a bag's bottom edge to nothing, which put it across the top of the bag.
int lua_FontString_SetText(lua_State* L);

/// SetFormattedText(fmt, ...) on a region.
///
/// It was defined on the frame metatable only, and a label is not a frame - so
/// every FontString in FrameXML still answered with the no-op and kept whatever
/// placeholder its XML carried. The character sheet went on reading "Level level
/// race class" for exactly that reason.
int lua_FontString_SetFormattedText(lua_State* L) {
    const int n = lua_gettop(L);
    if (n < 2 || !lua_isstring(L, 2)) return 0;

    // Through Lua's own string.format, so the format specifiers behave the way
    // the interface expects them to.
    lua_getglobal(L, "string");
    lua_getfield(L, -1, "format");
    lua_remove(L, -2);
    for (int i = 2; i <= n; ++i) lua_pushvalue(L, i);
    if (lua_pcall(L, n - 1, 1, 0) != 0) {
        // A format string and its arguments disagreeing is an error in Lua, and
        // losing the file that asked for a label is worse than an unformatted
        // one.
        lua_pop(L, 1);
        lua_pushvalue(L, 2);
    }
    lua_replace(L, 2);
    lua_settop(L, 2);
    return lua_FontString_SetText(L);
}

int lua_Region_GetName(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (w && !w->name.empty()) lua_pushstring(L, w->name.c_str());
    else lua_pushnil(L);
    return 1;
}

int lua_Region_SetWidth(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (tree && id != 0) tree->setWidth(id, static_cast<float>(luaL_optnumber(L, 2, 0)));
    return 0;
}

int lua_Region_SetHeight(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (tree && id != 0) tree->setHeight(id, static_cast<float>(luaL_optnumber(L, 2, 0)));
    return 0;
}

/// Width of a string as it would be drawn.
///
/// Through the real font where there is one, so a button sized to its label
/// gets the size the label actually takes. During the FrameXML load there may
/// not be a frame in flight to ask, and an estimate is far better than nothing:
/// the alternative was answering nil, and MoneyFrame does
/// SetWidth(GetTextWidth() + iconWidth) - arithmetic that loses the file.
float measureTextWidth(const std::string& text, const std::string& fontFace,
                       float fontHeight) {
    // Through the same function the renderer draws with, so a widget sized to
    // its own text gets the width its own text will occupy. Measuring in this
    // client's face at a flat twelve while drawing in the interface's at its
    // real height made every self-sized label narrower than what went in it -
    // MoneyFrame sizes all three coin buttons that way, and the gold, silver
    // and copper ran into one another.
    return wowee::ui::interfaceTextWidth(text, fontFace, fontHeight);
}

/// The font string a widget measures: itself if it is one, and otherwise the
/// one a button was given, which is where its text actually lives.
const wowee::ui::Widget* textWidgetOf(lua_State* L, int index) {
    const wowee::ui::Widget* w = widgetOf(L, index);
    if (!w || w->kind == wowee::ui::WidgetKind::FontString) return w;
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree) return w;
    lua_getfield(L, index, "__fontString");
    const wowee::ui::Widget* fs =
        lua_istable(L, -1) ? tree->get(widgetIdOf(L, lua_gettop(L))) : nullptr;
    lua_pop(L, 1);
    return fs ? fs : w;
}

int lua_Region_GetTextWidth(lua_State* L) {
    const auto* w = textWidgetOf(L, 1);
    lua_pushnumber(L, w ? measureTextWidth(w->text, w->fontFace, w->fontHeight) : 0.0);
    return 1;
}

// GameTooltip:SetMinimumWidth(width) / :GetMinimumWidth() - a floor on how
// narrow the tooltip may size itself.
//
// The pair has to be real together. SetTooltipMoney reads the getter first:
//
//     if ( frame:GetMinimumWidth() < moneyFrameWidth ) then
//
// and the no-op answered nil, so every caller of SetTooltipMoney raised on
// that line - the taxi node's flight cost, the mail money and COD lines, the
// repair-all cost, a dungeon's reward money. The tooltip drew whatever it had
// managed before the raise and the money was never added.
//
// The second argument to the setter is a "force" flag the real client uses to
// apply the width immediately rather than at the next layout. Ours sizes
// tooltips every frame, so there is nothing for it to force.
int lua_Frame_SetMinimumWidth(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->tooltipMinWidth = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    }
    return 0;
}

int lua_Frame_GetMinimumWidth(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->tooltipMinWidth : 0.0);
    return 1;
}

// FontString:GetFieldSize() → how many bytes of text this string will hold.
//
// One caller in all of FrameXML, and it is not asking out of curiosity:
// GuildEventLog_Update reads it into `max` and then compares a running byte
// count against it. Answering nil - which is what the no-op did, because
// nothing here defined the name - raises "attempt to compare number with nil"
// on the first event with a message, and the SetText at the end of the
// function never runs. The guild event log was blank whenever there was
// anything to put in it, and empty when there was not, so it looked consistent.
//
// Our font strings hold a std::string and truncate at nothing, so the true
// answer is "more than you have". Kept finite anyway, because the caller uses
// it as a number and a sentinel like HUGE_VAL would read oddly in arithmetic.
int lua_Region_GetFieldSize(lua_State* L) {
    (void)L;
    lua_pushnumber(L, 1 << 20);
    return 1;
}

int lua_Region_GetTextHeight(lua_State* L) {
    const auto* w = textWidgetOf(L, 1);
    if (!w) { lua_pushnumber(L, 0.0); return 1; }
    // Every line of it. This answered one line's height however many the label
    // drew, and FrameXML sizes panels from it - a quest description or an
    // options paragraph asked how tall it was, was told twelve, and the frame
    // around it was built to fit one line of the several on screen.
    // The size it is actually drawn at, which is what interfaceFontSize
    // answers. Three places asked this question with three different
    // fallbacks - twelve here, twelve below, fourteen in the edit box - and a
    // label with no declared height is drawn at none of them.
    const float line = wowee::ui::interfaceFontSize(w->fontHeight);
    const int lines = (w->wrappedLines > 0) ? w->wrappedLines : 1;
    lua_pushnumber(L, line * 1.2 * lines);
    return 1;
}

int lua_Region_GetWidth(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    // In the frame's own units, which is what WoW reports: the laid-out rect
    // has the effective scale in it and handing that back would have a script
    // that reads a width and sets it again shrink the frame every time.
    const float es = (w && w->effScale > 0.0f) ? w->effScale : 1.0f;
    if (w && w->rectW <= 0.0f && w->width <= 0.0f &&
        w->kind == wowee::ui::WidgetKind::FontString && !w->text.empty()) {
        // A font string that was never given a width is as wide as its text.
        // That is what WoW answers, and the interface sizes things from it:
        // PanelTemplates_TabResize builds a tab's width out of
        // _G[name.."Text"]:GetWidth(), so answering zero made every tab on the
        // character sheet collapse to the width of its two end textures, with
        // the label clipped out of sight inside it.
        lua_pushnumber(L, measureTextWidth(w->text, w->fontFace, w->fontHeight));
        return 1;
    }
    lua_pushnumber(L, w ? (w->rectW > 0.0f ? w->rectW / es : w->width) : 0.0);
    return 1;
}

int lua_Region_GetHeight(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    const float es = (w && w->effScale > 0.0f) ? w->effScale : 1.0f;
    lua_pushnumber(L, w ? (w->rectH > 0.0f ? w->rectH / es : w->height) : 0.0);
    return 1;
}

// The four edges, in the same coordinates the tree lays out in: origin at the
// bottom-left, y growing upward, interface units rather than pixels.
//
// These are read constantly and almost always into arithmetic - the chat frame
// works out where its dock sits, the container frames decide which side to
// open a tooltip on. A no-op behind them is not a getter that answers badly,
// it is nil in a subtraction, which takes the whole file down.
int lua_Region_GetLeft(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    lua_pushnumber(L, w ? w->left : 0.0);
    return 1;
}

/// GetCenter() - the middle of the frame, in the same screen units GetLeft and
/// GetBottom report.
///
/// There was a version of this that read __xOfs and __yOfs, fields written only
/// by a SetPoint that was defined beside it and registered nowhere. So it
/// answered the frame's anchor offsets when it answered anything, and zero for
/// every frame in the interface - which is what the minimap uses to turn a
/// click into a ping position, and what the world map uses to place its
/// markers.
int lua_Region_GetCenter(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    if (!w) { lua_pushnil(L); lua_pushnil(L); return 2; }
    lua_pushnumber(L, w->left + w->rectW * 0.5f);
    lua_pushnumber(L, w->bottom + w->rectH * 0.5f);
    return 2;
}

/// SetParent/GetParent. Shared with the region method table: a texture's
/// parent can be changed as well as a frame's, and that table used to carry
/// its own copy of both bodies.
static int lua_Region_SetParent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // A name is as good as a table here, and FrameXML passes both.
    if (lua_isstring(L, 2) && !lua_isnumber(L, 2)) {
        lua_getglobal(L, lua_tostring(L, 2));
        lua_replace(L, 2);
    }
    if (!lua_istable(L, 2) && !lua_isnil(L, 2)) return 0;
    lua_pushvalue(L, 2);
    lua_setfield(L, 1, "__parent");
    // And the widget, which is what everything inherited actually follows.
    // Writing only the field left GetParent answering the new parent while the
    // frame stayed laid out, clipped and shown by the old one.
    if (auto* tree = wowee::addons::getWidgetTree(L)) {
        const uint32_t id = widgetIdOf(L, 1);
        const uint32_t np = lua_istable(L, 2) ? widgetIdOf(L, 2) : 0;
        if (id != 0) tree->setParent(id, np);
    }
    return 0;
}

static int lua_Region_GetParent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__parent");
    return 1;
}

int lua_Region_GetRight(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    lua_pushnumber(L, w ? (w->left + w->rectW) : 0.0);
    return 1;
}

int lua_Region_GetBottom(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    lua_pushnumber(L, w ? w->bottom : 0.0);
    return 1;
}

int lua_Region_GetTop(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    lua_pushnumber(L, w ? (w->bottom + w->rectH) : 0.0);
    return 1;
}

/// GetRect() → left, bottom, width, height. All four at once, which is what
/// the newer code in FrameXML reaches for.
/// IsMouseOver() - whether the cursor is inside this frame's rect.
///
/// Answered from the rect rather than from hover, because hover names the
/// mouse-enabled frame under the cursor and the callers ask about frames that
/// are not: mainmenubarmicrobuttons.xml asks it of a micro button to decide
/// whether to put its tooltip back, and floatingchatframe.lua asks it of the
/// dock. It was in the no-op allowlist, so every one of those was false.
int lua_Region_IsMouseOver(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (!w || !w->visible) { lua_pushboolean(L, 0); return 1; }
    const float mx = wowee::addons::LuaEngine::lastMouseX();
    const float my = wowee::addons::LuaEngine::lastMouseY();
    const bool inside = mx >= w->left && mx <= w->left + w->rectW &&
                        my >= w->bottom && my <= w->bottom + w->rectH;
    lua_pushboolean(L, inside ? 1 : 0);
    return 1;
}

int lua_Region_GetRect(lua_State* L) {
    const auto* w = measuredWidgetOf(L, 1);
    if (!w) return 0;
    lua_pushnumber(L, w->left);
    lua_pushnumber(L, w->bottom);
    lua_pushnumber(L, w->rectW);
    lua_pushnumber(L, w->rectH);
    return 4;
}

/// Per-frame scale is not modelled - the tree scales the whole interface at
/// once, which is what UIParent's scale means and where the number FrameXML
/// wants comes from. One is therefore the true answer for every frame, and it
/// is a number, which is the part that matters where it is divided by.
int lua_Region_GetScale(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->scale : 1.0);
    return 1;
}

int lua_Region_SetScale(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        const float v = static_cast<float>(luaL_optnumber(L, 2, 1.0));
        // A scale of zero collapses the frame and everything under it to
        // nothing, with no way back from Lua; WoW rejects it too.
        if (v > 0.0f) w->scale = v;
    }
    return 0;
}

int lua_Region_GetEffectiveScale(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->effScale : 1.0);
    return 1;
}

int lua_Frame_GetFrameLevel(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->effLevel : 0);
    return 1;
}

/// GetPoint(n) → point, relativeTo, relativePoint, x, y.
///
/// The stub this replaces answered "CENTER", nil, "CENTER", 0, 0 for every
/// frame, which is not a getter answering roughly - FrameXML reads a point and
/// puts it back to move something (a dragged chat window, a frame the panel
/// manager shifts aside), and a constant means every one of those teleports to
/// the middle of its parent.
///
/// relativeTo comes back as the frame itself, not its name, because that is
/// what SetPoint is handed straight back in.
int lua_Region_GetPoint(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (!w || w->anchors.empty()) return 0;
    // One-based, and no argument means the first - which is what FrameXML
    // relies on where a frame has only one.
    size_t index = static_cast<size_t>(luaL_optnumber(L, 2, 1));
    if (index < 1) index = 1;
    if (index > w->anchors.size()) return 0;
    const wowee::ui::Anchor& a = w->anchors[index - 1];

    lua_pushstring(L, a.point.c_str());
    // Zero means "my parent", which SetPoint also treats as the default, so it
    // comes back as nil rather than as a frame that was never named.
    if (a.relativeTo == 0) {
        lua_pushnil(L);
    } else {
        lua_getglobal(L, "__WoweeFramesByWid");
        if (lua_istable(L, -1)) {
            lua_pushinteger(L, static_cast<lua_Integer>(a.relativeTo));
            lua_rawget(L, -2);
            lua_remove(L, -2);          // drop the registry, keep the frame
            if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); }
        } else {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
    }
    lua_pushstring(L, a.relativePoint.c_str());
    lua_pushnumber(L, a.x);
    lua_pushnumber(L, a.y);
    return 5;
}

/// The types a widget answers to, most specific first.
///
/// WoW's IsObjectType is true for the type itself and everything it derives
/// from - a Button is a Frame is a Region - and FrameXML relies on that: it
/// asks whether something is a Region to decide it can be positioned at all.
static bool objectTypeMatches(const std::string& actual, const std::string& asked) {
    if (actual == asked) return true;
    const bool isRegion = (actual == "Texture" || actual == "FontString");
    if (isRegion) {
        return asked == "LayeredRegion" || asked == "Region";
    }
    // Everything else this creates is a Frame or derives from one.
    return asked == "Frame" || asked == "Region";
}

int lua_Region_GetObjectType(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushstring(L, w ? w->objectType.c_str() : "Frame");
    return 1;
}

int lua_Region_IsObjectType(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    const char* asked = luaL_optstring(L, 2, "");
    lua_pushboolean(L, w && objectTypeMatches(w->objectType, asked));
    return 1;
}

/// Tells a tooltip that what it now holds is an item.
///
/// GameTooltip's own OnTooltipSetItem is where the comparison tooltips come
/// from - it tests IsModifiedClick("COMPAREITEMS") and calls
/// GameTooltip_ShowCompareItem - and nothing here fired it, so holding shift
/// over a bag, a bank slot, a merchant row or a link did nothing at all. Every
/// item setter goes through the builder this is called from, so it is fired
/// once there rather than in each of the twenty-odd setters.
///
/// The tooltip is the first argument at every one of those call sites, which
/// is what makes one index right for all of them.
static void fireTooltipSetItem(lua_State* L) {
    if (lua_istable(L, 1)) callScriptOnTable(L, 1, "OnTooltipSetItem", 0.0);
}

// Scroll frames. A window onto a taller child: the child is laid out at its
// full size and moved by the offset, and what falls outside the frame is
// clipped. Until now SetVerticalScroll was a no-op and every getter answered
// zero, so a scroll bar had nothing to report and nothing to move.
int lua_ScrollFrame_SetScrollChild(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    tree->markScrollFrame(id);
    if (auto* w = tree->get(id)) {
        w->scrollChild = lua_istable(L, 2) ? widgetIdOf(L, 2) : 0;
    }
    // Kept on the table too, because GetScrollChild hands the frame itself
    // back and that is what the caller passed in.
    lua_pushvalue(L, 2);
    lua_setfield(L, 1, "__scrollChild");
    return 0;
}

/// How far the child can move before its far edge reaches the frame's. Zero
/// when the child fits, which is how a scroll bar knows to disable itself.
static float scrollRange(wowee::ui::WidgetTree& tree, uint32_t id, bool vertical) {
    const auto* w = tree.get(id);
    if (!w || w->scrollChild == 0) return 0.0f;
    // Both rects resolved first, the same way the measuring getters do it.
    //
    // Only matters for a scroll child whose height comes out of the solver -
    // one anchored TOP and BOTTOM to something rather than given a height.
    // SetHeight writes the rect directly, so a stated size was always readable
    // here; a solved one was not, and answered zero.
    //
    // Which was worse than a wrong number, because the two disagreed: the
    // child answered its full height when asked directly and the range
    // answered nothing, and a range of nothing is how a scroll bar decides it
    // is not needed.
    const uint32_t childId = w->scrollChild;
    tree.resolveWidget(id);
    tree.resolveWidget(childId);
    w = tree.get(id);
    const auto* child = tree.get(childId);
    if (!w || !child) return 0.0f;
    // What the child actually holds, not what it declares. Nothing in FrameXML
    // sizes the talent tree's scroll child - the client does - so the declared
    // 50 gave a range of zero on a tree eleven rows deep.
    float contentW = child->rectW, contentH = child->rectH;
    tree.scrollContentExtent(childId, contentW, contentH);
    const float over = vertical ? (contentH - w->rectH) : (contentW - w->rectW);
    return over > 0.0f ? over : 0.0f;
}

int lua_ScrollFrame_SetVerticalScroll(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    if (auto* w = tree->get(id)) {
        const float v = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        const float max = scrollRange(*tree, id, true);
        const float clamped = (v < 0.0f) ? 0.0f : (v > max ? max : v);
        const bool moved = (clamped != w->scrollY);
        w->scrollY = clamped;
        // OnVerticalScroll is how the scroll bar beside the frame learns where
        // the frame now is; UIPanelScrollFrameTemplate's body sets the bar's
        // value from it. Announced only on a change, because the interface
        // sets the scroll it already has on every update.
        if (moved) callScriptOnTable(L, 1, "OnVerticalScroll", clamped);
    }
    return 0;
}

int lua_ScrollFrame_SetHorizontalScroll(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    if (auto* w = tree->get(id)) {
        const float v = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        const float max = scrollRange(*tree, id, false);
        const float clamped = (v < 0.0f) ? 0.0f : (v > max ? max : v);
        const bool moved = (clamped != w->scrollX);
        w->scrollX = clamped;
        if (moved) callScriptOnTable(L, 1, "OnHorizontalScroll", clamped);
    }
    return 0;
}

int lua_ScrollFrame_GetVerticalScroll(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->scrollY : 0.0f);
    return 1;
}

int lua_ScrollFrame_GetHorizontalScroll(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->scrollX : 0.0f);
    return 1;
}

int lua_ScrollFrame_GetVerticalScrollRange(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    lua_pushnumber(L, (tree && id) ? scrollRange(*tree, id, true) : 0.0f);
    return 1;
}

int lua_ScrollFrame_GetHorizontalScrollRange(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    lua_pushnumber(L, (tree && id) ? scrollRange(*tree, id, false) : 0.0f);
    return 1;
}

/// SetChecked / GetChecked. A check button shows its checked art or none, and
/// the interface both sets this and reads it back to decide what a click meant.
int lua_CheckButton_SetChecked(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        // No argument means checked, as in WoW, where SetChecked() with
        // nothing is how a box is ticked.
        //
        // A number is read as a number, because WoW's widget API takes 0 and 1
        // here and seventy-seven places in this FrameXML write SetChecked(0).
        // lua_toboolean answers true for 0 - only nil and false are false in
        // Lua - so every one of those was setting the box rather than
        // clearing it, and nothing that reported its state by unchecking ever
        // turned off. BagSlotButton_UpdateChecked is one: it counts the open
        // container frames and passes 0 or 1 straight in, so every bag button
        // stayed lit whether or not its bag was open.
        w->checked = lua_isnone(L, 2)    ? true
                   : lua_isnumber(L, 2)  ? (lua_tonumber(L, 2) != 0)
                                         : (lua_toboolean(L, 2) != 0);
    }
    return 0;
}

/// GetChecked. 1 when ticked and nil when not, which is what 3.3.5 answers and
/// what this FrameXML is written against - the same 0/1 convention SetChecked
/// takes, and forty SetChecked(0) calls in these files say which convention
/// that is.
///
/// A boolean reads the same to every `if (box:GetChecked())`, which is most of
/// the hundred and seventy call sites and why this looked right. It differs at
/// the ones that hand the answer to something else: SetCVar("questPOI",
/// self:GetChecked()) stored "true" instead of "1", and QueryAuctionItems took
/// it as its isUsable and raised - false is not nil, so the unticked box
/// raised too, and the auction browse never sent a search either way.
int lua_CheckButton_GetChecked(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (w && w->checked) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// SetButtonState(state, lock) / GetButtonState. The interface holding a
/// button down itself: ActionButton_UpdateState pushes the button for an
/// ability that is toggled on, and no amount of moving the cursor should let
/// it back up.
int lua_Button_SetButtonState(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const std::string state = luaL_optstring(L, 2, "NORMAL");
    using F = wowee::ui::Widget::Forced;
    if      (state == "PUSHED")   w->forcedState = F::Pushed;
    else if (state == "DISABLED") w->forcedState = F::Disabled;
    else                          w->forcedState = F::Normal;
    return 0;
}

int lua_Button_GetButtonState(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    using F = wowee::ui::Widget::Forced;
    const F f = w ? w->forcedState : F::None;
    lua_pushstring(L, f == F::Pushed ? "PUSHED"
                    : f == F::Disabled ? "DISABLED" : "NORMAL");
    return 1;
}

int lua_Button_LockHighlight(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->highlightLocked = true;
    return 0;
}

int lua_Button_UnlockHighlight(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->highlightLocked = false;
    return 0;
}

int lua_Texture_SetButtonArt(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const std::string slot = luaL_optstring(L, 2, "");
    using wowee::ui::ButtonArt;
    if      (slot == "NormalTexture")          w->buttonArt = ButtonArt::Normal;
    else if (slot == "PushedTexture")          w->buttonArt = ButtonArt::Pushed;
    else if (slot == "HighlightTexture")       w->buttonArt = ButtonArt::Highlight;
    else if (slot == "DisabledTexture")        w->buttonArt = ButtonArt::Disabled;
    else if (slot == "CheckedTexture")         w->buttonArt = ButtonArt::Checked;
    else if (slot == "DisabledCheckedTexture") w->buttonArt = ButtonArt::DisabledChecked;
    return 0;
}

int lua_Frame_IsWheelEnabled(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    const auto* w = (tree && id != 0) ? tree->get(id) : nullptr;
    lua_pushboolean(L, w && w->wheelEnabled);
    return 1;
}

int lua_Frame_SetWheelEnabled(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    if (auto* w = tree->get(id)) w->wheelEnabled = lua_toboolean(L, 2) != 0;
    return 0;
}

// ── Scrolling message frames ───────────────────────────────────────────────
//
// Chat. AddMessage was a name in the method list and nothing else, so every
// line the interface was handed went nowhere: the frame received the events,
// formatted the text, and dropped it.
int lua_MessageFrame_AddMessage(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    wowee::ui::Widget::Message m;
    // The same escape a label gets. Chat and the error frame carry counted
    // strings too - "you may not do that for another %d |4second:seconds;" -
    // and a message frame never passes through SetText, so resolving it there
    // alone would have left every one of these lines showing the escape.
    m.text = wowee::ui::resolvePluralEscapes(luaL_optstring(L, 2, ""));
    // WoW's colours are optional and default to the frame's own.
    m.color[0] = static_cast<float>(luaL_optnumber(L, 3, w->color[0]));
    m.color[1] = static_cast<float>(luaL_optnumber(L, 4, w->color[1]));
    m.color[2] = static_cast<float>(luaL_optnumber(L, 5, w->color[2]));
    m.color[3] = 1.0f;
    // The three the chat history hangs off a line. FrameXML hands them over on
    // the way in and reads them back with GetMessageInfo when it moves a
    // conversation into a window of its own; dropping them here meant the copy
    // had nothing to copy.
    m.lineId   = luaL_optnumber(L, 6, 0.0);
    m.accessId = luaL_optnumber(L, 8, 0.0);
    // Kept apart by type, because the caller looks it up in a table on the way
    // back out and a number written down as text finds nothing there.
    if (lua_type(L, 9) == LUA_TNUMBER) {
        m.hasExtra = m.extraIsNumber = true;
        m.extraNumber = lua_tonumber(L, 9);
    } else if (lua_type(L, 9) == LUA_TSTRING) {
        m.hasExtra = true;
        m.extra = lua_tostring(L, 9);
    }
    w->isMessageFrame = true;
    // UIErrorsFrame asks for insertMode="TOP", which puts a new line above the
    // ones already there rather than below them.
    if (w->messagesInsertTop) w->messages.push_front(std::move(m));
    else                      w->messages.push_back(std::move(m));
    while (w->messages.size() > w->maxMessages) {
        if (w->messagesInsertTop) w->messages.pop_back();
        else                      w->messages.pop_front();
    }
    // A new line at the bottom means the view follows it, which is what a
    // chat frame does unless someone has scrolled up.
    if (w->messageScroll > 0) ++w->messageScroll;
    return 0;
}

/// GetMessageInfo(index[, accessID]) → text, accessID, lineID, extraData.
///
/// What FCF_OpenTemporaryWindow reads when it moves a conversation into its
/// own window. It walks GetNumMessages and copies each line across, and this
/// answered nothing - so `ChatTypeInfo[cType]` was indexed with a nil key,
/// came back nil, and the next line read a field off it. Opening a whisper in
/// its own window raised on the first message it tried to carry.
int lua_MessageFrame_GetMessageInfo(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!w || index < 1) return 0;
    // Indexed within the conversation when one is named, to match the count
    // GetNumMessages gave for it - the caller walks one and reads the other,
    // so they have to be counting the same lines.
    const wowee::ui::Widget::Message* found = nullptr;
    if (lua_isnumber(L, 3)) {
        const double id = lua_tonumber(L, 3);
        int seen = 0;
        for (const auto& msg : w->messages) {
            if (msg.accessId != id) continue;
            if (++seen == index) { found = &msg; break; }
        }
    } else if (index <= static_cast<int>(w->messages.size())) {
        found = &w->messages[static_cast<size_t>(index) - 1];
    }
    if (!found) return 0;
    const auto& m = *found;
    lua_pushstring(L, m.text.c_str());
    lua_pushnumber(L, m.accessId);
    lua_pushnumber(L, m.lineId);
    if (!m.hasExtra)          lua_pushnil(L);
    else if (m.extraIsNumber) lua_pushnumber(L, m.extraNumber);
    else                      lua_pushstring(L, m.extra.c_str());
    return 4;
}

/// RemoveMessagesByAccessID(accessID) - the other half of that move.
///
/// The lines are copied to the new window and then taken out of the old one.
/// Without this they stayed in both, so a conversation pulled out of the main
/// window appeared twice.
int lua_MessageFrame_RemoveMessagesByAccessID(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const double id = luaL_optnumber(L, 2, 0.0);
    for (auto it = w->messages.begin(); it != w->messages.end();) {
        if (it->accessId == id) it = w->messages.erase(it);
        else                    ++it;
    }
    return 0;
}

/// SetTimeVisible(seconds) - how long a line stays before it fades.
///
/// Zero, the default, means for good, which is what a chat frame wants.
/// UIErrorsFrame declares five and it was dropped, so every refusal the server
/// sent stayed on screen until a hundred and twenty-eight had piled up.
int lua_FontString_SetWordWrap(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (w) w->wordWrap = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_FontString_CanWordWrap(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, (w && w->wordWrap) ? 1 : 0);
    return 1;
}

int lua_FontString_SetNonSpaceWrap(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (w) w->nonSpaceWrap = lua_toboolean(L, 2) != 0;
    return 0;
}

/// __WoweeReportFrame(name) - say what a frame is doing, into the client log.
///
/// Not a WoW call. It exists because "the window did not open" is one symptom
/// with three causes that look identical from outside: the frame was never
/// built, it was built and never shown, or it was shown and laid out to
/// nothing. Only this side can tell them apart.
int lua_WoweeReportFrame(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const char* name = luaL_optstring(L, 1, "");
    if (!tree || !name || !*name) return 0;
    const auto* w = tree->findByName(name);
    if (!w) {
        LOG_WARNING("frame '", name, "' was never built");
        return 0;
    }
    LOG_WARNING("frame '", name, "' shown=", w->shown, " visible=", w->visible,
                " rect=", w->rectW, "x", w->rectH,
                " at (", w->left, ",", w->bottom, ") alpha=", w->alpha);
    return 0;
}

int lua_Cooldown_SetReverse(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (w) w->cooldownReverse = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Cooldown_SetDrawEdge(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (w) w->cooldownDrawEdge = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_MessageFrame_SetTimeVisible(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (w) w->messageDuration = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    return 0;
}

int lua_MessageFrame_GetTimeVisible(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->messageDuration : 0.0);
    return 1;
}

int lua_MessageFrame_SetFadeDuration(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (w) w->messageFadeDuration = static_cast<float>(luaL_optnumber(L, 2, 3.0));
    return 0;
}

int lua_MessageFrame_SetInsertMode(lua_State* L) {
    auto* w = widgetOf(L, 1);
    const char* mode = luaL_optstring(L, 2, "BOTTOM");
    if (w) w->messagesInsertTop = (mode && std::string(mode) == "TOP");
    return 0;
}

// ── Tooltips ───────────────────────────────────────────────────────────────
//
// AddLine was a name in the method list and nothing else, so every tooltip in
// the interface was empty: the frame was shown, positioned and sized, and had
// nothing in it.
int lua_Tooltip_AddLine(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    wowee::ui::Widget::TooltipLine line;
    line.left = luaL_optstring(L, 2, "");
    // A colour that is not a number means "the default one", not an error.
    //
    // mailframe.lua does `AddLine(ENCLOSED_MONEY, "", 1, 1, 1)` - an empty
    // string where the red goes. optnumber accepts a string it can convert and
    // "" is not one, so it raised: the mail item's tooltip died on every hover,
    // and after five the OnUpdate driving it was unhooked for the rest of the
    // session. Nothing said so on screen; a raise inside a handler is
    // swallowed.
    //
    // White rather than black, because that is what the line is meant to be -
    // SetMoneyFrameColor is called with "white" two lines further down.
    auto colour = [L](int arg) {
        return lua_isnumber(L, arg) ? static_cast<float>(lua_tonumber(L, arg))
                                    : 1.0f;
    };
    line.lc[0] = colour(3);
    line.lc[1] = colour(4);
    line.lc[2] = colour(5);
    line.lc[3] = 1.0f;
    line.rc[0] = line.rc[1] = line.rc[2] = line.rc[3] = 1.0f;
    // AddLine(text, r, g, b, wrapText). The sixth argument was read off the
    // call and dropped, and FrameXML passes it on every line of prose.
    line.wrap = lua_toboolean(L, 6) != 0;
    w->isTooltip = true;
    w->tooltipLines.push_back(std::move(line));
    return 0;
}

/// GameTooltip:SetFrameStack(showHidden) - what is under the cursor.
///
/// This is /framestack, and it is the answer to every "what is drawing that?"
/// that otherwise costs a screenshot, a guess, and a round trip. It was the
/// last thing Blizzard_DebugTools needed that this client did not answer, and
/// a missing method there is a hard error rather than an empty tooltip.
///
/// Two deliberate differences from the real client, both because this is a
/// diagnostic and being useful beats being faithful:
///
///   * textures and font strings are listed too, not only frames. A stray
///     *region* is exactly the kind of thing worth identifying, and the real
///     framestack cannot name one.
///   * each line carries the rect, so a widget in the wrong place or at the
///     wrong size says so without anything else being opened.
///
/// Ordered outermost first, so the last line is what the cursor is really on.
/// GameTooltip:AppendText(text) - add to the end of the tooltip's first line.
///
/// The title line, not the last one added: the real client appends to the line
/// the tooltip was named with, which is what its one caller in the interface
/// wants - the bag buttons put the key that opens each bag in brackets after
/// the bag's own name.
///
/// A no-op answered it, so the tooltip was correct and the key was missing,
/// which is the kind of absence nobody reports.
int lua_Tooltip_AppendText(lua_State* L) {
    auto* w = widgetOf(L, 1);
    const char* text = luaL_optstring(L, 2, "");
    if (!w || !text || !*text || w->tooltipLines.empty()) return 0;
    w->tooltipLines.front().left += text;
    return 0;
}

int lua_Tooltip_SetFrameStack(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!w || !tree) return 0;
    const bool showHidden = lua_toboolean(L, 2) != 0;

    // The same conversion the input path makes: pixels to interface units,
    // and y flipped, because the widget tree grows upward and ImGui does not.
    const auto& io = ImGui::GetIO();
    const float s = tree->uiScale();
    const float px = io.MousePos.x;
    const float py = io.DisplaySize.y - io.MousePos.y;
    const float x = (s > 0.0f) ? px / s : px;
    const float y = (s > 0.0f) ? py / s : py;

    struct Entry { const wowee::ui::Widget* w; };
    std::vector<Entry> under;
    for (size_t id = 1; id < tree->size(); ++id) {
        const auto* c = tree->get(static_cast<uint32_t>(id));
        if (!c || c->id == 0) continue;
        if (!showHidden && !c->visible) continue;
        if (c->rectW <= 0.0f || c->rectH <= 0.0f) continue;
        if (x < c->left || x > c->left + c->rectW) continue;
        if (y < c->bottom || y > c->bottom + c->rectH) continue;
        under.push_back({c});
    }
    std::sort(under.begin(), under.end(), [](const Entry& a, const Entry& b) {
        if (a.w->effStrata != b.w->effStrata)
            return static_cast<int>(a.w->effStrata) < static_cast<int>(b.w->effStrata);
        return a.w->effLevel < b.w->effLevel;
    });

    w->isTooltip = true;
    w->tooltipLines.clear();
    auto line = [&](std::string text, float r, float g, float b) {
        wowee::ui::Widget::TooltipLine tl;
        tl.left = std::move(text);
        tl.lc[0] = r; tl.lc[1] = g; tl.lc[2] = b; tl.lc[3] = 1.0f;
        tl.rc[0] = tl.rc[1] = tl.rc[2] = tl.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(tl));
    };
    auto num = [](float v) {
        std::string s = std::to_string(static_cast<int>(v + (v < 0 ? -0.5f : 0.5f)));
        return s;
    };

    line("Frame Stack  (" + num(x) + ", " + num(y) + ")", 1.0f, 0.82f, 0.0f);
    if (under.empty()) {
        line("nothing under the cursor", 0.6f, 0.6f, 0.6f);
        return 0;
    }
    for (const Entry& e : under) {
        const char* kind = e.w->kind == wowee::ui::WidgetKind::Texture    ? "texture"
                         : e.w->kind == wowee::ui::WidgetKind::FontString ? "label"
                                                                         : "frame";
        std::string text = (e.w->name.empty() ? std::string("(unnamed)") : e.w->name);
        text += "  " + std::string(kind);
        text += "  " + num(e.w->left) + "," + num(e.w->bottom);
        text += " " + num(e.w->rectW) + "x" + num(e.w->rectH);
        if (!e.w->visible) text += "  HIDDEN";
        // A region is dimmer than a frame, so the frames read as the structure
        // and the art hanging off them as detail.
        if (e.w->kind == wowee::ui::WidgetKind::Frame) line(text, 1.0f, 1.0f, 1.0f);
        else                                           line(text, 0.6f, 0.8f, 1.0f);
        if (e.w->kind == wowee::ui::WidgetKind::Texture && !e.w->texturePath.empty()) {
            line("    " + e.w->texturePath, 0.5f, 0.5f, 0.5f);
        }
    }
    return 0;
}

int lua_Tooltip_AddDoubleLine(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    wowee::ui::Widget::TooltipLine line;
    line.left  = luaL_optstring(L, 2, "");
    line.right = luaL_optstring(L, 3, "");
    // Six colours, same rule as AddLine: anything that is not a number means
    // the default. Six arguments is six chances for the interface to pass one
    // of them as something else, and the whole tooltip is lost to a raise.
    auto colour = [L](int arg) {
        return lua_isnumber(L, arg) ? static_cast<float>(lua_tonumber(L, arg))
                                    : 1.0f;
    };
    line.lc[0] = colour(4);
    line.lc[1] = colour(5);
    line.lc[2] = colour(6);
    line.lc[3] = 1.0f;
    line.rc[0] = colour(7);
    line.rc[1] = colour(8);
    line.rc[2] = colour(9);
    line.rc[3] = 1.0f;
    w->isTooltip = true;
    w->tooltipLines.push_back(std::move(line));
    return 0;
}

/// SetOwner(frame, anchor) - where the tooltip goes, relative to what it is
/// describing. Every tooltip in the interface calls this before filling
/// itself, and it did nothing, so a tooltip with lines in it would still have
/// appeared wherever its XML left it rather than beside the button.
///
/// WoW's anchor names say which side of the owner the tooltip sits on; the
/// pair of points that produces is the whole of the mapping.
int lua_Tooltip_SetOwner(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    auto* w = tree->get(id);
    if (!w) return 0;
    w->isTooltip = true;
    // Cleared here, because SetOwner is what precedes a fresh set of lines.
    w->tooltipLines.clear();

    // Remembered, because IsOwned reads it and nothing wrote it - so every
    // check answered false and a tooltip outlived the frame it belonged to.
    // FrameXML hides a tooltip on OnLeave only when it owns it, which is what
    // stops one panel's tooltip being cleared by another's cursor.
    lua_pushvalue(L, 2);
    lua_setfield(L, 1, "__owner");

    const uint32_t owner = lua_istable(L, 2) ? widgetIdOf(L, 2) : 0;
    // So the tooltip can be taken away with the frame it describes, whether or
    // not that frame ever gets the OnLeave that would have done it.
    tree->setTooltipOwner(id, owner);
    const std::string anchor = luaL_optstring(L, 3, "ANCHOR_RIGHT");
    // ANCHOR_PRESERVE is the one that means what it says: keep the anchors.
    if (anchor == "ANCHOR_PRESERVE") return 0;
    if (owner == 0 || anchor == "ANCHOR_NONE") {
        // ANCHOR_NONE means the caller places the tooltip itself, and the call
        // that follows is a SetPoint - which is how GameTooltip_SetDefaultAnchor
        // pins an action button's tooltip to the bottom-right of the screen.
        // Returning without clearing left the anchor from the previous owner in
        // place, and SetPoint only replaces an anchor on the same point: a LEFT
        // anchor to the last button and a BOTTOMRIGHT anchor to UIParent both
        // applied, pulling the tooltip down onto the action bar it came from.
        tree->clearPoints(id);
        return 0;
    }

    struct Pair { const char* name; const char* point; const char* rel; };
    static const Pair kAnchors[] = {
        {"ANCHOR_TOPLEFT",     "BOTTOMLEFT",  "TOPLEFT"},
        {"ANCHOR_TOPRIGHT",    "BOTTOMRIGHT", "TOPRIGHT"},
        {"ANCHOR_BOTTOMLEFT",  "TOPLEFT",     "BOTTOMLEFT"},
        {"ANCHOR_BOTTOMRIGHT", "TOPRIGHT",    "BOTTOMRIGHT"},
        {"ANCHOR_LEFT",        "RIGHT",       "LEFT"},
        {"ANCHOR_RIGHT",       "LEFT",        "RIGHT"},
        {"ANCHOR_TOP",         "BOTTOM",      "TOP"},
        {"ANCHOR_BOTTOM",      "TOP",         "BOTTOM"},
        // The cursor is not a frame, so this lands beside the owner instead -
        // which is where the cursor is, near enough, and better than nowhere.
        {"ANCHOR_CURSOR",      "TOPLEFT",     "BOTTOMRIGHT"},
    };
    const char* point = "LEFT";
    const char* rel = "RIGHT";
    for (const Pair& p : kAnchors) {
        if (anchor == p.name) { point = p.point; rel = p.rel; break; }
    }

    wowee::ui::Anchor a;
    a.point = point;
    a.relativeTo = owner;
    a.relativePoint = rel;
    tree->clearPoints(id);
    tree->addPoint(id, a);
    return 0;
}

/// The one-line form. GameTooltip:SetText replaces what the tooltip says
/// rather than setting a font string, and returns whether it did - so the
/// shared SetText can hand off and stop.
int lua_Tooltip_SetText(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w || !w->isTooltip) { lua_pushboolean(L, 0); return 1; }
    w->tooltipLines.clear();
    wowee::ui::Widget::TooltipLine line;
    line.left = luaL_optstring(L, 2, "");
    line.lc[0] = static_cast<float>(luaL_optnumber(L, 3, 1.0));
    line.lc[1] = static_cast<float>(luaL_optnumber(L, 4, 1.0));
    line.lc[2] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    line.lc[3] = 1.0f;
    line.rc[0] = line.rc[1] = line.rc[2] = line.rc[3] = 1.0f;
    // SetText(text, r, g, b, a, textWrap) - the seventh here, since alpha sits
    // between the colour and the flag.
    line.wrap = lua_toboolean(L, 7) != 0;
    w->tooltipLines.push_back(std::move(line));
    w->shown = true;
    lua_pushboolean(L, 1);
    return 1;
}

/// Fills a tooltip from an action bar slot: the spell's or item's name and
/// what it does. ActionButton_SetTooltip asks for this and checks the answer -
/// `if (GameTooltip:SetAction(self.action))` - so returning nothing meant
/// every action button fell to its "no tooltip" branch.
static bool fillItemTooltipById(wowee::ui::Widget* w, game::GameHandler* gh,
                                uint32_t itemId);
static bool fillItemTooltipById(lua_State* L, game::GameHandler* gh,
                                uint32_t itemId);

/// One spell tooltip, for every path that shows one.
///
/// The action bar and the spellbook each built their own and both stopped at
/// the name and the description - so a spell never said what it costs, how far
/// it reaches or how long it takes to cast, all of which the client already
/// resolves for its own use. Two copies also meant either could be improved
/// alone and quietly drift from the other, which is exactly what happened to
/// the item tooltips.
///
/// Laid out as WoW lays it out: cost on the left of its line with the range on
/// the right, then the cast time, then the description.
/// A quest link, filled from the log.
///
/// This used to answer false for every quest, on the grounds that the client
/// kept nothing beyond the title and a tooltip holding only a name is worse
/// than the caller knowing it failed. The first half stopped being true: a log
/// entry now carries the quest giver's own text and the objective list, both
/// read from the query response.
///
/// Only for a quest in the player's own log, which is the honest limit - a
/// link to a quest they have never taken names an id nothing here has text
/// for, and answering false there is still the right answer. The caller reads
/// it: `if ( GameTooltip:SetHyperlink(link) )` decides whether to show the
/// tooltip at all.
static bool fillQuestTooltip(wowee::ui::Widget* w, game::GameHandler* gh,
                             uint32_t questId) {
    if (!w || !gh || questId == 0) return false;
    // Through the public log rather than the private finder beside it: this
    // wants the same entry the quest log itself draws from.
    const game::GameHandler::QuestLogEntry* q = nullptr;
    for (const auto& e : gh->getQuestLog()) {
        if (e.questId == questId) { q = &e; break; }
    }
    if (!q) return false;
    if (q->title.empty() && q->objectives.empty()) return false;

    w->isTooltip = true;
    w->tooltipLines.clear();
    auto line = [&w](std::string text, float r, float g, float b) {
        wowee::ui::Widget::TooltipLine t;
        t.left = std::move(text);
        t.lc[0] = r; t.lc[1] = g; t.lc[2] = b; t.lc[3] = 1.0f;
        t.rc[0] = t.rc[1] = t.rc[2] = 1.0f; t.rc[3] = 1.0f;
        // Prose, so it breaks to fit rather than making the tooltip as wide
        // as the sentence.
        t.wrap = true;
        w->tooltipLines.push_back(std::move(t));
    };
    line(q->title.empty() ? ("Quest #" + std::to_string(questId)) : q->title,
         1.0f, 0.82f, 0.0f);
    if (q->level > 0) {
        line("Level " + std::to_string(q->level), 1.0f, 1.0f, 1.0f);
    }
    if (!q->description.empty()) line(q->description, 1.0f, 1.0f, 1.0f);
    // What is left to do, or what to do now it is done - the same two the log
    // shows, and the same order.
    if (q->complete && !q->completionText.empty()) {
        line(q->completionText, 1.0f, 1.0f, 1.0f);
    } else if (!q->objectives.empty()) {
        line(q->objectives, 0.75f, 0.75f, 0.75f);
    }
    return true;
}

static bool fillSpellTooltip(wowee::ui::Widget* w, game::GameHandler* gh,
                             uint32_t spellId) {
    if (!w || !gh || spellId == 0) return false;
    const std::string& name = gh->getSpellName(spellId);
    if (name.empty()) return false;

    auto line = [&w](std::string l, std::string r, float lr, float lg, float lb) {
        wowee::ui::Widget::TooltipLine t;
        t.left = std::move(l);
        t.right = std::move(r);
        t.lc[0] = lr; t.lc[1] = lg; t.lc[2] = lb; t.lc[3] = 1.0f;
        t.rc[0] = t.rc[1] = t.rc[2] = 1.0f; t.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(t));
    };

    w->isTooltip = true;
    w->tooltipLines.clear();
    line(name, "", 1.0f, 0.82f, 0.0f);   // gold, as WoW titles a tooltip

    const auto info = gh->getSpellData(spellId);
    std::string cost, range;
    if (info.manaCost > 0) {
        const char* named = game::powerTypeName(info.powerType);
        const char* unit = named ? named : "";
        cost = std::to_string(info.manaCost) + (*unit ? std::string(" ") + unit : "");
    }
    if (info.maxRange > 0.0f) {
        range = std::to_string(static_cast<int>(info.maxRange)) + " yd range";
    }
    if (!cost.empty() || !range.empty()) line(cost, range, 1.0f, 1.0f, 1.0f);

    // Instant is a word rather than a zero, which is what WoW prints.
    const std::string cast = info.castTimeMs > 0
        ? std::to_string(info.castTimeMs / 1000.0f).substr(0, 4) + " sec cast"
        : std::string("Instant");
    line(cast, "", 1.0f, 1.0f, 1.0f);

    const std::string body =
        gh->formatSpellDescription(spellId, gh->getSpellDescription(spellId));
    if (!body.empty()) line(body, "", 1.0f, 1.0f, 1.0f);

    w->shown = true;
    return true;
}

int lua_Tooltip_SetAction(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0)) - 1;
    if (!w || !gh || slot < 0) { lua_pushboolean(L, 0); return 1; }
    const auto& bar = gh->getActionBar();
    if (slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    const auto& action = bar[slot];
    // An item on the bar gets the tooltip the bags give it, stats and all.
    // Naming it and stopping meant the same potion described itself two
    // different ways depending on where it was hovered.
    if (action.type == game::ActionBarSlot::ITEM) {
        lua_pushboolean(L, fillItemTooltipById(L, gh, action.id) ? 1 : 0);
        return 1;
    }
    if (action.type == game::ActionBarSlot::SPELL) {
        lua_pushboolean(L, fillSpellTooltip(w, gh, action.id) ? 1 : 0);
        return 1;
    }
    // A macro is the third kind a slot can hold and it has no tooltip of its
    // own - WoW shows the macro's name from the button rather than from here.
    lua_pushboolean(L, 0);
    return 1;
}

/// The same for a spell asked for by id, which is how the spellbook and the
/// stance bar fill theirs.
/// SetHyperlink(link) - fill the tooltip from a link, as a chat link or an
/// item reference carries it.
///
/// There was a Lua version of this on the frame metatable and it worked; what
/// it did not do was answer. Nine callers are written as
///
///     if ( GameTooltip:SetHyperlink("spell:"..self.spellID) ) then
///
/// or `return self:SetHyperlink(link)`, and it returned nothing, so every one
/// of them read nil. CompanionButton_OnEnter takes that as failure and clears
/// its own UpdateTooltip, so a mount's tooltip was drawn once and then never
/// refreshed; the eight bootstrap tooltip methods that route through a link -
/// loot, loot roll, merchant, buyback, mail, quest reward, quest log, trade -
/// each hand the same nil back to whatever asked them.
///
/// Here rather than in Lua so that an item link fills from the same code every
/// other item tooltip uses. Two builders for one tooltip is how the two drift,
/// which is what happened to the item tooltips once already.
///
/// A link is "kind:id:more:fields", and only the kind and the first number are
/// needed to fill one. A whole "|cff...|Hitem:1234:...|h[Name]|h|r" is accepted
/// too, because that is the shape a link pulled out of chat arrives in.
int lua_Tooltip_SetHyperlink(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    std::string link = luaL_optstring(L, 2, "");
    if (!w || !gh || link.empty()) { lua_pushboolean(L, 0); return 1; }

    // Take what is between |H and |h if the wrapper is there.
    if (const size_t at = link.find("|H"); at != std::string::npos) {
        link = link.substr(at + 2);
        if (const size_t end = link.find("|h"); end != std::string::npos)
            link = link.substr(0, end);
    }

    const size_t colon = link.find(':');
    if (colon == std::string::npos) { lua_pushboolean(L, 0); return 1; }
    const std::string kind = link.substr(0, colon);
    uint32_t id = 0;
    for (size_t i = colon + 1; i < link.size() && link[i] >= '0' && link[i] <= '9'; ++i)
        id = id * 10 + static_cast<uint32_t>(link[i] - '0');
    if (id == 0) { lua_pushboolean(L, 0); return 1; }

    bool filled = false;
    if (kind == "spell" || kind == "enchant" || kind == "talent")
        filled = fillSpellTooltip(w, gh, id);
    else if (kind == "item")
        filled = fillItemTooltipById(L, gh, id);
    else if (kind == "quest")
        filled = fillQuestTooltip(w, gh, id);
    lua_pushboolean(L, filled ? 1 : 0);
    return 1;
}

int lua_Tooltip_SetSpellByID(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const uint32_t id = static_cast<uint32_t>(luaL_optnumber(L, 2, 0));
    lua_pushboolean(L, fillSpellTooltip(w, gh, id) ? 1 : 0);
    return 1;
}

/// A glyph in its socket, hovered on the glyph panel.
///
/// SetGlyph(socketID, talentGroup) - the same one-based socket and spec
/// GetGlyphSocketInfo takes. A glyph *is* a spell once its properties id is
/// resolved, so this is the spell tooltip; the resolution is the one that
/// binding now does, off GlyphProperties.dbc.
int lua_Tooltip_SetGlyph(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int socket = static_cast<int>(luaL_optnumber(L, 2, 0));
    const int spec = static_cast<int>(luaL_optnumber(L, 3, 0));
    if (!w || !gh || socket < 1 || socket > game::GameHandler::MAX_GLYPH_SLOTS) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const auto& glyphs = (spec >= 1 && spec <= 2)
        ? gh->getGlyphs(static_cast<uint8_t>(spec - 1)) : gh->getGlyphs();
    const uint16_t glyphId = glyphs[static_cast<size_t>(socket) - 1];
    if (glyphId == 0) { lua_pushboolean(L, 0); return 1; }
    gh->ensureGlyphPropertiesLoaded();
    const uint32_t spellId = gh->getGlyphSpellId(glyphId);
    lua_pushboolean(L, spellId && fillSpellTooltip(w, gh, spellId) ? 1 : 0);
    return 1;
}

/// One item from a finished dungeon's reward list, hovered on the alert.
///
/// The alert frame passes the same one-based index it gave
/// GetLFGCompletionRewardItem to draw the icon, so the lookup is the one that
/// binding already does - SMSG_LFG_PLAYER_REWARD carries the item ids and this
/// client has been parsing them. Without it the icon had an empty box over it.
int lua_Tooltip_SetLFGCompletionReward(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!w || !gh) { lua_pushboolean(L, 0); return 1; }
    const auto& reward = gh->getLfgCompletionReward();
    if (!reward.valid || index < 1 ||
        index > static_cast<int>(reward.items.size())) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const uint32_t itemId = reward.items[static_cast<size_t>(index) - 1].itemId;
    lua_pushboolean(L, itemId && fillItemTooltipById(w, gh, itemId) ? 1 : 0);
    return 1;
}

/// An equipment set, hovered on the character sheet's gear manager.
///
/// The set name and what it holds. The item guids were parsed and exposed all
/// along - GetEquipmentSetItemIDs reads the same two accessors - so the only
/// thing missing was the tooltip that describes them, which the no-op fallback
/// left as an empty box over every set button.
///
/// A slot the set was told to leave alone is written as a guid of one, which is
/// what the ignore mask records; those are skipped rather than listed as an
/// item nobody can name. An item the player no longer holds resolves to no id
/// and is called out in red, which is the whole reason to hover a set.
int lua_Tooltip_SetEquipmentSet(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const char* wanted = luaL_optstring(L, 2, "");
    if (!w || !gh || !wanted || !*wanted) { lua_pushboolean(L, 0); return 1; }

    const game::EquipmentSetInfo* set = nullptr;
    for (const auto& es : gh->getEquipmentSets())
        if (es.name == wanted) { set = &es; break; }
    if (!set) { lua_pushboolean(L, 0); return 1; }

    w->isTooltip = true;
    w->tooltipLines.clear();
    auto line = [&](std::string text, float r, float g, float b) {
        wowee::ui::Widget::TooltipLine tl;
        tl.left = std::move(text);
        tl.lc[0] = r; tl.lc[1] = g; tl.lc[2] = b; tl.lc[3] = 1.0f;
        tl.rc[0] = tl.rc[1] = tl.rc[2] = tl.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(tl));
    };
    line(set->name, 1.0f, 1.0f, 1.0f);

    const auto* guids = gh->getEquipmentSetItems(set->setId);
    const uint32_t ignored = gh->getEquipmentSetIgnoreMask(set->setId);
    if (guids) {
        for (int slot = 0; slot < 19; ++slot) {
            if (ignored & (1u << slot)) continue;
            const uint64_t guid = (*guids)[static_cast<size_t>(slot)];
            if (guid == 0) continue;
            const uint32_t itemId = gh->getItemIdByGuid(guid);
            const auto* info = itemId ? gh->getItemInfo(itemId) : nullptr;
            if (info && info->valid && !info->name.empty()) {
                line(info->name, 1.0f, 1.0f, 1.0f);
            } else {
                line("Missing item", 1.0f, 0.1f, 0.1f);
            }
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

/// The spell a quest hands over, hovered in the quest log.
///
/// It was on the no-op list, which was the right answer while
/// GetQuestLogRewardSpell returned nil - questinfo.lua gates the whole reward
/// row on that, so there was no icon to hover. With the row drawn the tooltip
/// is the next thing anyone reaches for, and an empty box over a reward is the
/// shape a no-op leaves behind.
int lua_Tooltip_SetQuestLogRewardSpell(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    uint32_t spellId = 0;
    if (gh) {
        // Through the row mapper: the selected index is a display index into a
        // log grouped under zone headers, not a position in getQuestLog().
        if (const auto* q = wowee::addons::questAtLogRow(gh, gh->getSelectedQuestLogIndex()))
            spellId = q->rewardSpellId;
    }
    lua_pushboolean(L, spellId && fillSpellTooltip(w, gh, spellId) ? 1 : 0);
    return 1;
}

/// A talent's name, the rank the player has in it, and what it does.
///
/// Hovering a talent used to raise: the metatable had no SetTalent, so the call
/// landed on nil and took the handler with it. Listing it as a no-op stopped
/// that and left the tooltip empty; this fills it, which is the whole reason
/// anyone hovers a talent.
///
/// The description shown is the rank the player actually has. An unlearned
/// talent describes its first rank, which is what taking a point in it would
/// buy - the useful thing to read when deciding.
int lua_Tooltip_SetTalent(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int tabIndex    = static_cast<int>(luaL_optnumber(L, 2, 0));
    const int talentIndex = static_cast<int>(luaL_optnumber(L, 3, 0));
    if (!w || !gh) { lua_pushboolean(L, 0); return 1; }

    const auto* talent = wowee::addons::talentAt(gh, tabIndex, talentIndex);
    if (!talent) { lua_pushboolean(L, 0); return 1; }

    const uint8_t rank = gh->getTalentRank(talent->talentId);
    // Rank 1's spell names the talent whatever the player has in it, and is the
    // only entry guaranteed to be filled.
    const uint32_t titleSpell = talent->rankSpells[0];
    // The rank being described: what the player has, or the first if none.
    const uint32_t bodySpell = talent->rankSpells[rank > 0 ? rank - 1 : 0];
    if (titleSpell == 0) { lua_pushboolean(L, 0); return 1; }

    const std::string& name = gh->getSpellName(titleSpell);
    if (name.empty()) { lua_pushboolean(L, 0); return 1; }

    w->isTooltip = true;
    w->tooltipLines.clear();

    wowee::ui::Widget::TooltipLine title;
    title.left = name;
    title.lc[0] = 1.0f; title.lc[1] = 0.82f; title.lc[2] = 0.0f; title.lc[3] = 1.0f;
    title.rc[0] = title.rc[1] = title.rc[2] = title.rc[3] = 1.0f;
    w->tooltipLines.push_back(std::move(title));

    if (talent->maxRank > 0) {
        wowee::ui::Widget::TooltipLine rankLine;
        rankLine.left = "Rank " + std::to_string(rank) + "/" +
                        std::to_string(static_cast<int>(talent->maxRank));
        rankLine.lc[0] = rankLine.lc[1] = rankLine.lc[2] = 1.0f; rankLine.lc[3] = 1.0f;
        rankLine.rc[0] = rankLine.rc[1] = rankLine.rc[2] = rankLine.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(rankLine));
    }

    // Through the formatter for the same reason SetSpellByID is: a description
    // arrives as a template of $-tokens and reads as one if handed over raw.
    const std::string body =
        gh->formatSpellDescription(bodySpell, gh->getSpellDescription(bodySpell));
    if (!body.empty()) {
        wowee::ui::Widget::TooltipLine desc;
        desc.left = body;
        desc.lc[0] = desc.lc[1] = desc.lc[2] = 1.0f; desc.lc[3] = 1.0f;
        desc.rc[0] = desc.rc[1] = desc.rc[2] = desc.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(desc));
    }

    w->shown = true;
    lua_pushboolean(L, 1);
    return 1;
}

/// SetTradeSkillItem(index) - what a recipe in the open profession makes.
///
/// This client knows a recipe by its crafting spell rather than by the item it
/// produces, so what is shown is that spell: its name and its description,
/// which is the line that says what gets made. Not the crafted item's own
/// tooltip, which is what the real client shows - but the useful half of it,
/// and it is what this client actually knows.
int lua_Tooltip_SetTradeSkillItem(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!w || !gh || index < 1) { lua_pushboolean(L, 0); return 1; }

    const auto recipes = gh->getCraftingRecipes();
    if (index > static_cast<int>(recipes.size())) { lua_pushboolean(L, 0); return 1; }
    const uint32_t spellId = recipes[index - 1].spellId;
    if (spellId == 0) { lua_pushboolean(L, 0); return 1; }

    w->isTooltip = true;
    w->tooltipLines.clear();
    wowee::ui::Widget::TooltipLine title;
    title.left = recipes[index - 1].name;
    title.lc[0] = 1.0f; title.lc[1] = 0.82f; title.lc[2] = 0.0f; title.lc[3] = 1.0f;
    title.rc[0] = title.rc[1] = title.rc[2] = title.rc[3] = 1.0f;
    w->tooltipLines.push_back(std::move(title));

    const std::string body =
        gh->formatSpellDescription(spellId, gh->getSpellDescription(spellId));
    if (!body.empty()) {
        wowee::ui::Widget::TooltipLine desc;
        desc.left = body;
        desc.lc[0] = desc.lc[1] = desc.lc[2] = 1.0f; desc.lc[3] = 1.0f;
        desc.rc[0] = desc.rc[1] = desc.rc[2] = desc.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(desc));
    }
    w->shown = true;
    lua_pushboolean(L, 1);
    return 1;
}

/// A unit's name and level, which is what hovering a unit frame shows.
/// IsUnit(token) - whether this tooltip is describing that unit.
///
/// One caller, and it is the reason OnTooltipSetUnit exists: gametooltip.xml
/// opens that handler with `if ( self:IsUnit("mouseover") )` before colouring
/// the name by reaction. Answered with a no-op, that test is false for every
/// tooltip, so firing the handler - which nothing did until yesterday -
/// changed nothing on its own.
///
/// Compared by guid rather than by token, which is the whole point of the
/// question: a tooltip set for "target" is describing "mouseover" whenever
/// they are the same unit, and the caller is asking exactly that.
static int lua_Tooltip_IsUnit(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const char* asked = luaL_optstring(L, 2, nullptr);
    if (!w || !gh || !asked || !*asked || w->tooltipUnit.empty()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    std::string other(asked);
    for (char& c : other) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (other == w->tooltipUnit) { lua_pushboolean(L, 1); return 1; }
    const uint64_t mine = wowee::addons::resolveUnitGuid(gh, w->tooltipUnit);
    const uint64_t theirs = wowee::addons::resolveUnitGuid(gh, other);
    lua_pushboolean(L, (mine != 0 && mine == theirs) ? 1 : 0);
    return 1;
}

int lua_Tooltip_SetUnit(lua_State* L) {
    auto* w = widgetOf(L, 1);
    // A model frame has a SetUnit of its own - it is how the paperdoll loads
    // the player - and that name collides with the tooltip's. Answering as the
    // tooltip made CharacterModelFrame a tooltip carrying the player's name,
    // which then drew across the rotate arrows and sized the frame to fit one
    // line of text instead of the figure.
    if (w && w->objectType != "GameTooltip") {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto* gh = wowee::addons::getGameHandler(L);
    const char* uid = luaL_optstring(L, 2, "player");
    if (!w || !gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const uint64_t guid = wowee::addons::resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushboolean(L, 0); return 1; }
    const std::string name = gh->lookupName(guid);
    if (name.empty()) { lua_pushboolean(L, 0); return 1; }

    w->isTooltip = true;
    // Whose tooltip this is, for IsUnit below.
    w->tooltipUnit = uidStr;
    w->tooltipLines.clear();

    auto addLine = [&w](std::string text, float r, float g, float b) {
        if (text.empty()) return;
        wowee::ui::Widget::TooltipLine line;
        line.left = std::move(text);
        line.lc[0] = r; line.lc[1] = g; line.lc[2] = b; line.lc[3] = 1.0f;
        line.rc[0] = line.rc[1] = line.rc[2] = line.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(line));
    };

    addLine(name, 1.0f, 1.0f, 1.0f);

    // What the name alone does not say, which in WoW is most of a unit
    // tooltip: the guild under a player's name, the trade under an NPC's, and
    // the level line under both.
    //
    // This built the title and stopped, so every mouseover in the game showed
    // a bare name. The client has all of it - its own nameplates draw the
    // guild and the subtitle from these very accessors - so this is another
    // case of handing an element over leaving behind what the window it
    // replaced was already doing.
    const auto entity = gh->getEntityManager().getEntity(guid);
    const bool isPlayer = entity && entity->getType() == game::ObjectType::PLAYER;

    if (isPlayer) {
        if (const uint32_t guildId = gh->getEntityGuildId(guid); guildId != 0) {
            const std::string& guildName = gh->lookupGuildName(guildId);
            if (!guildName.empty()) addLine("<" + guildName + ">", 0.25f, 1.0f, 0.25f);
        }
    } else if (entity) {
        // The trade an NPC plies, which is the line that tells a vendor from a
        // guard: <Auctioneer>, <Innkeeper>, <Reagent Vendor>.
        const auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
        if (unit) {
            const std::string sub = gh->getCachedCreatureSubName(unit->getEntry());
            if (!sub.empty()) addLine("<" + sub + ">", 1.0f, 1.0f, 1.0f);
        }
    }

    // "Level 50", and the class after it for a player. Yellow, as WoW draws a
    // level the player's own is a match for; the difficulty colouring proper
    // needs the same table the nameplates use and is not invented here.
    if (entity) {
        const uint32_t level =
            entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL));
        if (level > 0) {
            std::string levelLine = "Level " + std::to_string(level);
            if (isPlayer) {
                const uint32_t bytes0 =
                    entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                const unsigned classId = (bytes0 >> 8) & 0xFFu;
                if (const char* cls = wowee::addons::luaClassToken(classId)) {
                    (void)cls;
                    levelLine += " ";
                    levelLine += wowee::addons::kLuaClasses[classId];
                }
            }
            addLine(levelLine, 1.0f, 0.82f, 0.0f);
        }
    }

    w->shown = true;
    // The unit counterpart of fireTooltipSetItem, and it was the one of the
    // pair that was never fired. GameTooltip declares
    //
    //     <OnTooltipSetUnit>
    //         if ( self:IsUnit("mouseover") ) then
    //             _G[self:GetName().."TextLeft1"]:SetTextColor(
    //                 GameTooltip_UnitColor("mouseover"));
    //
    // so the name at the top of a unit tooltip takes its colour from that
    // unit's reaction - red for hostile, green for friendly - and without this
    // every one of them was drawn in the default colour instead.
    if (lua_istable(L, 1)) callScriptOnTable(L, 1, "OnTooltipSetUnit", 0.0);
    lua_pushboolean(L, 1);
    return 1;
}

/// The same tooltip for an item known only by its id.
///
/// Bags and the paperdoll hold a whole ItemDef; an auction row holds an entry
/// number and nothing else. Rather than a second tooltip builder for that case,
/// this fills in what the item cache knows and hands it to the one above - so
/// there is one quality colour table and one idea of what an item tooltip looks
/// like. False when the cache has not heard of the item yet, which is the same
/// answer an empty bag slot gives.
static bool fillItemTooltipById(wowee::ui::Widget* w, game::GameHandler* gh,
                                uint32_t itemId) {
    if (!w || !gh || itemId == 0) return false;
    const auto* info = gh->getItemInfo(itemId);
    if (!info || info->name.empty()) return false;
    game::ItemDef def;
    def.itemId  = itemId;
    def.name    = info->name;
    def.quality = static_cast<game::ItemQuality>(info->quality);
    def.subclassName = info->subclassName;
    fillItemTooltip(w, def, gh);
    return true;
}

/// The same, through the bootstrap's builder - which is the fuller of the two.
///
/// There were two item tooltips. The bootstrap Lua defines
/// _WoweePopulateItemTooltip and puts SetBagItem and SetAction on the frame
/// metatable after the C bindings are registered onto it, so a bag item went
/// through the Lua one while the guild bank, the currency tokens, the auction
/// rows and the quest log's special item went through the C one - and the two
/// do not say the same things. The Lua builder adds the item level, the
/// equip-slot and subclass line, the sell price, the heroic tag and the whole
/// table of rating names; the C builder has none of those. So the same sword
/// described itself two ways depending on where it was hovered, which is the
/// fault the SetAction comment above believed it had fixed.
///
/// Consolidated towards the fuller one rather than away from it. Every C setter
/// tries this first and keeps the C builder as its fallback, for an item the
/// interface has no GetItemInfo for yet - the Lua builder answers false there,
/// and false must not mean "no tooltip" when the client knows the name.
///
/// isTooltip is set before the call because SetText refuses a frame that is not
/// one, and the builder opens with SetText.
static bool fillItemTooltipById(lua_State* L, game::GameHandler* gh,
                                uint32_t itemId) {
    auto* w = widgetOf(L, 1);
    if (!w || !gh || itemId == 0) return false;
    const int top = lua_gettop(L);
    lua_getglobal(L, "_WoweePopulateItemTooltip");
    if (lua_isfunction(L, -1)) {
        w->isTooltip = true;
        w->tooltipLines.clear();
        lua_pushvalue(L, 1);                                  // self
        lua_pushnumber(L, static_cast<lua_Number>(itemId));
        if (lua_pcall(L, 2, 1, 0) == 0) {
            const bool ok = lua_toboolean(L, -1) != 0;
            lua_settop(L, top);
            if (ok) { w->shown = true; fireTooltipSetItem(L); return true; }
            // Nothing written, so leave the frame as the fallback finds it.
            w->tooltipLines.clear();
        } else {
            lua_settop(L, top);
        }
    } else {
        lua_settop(L, top);
    }
    return fillItemTooltipById(w, gh, itemId);
}

/// SetAuctionItem(list, index) - the item on an auction row.
///
/// The three lists are the browse results, the player's own auctions and the
/// ones they have bid on, named as GetAuctionItemInfo names them.
int lua_Tooltip_SetAuctionItem(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const char* list = luaL_optstring(L, 2, "list");
    const int index = static_cast<int>(luaL_optnumber(L, 3, 0));
    if (!w || !gh || index < 1) { lua_pushboolean(L, 0); return 1; }

    const std::string which(list ? list : "list");
    const auto& results = (which == "owner")  ? gh->getAuctionOwnerResults()
                        : (which == "bidder") ? gh->getAuctionBidderResults()
                                              : gh->getAuctionBrowseResults();
    if (index > static_cast<int>(results.auctions.size())) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const bool filled =
        fillItemTooltipById(L, gh, results.auctions[index - 1].itemEntry);
    lua_pushboolean(L, filled ? 1 : 0);
    return 1;
}

/// The temporary enchantment on one equipped item, as its own line.
///
/// A sharpening stone, an oil, a poison or a shaman's imbue live in the item's
/// *temporary* enchantment slot rather than in any aura, so nothing that reads
/// the player's buffs can see one. FrameXML's TemporaryEnchantFrame shows the
/// weapon's own icon for it and asks GameTooltip:SetInventoryItem for the text
/// - so without this the button came up describing the weapon's built-in
/// chance-on-hit and said nothing about the stone that put it there, which is
/// exactly how it was reported.
///
/// Green, and after the item's own lines, which is where the real client puts
/// it. Nothing is added when there is no temporary enchant, so a plain weapon
/// reads as it did.
static void appendEnchantLines(wowee::ui::Widget* w, game::GameHandler* gh,
                               uint64_t itemGuid) {
    if (!w || !gh || itemGuid == 0) return;
    const auto [permanentId, temporaryId] = gh->getItemEnchantIds(itemGuid);

    // Green, and after the item's own lines, which is where the real client
    // puts both of them. The permanent one was not shown at all: an enchanted
    // weapon described its base damage and said nothing about the enchant on
    // it, in the bags and on the paperdoll alike.
    const auto addLine = [&](uint32_t enchantId) {
        if (enchantId == 0) return;
        std::string name = gh->getEnchantName(enchantId);
        if (name.empty()) return;
        wowee::ui::Widget::TooltipLine line;
        line.left = std::move(name);
        line.lc[0] = 0.0f; line.lc[1] = 1.0f; line.lc[2] = 0.0f; line.lc[3] = 1.0f;
        line.rc[0] = line.rc[1] = line.rc[2] = line.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(line));
    };
    addLine(permanentId);
    addLine(temporaryId);
}

/// The random suffix an instance rolled, on top of a tooltip built from the
/// item's template.
///
/// "Bracers of Arcane Protection" is one item to a player and a base item plus
/// an ItemRandomSuffix.dbc row here. Both bag and paperdoll tooltips are built
/// from an item *id* - _WoweePopulateItemTooltip calls GetItemInfo, which
/// answers for the template - so the name came out as "Bracers" and the stats
/// the suffix rolled did not appear at all. Neither is reachable from an id,
/// which is the same gap appendEnchantLines fills for enchants.
///
/// The stats are asked for again rather than read off the ItemDef. buildItemDef
/// folds them into the base figures, and those are not what the tooltip is
/// showing: it is showing the template's.
static void appendRandomSuffix(wowee::ui::Widget* w, game::GameHandler* gh,
                               const game::ItemDef& item) {
    if (!w || !gh || item.randomPropertyId == 0 || w->tooltipLines.empty()) return;

    const std::string suffix = gh->getRandomPropertyName(item.randomPropertyId);
    if (!suffix.empty()) {
        // Appended once. A tooltip is refreshed in place for as long as the
        // pointer stays on the slot, and only SetText clears the lines, so a
        // blind append would grow the name on every frame.
        std::string& title = w->tooltipLines.front().left;
        const bool already = title.size() >= suffix.size() &&
            title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (!already) title += " " + suffix;
    }

    // Green and after the item's own lines, where the real client puts them.
    for (const auto& b : gh->getRandomStatBonuses(item.randomPropertyId,
                                                  item.suffixFactor)) {
        if (b.value == 0) continue;
        const char* statName = game::itemStatName(b.statType);
        if (!statName) continue;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%+d %s", b.value, statName);
        wowee::ui::Widget::TooltipLine line;
        line.left = buf;
        line.lc[0] = 0.0f; line.lc[1] = 1.0f; line.lc[2] = 0.0f; line.lc[3] = 1.0f;
        line.rc[0] = line.rc[1] = line.rc[2] = line.rc[3] = 1.0f;
        w->tooltipLines.push_back(std::move(line));
    }
}

/// _WoweeAppendItemEnchants(self, bag, slot) - the enchants on a bag item.
///
/// The bag tooltip is built in Lua and had no way to reach an item's GUID,
/// which is the only thing the enchant is keyed by - so a bag item never
/// mentioned its enchant even though the paperdoll's did.
int lua_Tooltip_AppendItemEnchants(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    if (!w || !gh) return 0;
    const int bag = static_cast<int>(luaL_optnumber(L, 2, -1));
    const int slot = static_cast<int>(luaL_optnumber(L, 3, 0));
    if (slot < 1) return 0;
    // The suffix first, because it renames the line the template put at the
    // top and the enchants below are appended after it.
    if (const game::ItemSlot* s =
            wowee::addons::containerItemSlot(gh->getInventory(), bag, slot);
        s && !s->empty()) {
        appendRandomSuffix(w, gh, s->item);
    }
    appendEnchantLines(w, gh, gh->getBagItemGuid(bag, slot - 1));
    return 0;
}

/// SetInventoryItem(unit, slot) - the gear on the paperdoll.
int lua_Tooltip_SetInventoryItem(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 3, 0));
    if (!w || !gh || slot < 1 || slot > 19) { lua_pushboolean(L, 0); return 1; }

    // The unit argument was ignored, so every slot of the inspect paperdoll
    // showed whatever the player themselves had in that slot - the wrong item
    // rather than none, which is the harder kind to notice.
    std::string uid(luaL_optstring(L, 2, "player"));
    wowee::addons::toLowerInPlace(uid);
    if (uid != "player") {
        const uint64_t guid = wowee::addons::resolveUnitGuid(gh, uid);
        auto& cache = gh->inspectedPlayerItemEntriesRef();
        auto it = guid ? cache.find(guid) : cache.end();
        const uint32_t entry = (it != cache.end()) ? it->second[static_cast<size_t>(slot - 1)] : 0;
        lua_pushboolean(L, fillItemTooltipById(L, gh, entry) ? 1 : 0);
        return 1;
    }

    const auto& s = gh->getInventory().getEquipSlot(static_cast<game::EquipSlot>(slot - 1));
    if (s.empty()) { lua_pushboolean(L, 0); return 1; }
    // Through the fuller builder, with the slot's own copy of the item as the
    // floor: the slot knows the name and quality even for an entry no
    // GetItemInfo has arrived for, and answering nothing there would be worse
    // than answering briefly.
    if (!fillItemTooltipById(L, gh, s.item.itemId)) {
        fillItemTooltip(w, s.item, gh);
        fireTooltipSetItem(L);
    } else {
        // Only on the by-id path. fillItemTooltip builds from the instance and
        // has named the suffix itself.
        appendRandomSuffix(w, gh, s.item);
    }
    appendEnchantLines(w, gh, gh->getEquipSlotGuid(slot - 1));
    lua_pushboolean(L, 1);
    return 1;
}

/// The three socketing-panel tooltips.
///
/// Hovering a socket asks for one of these, and unbound they fell to the no-op
/// path: the sockets drew, the gems drew, and hovering any of them produced
/// nothing at all with nothing raised to say why. The panel's own hover handler
/// picks between the first two - the gem waiting to go in, or the one already
/// there - and shows both side by side when a click would replace one with the
/// other.
int lua_Tooltip_SetSocketGem(lua_State* L) {
    auto* gh = wowee::addons::getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 1)) - 1;
    if (!widgetOf(L, 1) || !gh || index < 0 || index > 2) { lua_pushboolean(L, 0); return 1; }
    const uint32_t gemId = gh->getSocketPendingGemItemId(index);
    lua_pushboolean(L, fillItemTooltipById(L, gh, gemId) ? 1 : 0);
    return 1;
}

/// SetExistingSocketGem(index, [comparison]) - the gem already in the socket.
///
/// The second argument means "this is the comparison window", which is how the
/// panel shows what a click would throw away. It changes nothing about which
/// item is described, so it is read and not used rather than silently ignored:
/// the shopping tooltip it fills is positioned by the caller.
int lua_Tooltip_SetExistingSocketGem(lua_State* L) {
    auto* gh = wowee::addons::getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 1)) - 1;
    if (!widgetOf(L, 1) || !gh || index < 0 || index > 2) { lua_pushboolean(L, 0); return 1; }
    const auto enchants = gh->getItemSocketEnchantIds(gh->getSocketItemGuid());
    const uint32_t gemId = gh->getEnchantGemItem(enchants[static_cast<size_t>(index)]);
    lua_pushboolean(L, fillItemTooltipById(L, gh, gemId) ? 1 : 0);
    return 1;
}

/// SetSocketedItem() - the item whose sockets are on screen, which the panel
/// keeps below the sockets as its description pane.
int lua_Tooltip_SetSocketedItem(lua_State* L) {
    auto* gh = wowee::addons::getGameHandler(L);
    if (!widgetOf(L, 1) || !gh) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, fillItemTooltipById(L, gh, gh->getSocketItemId()) ? 1 : 0);
    return 1;
}

/// SetQuestLogSpecialItem(questLogIndex) - the usable item on the watch frame.
///
/// This sat in the method allowlist, which means it was answered with a no-op:
/// the button existed, hovering it did nothing, and nothing raised to say so.
int lua_Tooltip_SetQuestLogSpecialItem(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!w || !gh) { lua_pushboolean(L, 0); return 1; }
    const auto item = wowee::addons::questSpecialItemAt(gh, index);
    lua_pushboolean(L, fillItemTooltipById(L, gh, item.itemId) ? 1 : 0);
    return 1;
}

/// SetCurrencyToken(index) - a row of the currency list.
/// SetBackpackToken(index) - the same for one pinned under the bags.
///
/// Both were unbound, so hovering a currency called nil and took the token
/// frame's OnEnter with it. A currency is an item held in the bags, so its
/// tooltip is the item's.
int lua_Tooltip_ReturnFalse(lua_State* L) { lua_pushboolean(L, 0); return 1; }

int lua_Tooltip_SetCurrencyToken(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!w || !gh) { lua_pushboolean(L, 0); return 1; }
    const uint32_t itemId = wowee::addons::currencyListItemId(L, index);
    lua_pushboolean(L, fillItemTooltipById(L, gh, itemId) ? 1 : 0);
    return 1;
}

/// SetBagItem(bag, slot) - the same for something in the bags.
int lua_Tooltip_SetBagItem(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int bag = static_cast<int>(luaL_optnumber(L, 2, 0));
    const int slot = static_cast<int>(luaL_optnumber(L, 3, 0));
    if (!w || !gh || slot < 1) { lua_pushboolean(L, 0); return 1; }
    const auto& inv = gh->getInventory();
    const auto& s = (bag == 0) ? inv.getBackpackSlot(slot - 1)
                               : inv.getBagSlot(bag - 1, slot - 1);
    if (s.empty()) { lua_pushboolean(L, 0); return 1; }
    // Through the fuller builder, with the slot's own copy of the item as the
    // floor: the slot knows the name and quality even for an entry no
    // GetItemInfo has arrived for, and answering nothing there would be worse
    // than answering briefly.
    if (!fillItemTooltipById(L, gh, s.item.itemId)) {
        fillItemTooltip(w, s.item, gh);
        fireTooltipSetItem(L);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/// SetGuildBankItem(tab, slot) - hovering a slot in the guild bank.
///
/// It was in the no-op allowlist, so the call succeeded and wrote nothing: a
/// guild bank where no item has a tooltip, which reads as the tooltip system
/// being broken rather than as one method missing. The data was already there
/// - GetGuildBankItemInfo answers from the same slots.
int lua_Tooltip_SetGuildBankItem(lua_State* L) {
    auto* w = widgetOf(L, 1);
    auto* gh = wowee::addons::getGameHandler(L);
    const int tab  = static_cast<int>(luaL_optnumber(L, 2, 0));
    const int slot = static_cast<int>(luaL_optnumber(L, 3, 0));
    if (!w || !gh || slot < 1) { lua_pushboolean(L, 0); return 1; }

    const auto& data = gh->getGuildBankData();
    // The open tab is the one kept current; any other answers from whatever
    // the last full update left behind, which is what the panel draws too.
    const std::vector<game::GuildBankItemSlot>* items = nullptr;
    if (tab - 1 == data.tabId) {
        items = &data.tabItems;
    } else if (tab >= 1 && tab <= static_cast<int>(data.tabs.size())) {
        items = &data.tabs[tab - 1].items;
    }
    if (!items) { lua_pushboolean(L, 0); return 1; }

    for (const auto& it : *items) {
        if (it.slotId + 1 != slot) continue;
        lua_pushboolean(L, fillItemTooltipById(L, gh, it.itemEntry) ? 1 : 0);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

int lua_Tooltip_ClearLines(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->tooltipLines.clear();
    // GameTooltip declares OnTooltipCleared and nothing fired it. What it runs
    // is GameTooltip_ClearMoney, so a tooltip that had shown a price kept its
    // money line when it was next filled with something that has none - the
    // lines are cleared here and the money frames are not, because they are
    // frames rather than lines.
    callScriptOnTable(L, 1, "OnTooltipCleared", 0);
    return 0;
}

int lua_Tooltip_NumLines(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? static_cast<lua_Number>(w->tooltipLines.size()) : 0.0);
    return 1;
}

int lua_MessageFrame_Clear(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) { w->messages.clear(); w->messageScroll = 0; }
    return 0;
}

/// GetNumMessages([accessID]) - how many lines, or how many belonging to one
/// conversation.
///
/// The argument is not decoration. FCF_OpenTemporaryWindow walks this count
/// and reads each line with GetMessageInfo, then looks the line's kind up in
/// ChatTypeInfo - so counting *every* line here would walk it onto the ones
/// that have no conversation behind them, hand back no kind for them, and
/// leave it indexing that table with nil. Every plain AddMessage in the
/// interface writes such a line.
int lua_MessageFrame_GetNumMessages(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (!w) { lua_pushnumber(L, 0.0); return 1; }
    if (lua_isnumber(L, 2)) {
        const double id = lua_tonumber(L, 2);
        lua_Number n = 0;
        for (const auto& m : w->messages) if (m.accessId == id) ++n;
        lua_pushnumber(L, n);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(w->messages.size()));
    return 1;
}

int lua_MessageFrame_SetMaxLines(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        const int n = static_cast<int>(luaL_optnumber(L, 2, 128));
        w->maxMessages = (n > 0) ? static_cast<size_t>(n) : 1;
    }
    return 0;
}

int lua_MessageFrame_ScrollUp(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        if (static_cast<size_t>(w->messageScroll) + 1 < w->messages.size())
            ++w->messageScroll;
    }
    return 0;
}

int lua_MessageFrame_ScrollDown(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        if (w->messageScroll > 0) --w->messageScroll;
    }
    return 0;
}

int lua_MessageFrame_ScrollToBottom(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->messageScroll = 0;
    return 0;
}

int lua_Region_GetNumPoints(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? static_cast<lua_Number>(w->anchors.size()) : 0.0);
    return 1;
}

// Minimap zoom. Five levels, as in WoW, and the level is kept on the widget so
// the buttons that step it can read back what they set - Minimap_Update
// compares GetZoom() against GetZoomLevels() - 1 to decide whether to grey the
// zoom-in button out, and nil there is arithmetic on nothing.
int lua_Minimap_SetZoom(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        int z = static_cast<int>(luaL_optnumber(L, 2, 0));
        w->zoomLevel = (z < 0) ? 0 : (z > 4 ? 4 : z);
    }
    return 0;
}

int lua_Minimap_GetZoom(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->zoomLevel : 0);
    return 1;
}

int lua_Minimap_GetZoomLevels(lua_State* L) {
    (void)L;
    lua_pushnumber(L, 5);
    return 1;
}

/// Minimap_OnClick's whole body: a click inside the circle pings that spot for
/// the party. The offsets arrive minimap-local, in interface units from the
/// centre, and turning them into a world position needs the map's view radius
/// and the camera bearing - neither of which Lua can reach. So the request is
/// parked on the widget and the frame loop, which has both, converts it.
int lua_Minimap_PingLocation(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->pingX = static_cast<float>(luaL_optnumber(L, 2, 0));
        w->pingY = static_cast<float>(luaL_optnumber(L, 3, 0));
        w->pingRequested = true;
    }
    return 0;
}

/// Enable and Disable, with the handlers that go with them.
///
/// A disabled button is greyed and takes no clicks, and FrameXML both sets
/// this and listens for it - a scroll bar's arrows disable themselves at the
/// end of their range. Fired only on a real change, since the interface
/// disables what is already disabled on every update.
int lua_Button_Enable(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w || w->enabled) return 0;
    w->enabled = true;
    lua_getfield(L, 1, "__scripts");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "OnEnable");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);
            if (pcallScript(L, "OnEnable", 1, 0) != 0) recordScriptError(L, "OnEnable");
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 0;
}

int lua_Button_Disable(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w || !w->enabled) return 0;
    w->enabled = false;
    lua_getfield(L, 1, "__scripts");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "OnDisable");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);
            if (pcallScript(L, "OnDisable", 1, 0) != 0) recordScriptError(L, "OnDisable");
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 0;
}

int lua_Button_IsEnabled(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    // A number, not a boolean, because that is what WoW answers and FrameXML
    // is written against it: twenty-two places test `IsEnabled() ~= 0` and six
    // test it for truth. A boolean fails the twenty-two silently - `false ~= 0`
    // compares two different types and is therefore *true* - so a disabled
    // button read as enabled everywhere it was asked properly. Pressing return
    // in the macro name box confirmed through a greyed-out OK button that way.
    //
    // The six truth tests then see 0 as true, which is a real flaw, but it is
    // retail's flaw: FrameXML was authored against a client that answers a
    // number here, and matching it is what keeps the other twenty-two right.
    lua_pushnumber(L, w && w->enabled ? 1 : 0);
    return 1;
}

/// Remember a frame that was just shown and declares OnAnimFinished, so that
/// script can be run once the show has returned.
///
/// Only frames that declare it are queued, which in this whole interface is the
/// bag buttons' item animation and nothing else.
static void queueAnimFinished(lua_State* L, int frameIndex) {
    const int abs = frameIndex > 0 ? frameIndex : lua_gettop(L) + frameIndex + 1;
    lua_getfield(L, abs, "__scripts");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "OnAnimFinished");
    const bool has = lua_isfunction(L, -1);
    lua_pop(L, 2);
    if (!has) return;

    lua_getglobal(L, "__WoweePendingAnimFinished");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    const int n = static_cast<int>(lua_objlen(L, -1));
    lua_pushvalue(L, abs);
    lua_rawseti(L, -2, n + 1);
    lua_pop(L, 1);
}

/// Whether every ancestor is shown, read off the flags rather than the layout.
///
/// Show and Hide both ask this, and they have to ask it the same way: a frame
/// whose parent is hidden is not on screen either way round, and answering the
/// two differently would fire one handler of a pair and not the other.
static bool ancestorsShown(lua_State* L, const wowee::ui::Widget* w) {
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree) return true;
    for (uint32_t at = w->parent; at != 0;) {
        const auto* p = tree->get(at);
        if (!p) break;
        if (!p->shown) return false;
        at = p->parent;
    }
    return true;
}

/// Run a frame's OnShow now, from inside Show().
///
/// The visibility pass that normally fires it runs once a frame, and that is
/// too late for anything whose next step depends on what the handler did.
/// ContainerFrame_GenerateFrame is the case that showed it: it writes
/// `bags[bagsShown + 1] = frame:GetName()`, anchors the list, and only then
/// calls Show - and `bagsShown` is incremented by ContainerFrame_OnShow. With
/// the handler deferred, every bag opened in the same breath wrote itself to
/// index 1 over the last one, so the list never held more than one name and
/// only that one was ever anchored. Opening the bags at a vendor put all of
/// them in the same place, one on top of another.
///
/// The real client runs OnShow as part of Show, so this is the faithful
/// order rather than a workaround for that one function.
static void fireOnShowNow(lua_State* L, int frameIndex, wowee::ui::Widget* w) {
    const int abs = frameIndex > 0 ? frameIndex : lua_gettop(L) + frameIndex + 1;
    lua_getfield(L, abs, "__scripts");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "OnShow");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
    lua_pushvalue(L, abs);                       // self
    // Errors are reported the same way every other script call reports them
    // rather than unwinding through Show.
    if (pcallScript(L, "OnShow", 1, 0) != 0) {
        LOG_WARNING("[Lua] OnShow error: ", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);                               // __scripts
    w->onShowFired = true;
}

int lua_Region_Show(lua_State* L) {
    auto* w = widgetOf(L, 1);
    bool becameShown = false;
    if (w) {
        // Counted, not just set. A hide and a show in the same breath leave the
        // flag where it started, and the pass that fires OnShow works by
        // comparing against the last state it reported - so the rebuild the
        // interface asked for is invisible to it. See Widget::shownToggles.
        if (!w->shown && w->shownToggles < 200) ++w->shownToggles;
        becameShown = !w->shown;
        w->shown = true;
    }
    lua_pushboolean(L, 1); lua_setfield(L, 1, "__visible");
    // A frame shown to play a model animation has already finished it.
    //
    // The bag buttons each carry a <Model> that shows itself on ITEM_PUSH and
    // hides itself again from OnAnimFinished - the item flying into the bag.
    // This client plays no model sequences: SetSequence is one of the handful
    // of widget methods that answer with a no-op, so there is no animation to
    // finish and nothing ever fired that script. The frame went up on the first
    // item looted and stayed up, one per bag button, until a reload.
    //
    // Queued rather than called here, for the same reason the text change is:
    // the handler runs Hide, and running it inside Show is the sort of ordering
    // the interface does not expect.
    queueAnimFinished(L, 1);

    // Only when this call is what made it shown, and only when every ancestor
    // is shown too - the same condition the deferred pass uses, read off the
    // flags Show and Hide set rather than off the layout, which has not run.
    // A clean show only. `panel:Hide(); panel:Show()` is the interface asking
    // for a rebuild, and the deferred pass answers that pair together, in
    // order, on purpose - Hide fires no handler of its own, so running OnShow
    // here would put it before the OnHide that is owed. One toggle means this
    // Show is the only thing that happened since the last pass.
    if (w && becameShown && w->shownToggles <= 1 && ancestorsShown(L, w)) {
        fireOnShowNow(L, 1, w);
    }
    return 0;
}

int lua_Region_Hide(lua_State* L) {
    // No OnHide from here, and it is not the asymmetry with Show it looks
    // like. Running it here is what the real client does and it fixes a real
    // fault - ContainerFrame_GenerateFrame indexes its bag list by a counter
    // ContainerFrame_OnHide maintains, so OpenAllBags, which hides every open
    // bag and reopens it in one breath, rebuilds that list from a stale count
    // and the bags come back stacked.
    //
    // It also stops ShowUIPanel from leaving QuestFrame shown, and the four
    // NPC dialogs then fill themselves in from nothing. Measured, not
    // guessed: with OnHide deferred ShowUIPanel(QuestFrame) returns with the
    // frame shown, and with it immediate the same call returns with the frame
    // hidden, so something in UIParent's panel management is ordering-
    // sensitive in a way this client does not reproduce. check_npc_dialogs_fill
    // catches it.
    //
    // The vendor and guild bank no longer take the reopen path - see
    // openBagsForTrading - so what is left exposed is the bag keybind.
    if (auto* w = widgetOf(L, 1)) {
        if (w->shown && w->shownToggles < 200) ++w->shownToggles;
        w->shown = false;
    }
    lua_pushboolean(L, 0); lua_setfield(L, 1, "__visible");
    return 0;
}

int lua_Region_IsShown(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w ? (w->shown ? 1 : 0) : 0);
    return 1;
}

/// IsVisible() - shown, and every ancestor shown too.
///
/// It was an alias for IsShown, which answers only this frame's own flag, so a
/// frame inside a closed panel reported itself visible. Fifty-six places in
/// FrameXML ask this rather than IsShown, and they ask it precisely because
/// they mean "on screen": SpellBookFrame_OnEvent rebuilds the page on
/// SPELLS_CHANGED only when visible, and a dozen others skip work the same way.
///
/// Walks the parents rather than reading the tree's own computed flag, which
/// is only right after a layout pass - a frame shown and asked in the same
/// handler would otherwise answer no.
int lua_Region_IsVisible(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    auto* tree = wowee::addons::getWidgetTree(L);
    bool on = w != nullptr && w->shown;
    if (on && tree) {
        for (uint32_t id = w->parent; id != 0;) {
            const auto* p = tree->get(id);
            if (!p) break;
            if (!p->shown) { on = false; break; }
            id = p->parent;
        }
    }
    lua_pushboolean(L, on);
    return 1;
}

int lua_Region_SetAlpha(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->alpha = static_cast<float>(luaL_optnumber(L, 2, 1.0));
    return 0;
}

int lua_Region_GetAlpha(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->alpha : 1.0);
    return 1;
}

// SetTexture takes either a path or a colour, and addons use both freely.
int lua_Texture_SetTexture(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    if (lua_isnumber(L, 2)) {
        w->solidColor = true;
        w->texturePath.clear();
        w->color[0] = static_cast<float>(lua_tonumber(L, 2));
        w->color[1] = static_cast<float>(luaL_optnumber(L, 3, 0.0));
        w->color[2] = static_cast<float>(luaL_optnumber(L, 4, 0.0));
        w->color[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    } else {
        w->solidColor = false;
        w->texturePath = luaL_optstring(L, 2, "");
    }
    return 0;
}

int lua_Texture_GetTexture(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushstring(L, w ? w->texturePath.c_str() : "");
    return 1;
}

int lua_Texture_SetTexCoord(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        // Eight numbers is the rotated form: a UV per corner, in WoW's order
        // of upper-left, lower-left, upper-right, lower-right. Four is the
        // plain left/right/top/bottom crop. Reading the first four of an
        // eight-number call treats two corners as a crop rectangle, which is
        // how the paperdoll's sideways flyout arrow became a pale bar.
        if (lua_gettop(L) >= 9) {
            for (int i = 0; i < 8; ++i) {
                w->texCoordQuad[i] = static_cast<float>(luaL_optnumber(L, 2 + i, 0.0));
            }
            w->texCoordRotated = true;
            return 0;
        }
        w->texCoordRotated = false;
        w->texCoord[0] = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        w->texCoord[1] = static_cast<float>(luaL_optnumber(L, 3, 1.0));
        w->texCoord[2] = static_cast<float>(luaL_optnumber(L, 4, 0.0));
        w->texCoord[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    }
    return 0;
}

int lua_Texture_SetBlendMode(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        const char* mode = luaL_optstring(L, 2, "BLEND");
        // "ADD" and "ALPHAKEY" are the two that add rather than cover; the rest
        // - BLEND, MOD, DISABLE - are close enough to ordinary blending that
        // telling them apart would not change a pixel here yet.
        w->blendAdd = (std::strcmp(mode, "ADD") == 0);
    }
    return 0;
}

int lua_Texture_GetBlendMode(lua_State* L) {
    auto* w = widgetOf(L, 1);
    lua_pushstring(L, (w && w->blendAdd) ? "ADD" : "BLEND");
    return 1;
}

int lua_Region_SetVertexColor(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        // Lenient, because the interface passes junk into the fourth argument
        // and the real client accepts it. GetMessageTypeColor ends
        //
        //     return info.r, info.g, info.b, group;
        //
        // where group is a *table*, and the chat settings panel spreads that
        // straight into SetVertexColor - so the alpha is a table on every
        // colour swatch it draws. luaL_optnumber falls back to its default for
        // nil and none only; anything else that will not convert raises, which
        // took the panel down as it opened.
        w->color[0] = static_cast<float>(luaOptNumberText(L, 2, 1.0));
        w->color[1] = static_cast<float>(luaOptNumberText(L, 3, 1.0));
        w->color[2] = static_cast<float>(luaOptNumberText(L, 4, 1.0));
        w->color[3] = static_cast<float>(luaOptNumberText(L, 5, 1.0));
    }
    return 0;
}

int lua_Region_SetDrawLayer(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        const auto layer = wowee::ui::parseDrawLayer(luaL_optstring(L, 2, "ARTWORK"));
        // On a status bar this names the layer of the *fill*, which is a
        // region in the real client and is drawn by the bar here. Twenty bars
        // in the interface declare it and the cast bar is one of them: it asks
        // for BORDER so that its own dark backing, which is BACKGROUND, stays
        // behind the fill rather than over it.
        if (w->isStatusBar) w->barLayer = layer;
        else                w->layer = layer;
        w->subLevel = static_cast<int>(luaL_optnumber(L, 3, 0));
    }
    return 0;
}

/// Whether links drawn in this frame answer a click, which is separate from
/// whether the frame has a handler for them. FCF_SetUninteractable turns it off
/// for a chat window made click-through and back on when it is not.
/// Whether OnEnter and OnLeave still run once this button is disabled. WoW
/// suppresses them by default, which is why the XML has an attribute to ask
/// for them back - MainMenuBarMicroButton and LootRollButtonTemplate among the
/// five that do, both of them explaining in a tooltip why they are greyed.
int lua_Frame_SetMotionScriptsWhileDisabled(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->motionScriptsWhileDisabled = lua_isnone(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    }
    return 0;
}

int lua_Frame_GetMotionScriptsWhileDisabled(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->motionScriptsWhileDisabled ? 1 : 0);
    return 1;
}

int lua_Frame_SetHyperlinksEnabled(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->hyperlinksEnabled = lua_isnone(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    }
    return 0;
}

int lua_Frame_EnableMouse(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        // Absent argument means true, which is how addons usually write it.
        w->mouseEnabled = lua_isnone(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    }
    return 0;
}

int lua_Frame_IsMouseEnabled(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->mouseEnabled ? 1 : 0);
    return 1;
}

int lua_Frame_SetFrameStrata(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->strata = wowee::ui::parseStrata(luaL_optstring(L, 2, "MEDIUM"));
        w->strataExplicit = true;
    }
    return 0;
}

/// Frame:GetFrameStrata() - the stratum SetFrameStrata was given.
///
/// This was a Lua method answering self.__strata, and SetFrameStrata is a C
/// binding that writes the widget rather than that field - so it answered
/// MEDIUM for every frame in the interface however it had been set, including
/// the ones the XML puts in FULLSCREEN_DIALOG. Anything branching on it was
/// deciding from a constant.
int lua_Frame_GetFrameStrata(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushstring(L, w ? wowee::ui::strataName(w->strata) : "MEDIUM");
    return 1;
}

int lua_Frame_SetFrameLevel(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const int wanted = static_cast<int>(luaL_optnumber(L, 2, 0));
    // Anything inside that set its own level moves by the same amount, because
    // a child's level is relative to its parent. Raising a window by hand and
    // raising it by clicking it must leave the same arrangement behind.
    if (tree && id) tree->shiftExplicitLevels(id, wanted - w->effLevel);
    w->level = wanted;
    w->levelExplicit = true;
    return 0;
}

int lua_FontString_SetText(lua_State* L) {
    // Anything but a string is taken as no text rather than as an error.
    //
    // WoW raises here, and so would this - except that the missing-API
    // fallback hands back a callable for a name nothing defines, so a label
    // fed a global that does not exist is given a function where WoW would
    // have given it a string. Raising kills the handler that was mid-update,
    // which costs far more than the empty label does: one such call took out
    // chatconfigframe's whole OnEvent.
    const char* text = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const std::string shown = wowee::ui::resolvePluralEscapes(text);
    if (auto* w = widgetOf(L, 1)) w->text = shown;
    // GetText answers what was set, not what is drawn. WoW resolves the escape
    // on the way to the screen and hands the original back, and FrameXML reads
    // its own labels back in a few places to re-format them.
    lua_pushstring(L, text);
    lua_setfield(L, 1, "_text");
    return 0;
}

int lua_FontString_GetText(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushstring(L, w ? w->text.c_str() : "");
    return 1;
}

int lua_FontString_SetJustifyH(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->justifyH = luaL_optstring(L, 2, "CENTER");
    return 0;
}

int lua_FontString_SetJustifyV(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->justifyV = luaL_optstring(L, 2, "MIDDLE");
    return 0;
}

int lua_FontString_GetJustifyV(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushstring(L, w ? w->justifyV.c_str() : "MIDDLE");
    return 1;
    return 0;
}

/// GetTextColor() → r, g, b, a. The getter of a setter that has been
/// implemented all along, which is the asymmetry worth watching for.
///
/// It is asked of two different things and has to answer both. A font string
/// is a widget and keeps its colour there. A **font object** - GameFontNormal
/// and its ninety siblings - is a plain Lua table with r, g, b and a fields,
/// which is what applyFontObject reads when a font string inherits one.
///
/// optionspaneltemplates is the caller: re-enabling a control does
/// `SetTextColor(fontObject:GetTextColor())`, and nil for all four made that
/// SetTextColor() with no arguments - which defaults to white rather than to
/// the colour the font object actually carries.
int lua_FontString_GetTextColor(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    wowee::ui::Widget* w = widgetOf(L, 1);
    // A button colours the label it holds, exactly as the setter does.
    if (w && w->kind != wowee::ui::WidgetKind::FontString && tree) {
        lua_getfield(L, 1, "__fontString");
        if (lua_istable(L, -1)) {
            if (auto* fs = tree->get(widgetIdOf(L, lua_gettop(L)))) w = fs;
        }
        lua_pop(L, 1);
    }
    if (w) {
        for (float component : w->color) lua_pushnumber(L, component);
        return 4;
    }
    if (lua_istable(L, 1)) {
        static const char* keys[4] = {"r", "g", "b", "a"};
        for (auto& key : keys) {
            lua_getfield(L, 1, key);
            lua_pushnumber(L, lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 1.0);
            lua_remove(L, -2);
        }
        return 4;
    }
    for (int i = 0; i < 4; ++i) lua_pushnumber(L, 1.0);
    return 4;
}

int lua_FontString_SetTextColor(lua_State* L) {
    // On a button this colours the label it holds, which is where a button's
    // text actually lives; on a font string it colours itself. FrameXML calls
    // it both ways at two hundred sites - greying an unavailable option,
    // reddening a cost that cannot be paid - and on the frame metatable it was
    // a no-op, so none of that showed.
    auto* tree = wowee::addons::getWidgetTree(L);
    wowee::ui::Widget* w = widgetOf(L, 1);
    if (w && w->kind != wowee::ui::WidgetKind::FontString && tree) {
        lua_getfield(L, 1, "__fontString");
        if (lua_istable(L, -1)) {
            if (auto* fs = tree->get(widgetIdOf(L, lua_gettop(L)))) w = fs;
        }
        lua_pop(L, 1);
    }
    if (w) {
        w->color[0] = static_cast<float>(luaL_optnumber(L, 2, 1.0));
        w->color[1] = static_cast<float>(luaL_optnumber(L, 3, 1.0));
        w->color[2] = static_cast<float>(luaL_optnumber(L, 4, 1.0));
        w->color[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    }
    return 0;
}

int lua_FontString_SetFont(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        if (lua_isstring(L, 2)) w->fontFace = lua_tostring(L, 2);
        // The flags argument, where "OUTLINE" and "THICKOUTLINE" arrive.
        if (const char* flags = lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr) {
            const std::string f(flags);
            if (f.find("THICK") != std::string::npos)        w->fontOutline = "THICK";
            else if (f.find("OUTLINE") != std::string::npos) w->fontOutline = "NORMAL";
            else                                             w->fontOutline.clear();
        }
        // (path, height, flags). Only the height is honoured for now; the path
        // needs a font atlas rebuild, which cannot happen mid-frame.
        const double h = luaL_optnumber(L, 3, 0.0);
        if (h > 0.0) w->fontHeight = static_cast<float>(h);
    }
    return 0;
}

/// GetFont() → path, height, flags.
///
/// The height is the part anything does arithmetic on: WatchFrame measures a
/// test line with local _, fontHeight = line.text:GetFont() and divides by it
/// two lines later, so answering nothing loses the file.
int lua_FontString_GetFont(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushstring(L, "Fonts\\FRIZQT__.TTF");
    lua_pushnumber(L, wowee::ui::interfaceFontSize(w ? w->fontHeight : 0.0f));
    lua_pushstring(L, "");
    return 3;
}

/// Extra space between wrapped lines. Zero unless set, and a number either
/// way: WorldMapFrame adds it to a font height on the line after asking.
int lua_FontString_GetSpacing(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->lineSpacing : 0.0);
    return 1;
}

int lua_FontString_SetSpacing(lua_State* L) {
    if (auto* w = widgetOf(L, 1))
        w->lineSpacing = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    return 0;
}

/// SetFontObject(obj) where obj is one of the shared font objects, which carry
/// a height and a colour. FrameXML reaches for these more than three thousand
/// times, so a FontString that ignores them is the wrong size and colour nearly
/// everywhere.
/// Read a font object - a table, or the name of one - onto a widget.
///
/// Shared because a button says the same thing a different way: a font string
/// has SetFontObject, a button has SetNormalFontObject and the font belongs to
/// the font string it holds.
static void applyFontObject(lua_State* L, int fontIndex, wowee::ui::Widget* w) {
    if (!w) return;
    if (lua_isstring(L, fontIndex)) {       // by name
        // Remembered, not only unpacked. GetFontObject has to hand the object
        // back, and the fields copied out of it cannot be reassembled into the
        // one it came from. A name covers the path that matters: every
        // `inherits="GameFontNormal"` in the XML emits SetFontObject with the
        // name as a string.
        w->fontObjectName = lua_tostring(L, fontIndex);
        lua_getglobal(L, lua_tostring(L, fontIndex));
    } else {
        lua_pushvalue(L, fontIndex);
    }
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "height");
        if (lua_isnumber(L, -1)) w->fontHeight = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        // Which typeface, not only how big. FrameXML sets its headings in
        // MORPHEUS and its damage numbers in SKURRI, and a font object is
        // where it says so.
        lua_getfield(L, -1, "font");
        if (lua_isstring(L, -1)) w->fontFace = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "outline");
        if (lua_isstring(L, -1)) w->fontOutline = lua_tostring(L, -1);
        lua_pop(L, 1);
        const char* keys[4] = {"r", "g", "b", "a"};
        for (int i = 0; i < 4; ++i) {
            lua_getfield(L, -1, keys[i]);
            if (lua_isnumber(L, -1)) w->color[i] = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_getfield(L, -1, "shadowX");
        if (lua_isnumber(L, -1)) {
            w->hasShadow = true;
            w->shadowX = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "shadowY");
        if (lua_isnumber(L, -1)) w->shadowY = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        const char* skeys[4] = {"shadowR", "shadowG", "shadowB", "shadowA"};
        for (int i = 0; i < 4; ++i) {
            lua_getfield(L, -1, skeys[i]);
            if (lua_isnumber(L, -1)) {
                w->shadowColor[i] = static_cast<float>(lua_tonumber(L, -1));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

int lua_FontString_SetShadowOffset(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->hasShadow = true;
        w->shadowX = static_cast<float>(luaL_optnumber(L, 2, 1.0));
        w->shadowY = static_cast<float>(luaL_optnumber(L, 3, -1.0));
    }
    return 0;
}

int lua_FontString_SetShadowColor(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->shadowColor[0] = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        w->shadowColor[1] = static_cast<float>(luaL_optnumber(L, 3, 0.0));
        w->shadowColor[2] = static_cast<float>(luaL_optnumber(L, 4, 0.0));
        w->shadowColor[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    }
    return 0;
}

/// SetTextHeight(height) - the font's size, keeping its face and flags.
///
/// A different thing from GetTextHeight, which answers how tall the rendered
/// string turned out; this one is a setter and the two are not a pair.
///
/// The raid-warning banner is the caller that shows it: RaidNotice_UpdateSlot
/// scales its text up over the first fraction of a second and back down over
/// the rest, so with this doing nothing the banner appeared at whatever size
/// its template carried and sat there. Combat feedback sizes its numbers by
/// hit type through the same call, and the scrolling combat text its crits.
int lua_FontString_SetTextHeight(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        const double h = luaL_optnumber(L, 2, 0.0);
        if (h > 0.0) w->fontHeight = static_cast<float>(h);
    }
    return 0;
}

int lua_FontString_SetFontObject(lua_State* L) {
    applyFontObject(L, 2, widgetOf(L, 1));
    return 0;
}

/// FontString:GetFontObject() → the font object its settings came from.
///
/// Nil until now, and the caller that wanted it does not check:
///
///     local fontObject = text:GetFontObject()
///     text:SetTextColor(fontObject:GetTextColor())
///
/// which is how every options-panel control puts its label back to full colour
/// when it is re-enabled - so enabling a checkbox raised instead.
///
/// Answers the shared global by name. A region whose font was set field by
/// field rather than from an object has no object to name, and nil is the right
/// answer there: it is what the real client says too.
int lua_FontString_GetFontObject(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (!w || w->fontObjectName.empty()) { lua_pushnil(L); return 1; }
    lua_getglobal(L, w->fontObjectName.c_str());
    return 1;
}

/// EditBox:GetInputLanguage() → which input method the box is taking text
/// from, as one of ROMAN, CHINESE, JAPANESE or KOREAN.
///
/// Always ROMAN here: this client has no IME, so there is nothing else it could
/// truthfully be. The answer still has to be a string, because its one caller
/// concatenates it -
///
///     local variable = _G["INPUT_"..self:GetInputLanguage()]
///
/// - and nil in a concatenation raises. Only reachable once
/// OnInputLanguageChanged is fired, which nothing does yet, so this is the
/// shape being closed rather than a fault being seen.
int lua_EditBox_GetInputLanguage(lua_State* L) {
    (void)L;
    lua_pushstring(L, "ROMAN");
    return 1;
}

/// Texture:GetVertexColor() → the tint SetVertexColor last applied.
///
/// White until something says otherwise, which is what an untinted texture
/// draws at. The tabard designer reads all three of its swatches back this way
/// to seed the colour picker.
int lua_Region_GetVertexColor(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    for (int i = 0; i < 4; ++i) lua_pushnumber(L, w ? w->color[i] : 1.0);
    return 4;
}

/// Button:SetNormalFontObject(font) - the font its label is drawn in.
///
/// FrameXML declares this as <NormalFont style="GameFontNormal"/> on 71 button
/// templates and never sets the font on the label itself, so without this every
/// button in the interface drew its text at the built-in default rather than at
/// the size and face the template asked for.
int lua_Frame_SetNormalFontObject(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree) return 0;
    // The label, not the button: a button has no text of its own.
    lua_getfield(L, 1, "__fontString");
    wowee::ui::Widget* fs =
        lua_istable(L, -1) ? tree->get(widgetIdOf(L, lua_gettop(L))) : nullptr;
    lua_pop(L, 1);
    applyFontObject(L, 2, fs ? fs : widgetOf(L, 1));
    return 0;
}

/// Attach the shared region methods to the table on top of the stack.
void installRegionMethods(lua_State* L, bool isTexture, bool isFontString) {
    auto set = [&](const char* name, lua_CFunction fn) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, name);
    };
    set("GetName", lua_Region_GetName);
    // The frame the region was created on. Without this the name falls through
    // to the shared widget no-op - GetParent is in __WoweeWidgetMethods - so
    // every texture and font string in the interface answered nil for it, and
    // the region's own __parent, which CreateTexture writes, was never read.
    // A region's parent can be changed as well as read. Both halves of the
    // pair were missing and for the same reason: SetParent is a name in
    // __WoweeWidgetMethods, so on a texture it fell to the shared no-op and
    // did nothing at all.
    //
    // PlayerTalentFrameActiveSpecTabHighlight is a Texture and the talent
    // frame moves it between the two spec tabs by reparenting it, so the
    // highlight stayed wherever it was first put.
    set("SetParent", lua_Region_SetParent);
    // A region has a centre like any other measured thing, and the frame
    // table has answered this since it was written.
    set("GetCenter", lua_Region_GetCenter);
    set("GetParent", lua_Region_GetParent);
    set("SetPoint", lua_Region_SetPoint);
    set("ClearAllPoints", lua_Region_ClearAllPoints);
    set("SetAllPoints", lua_Region_SetAllPoints);
    set("SetSize", lua_Region_SetSize);
    set("SetWidth", lua_Region_SetWidth);
    set("SetHeight", lua_Region_SetHeight);
    set("GetWidth", lua_Region_GetWidth);
    set("GetTextWidth", lua_Region_GetTextWidth);
    set("GetStringWidth", lua_Region_GetTextWidth);
    set("GetTextHeight", lua_Region_GetTextHeight);
    set("GetStringHeight", lua_Region_GetTextHeight);
    set("GetFieldSize", lua_Region_GetFieldSize);
    set("GetHeight", lua_Region_GetHeight);
    set("GetLeft", lua_Region_GetLeft);
    set("GetRight", lua_Region_GetRight);
    set("GetBottom", lua_Region_GetBottom);
    set("GetTop", lua_Region_GetTop);
    set("GetRect", lua_Region_GetRect);
    set("GetPoint", lua_Region_GetPoint);
    set("GetObjectType", lua_Region_GetObjectType);
    set("IsObjectType", lua_Region_IsObjectType);
    set("GetNumPoints", lua_Region_GetNumPoints);
    set("GetScale", lua_Region_GetScale);
    set("SetScale", lua_Region_SetScale);
    set("GetEffectiveScale", lua_Region_GetEffectiveScale);
    set("Show", lua_Region_Show);
    set("Hide", lua_Region_Hide);
    set("IsShown", lua_Region_IsShown);
    set("IsVisible", lua_Region_IsVisible);
    set("SetAlpha", lua_Region_SetAlpha);
    set("GetAlpha", lua_Region_GetAlpha);
    set("SetVertexColor", lua_Region_SetVertexColor);
    set("GetVertexColor", lua_Region_GetVertexColor);
    set("GetFontObject", lua_FontString_GetFontObject);
    set("SetDrawLayer", lua_Region_SetDrawLayer);
    if (isTexture) {
        set("SetTexture", lua_Texture_SetTexture);
        set("GetTexture", lua_Texture_GetTexture);
        set("SetTexCoord", lua_Texture_SetTexCoord);
        set("SetBlendMode", lua_Texture_SetBlendMode);
        set("GetBlendMode", lua_Texture_GetBlendMode);
    }
    if (isFontString) {
        set("GetFont", lua_FontString_GetFont);
        set("SetFont", lua_FontString_SetFont);
        set("SetTextHeight", lua_FontString_SetTextHeight);
        set("GetTextColor", lua_FontString_GetTextColor);
        set("SetText", lua_FontString_SetText);
        set("SetFormattedText", lua_FontString_SetFormattedText);
        set("GetText", lua_FontString_GetText);
        set("SetJustifyH", lua_FontString_SetJustifyH);
        set("SetJustifyV", lua_FontString_SetJustifyV);
        set("GetJustifyV", lua_FontString_GetJustifyV);
        set("SetTextColor", lua_FontString_SetTextColor);
        set("SetFont", lua_FontString_SetFont);
        set("GetFont", lua_FontString_GetFont);
        set("GetSpacing", lua_FontString_GetSpacing);
        set("SetSpacing", lua_FontString_SetSpacing);
        set("SetFontObject", lua_FontString_SetFontObject);
        set("SetShadowOffset", lua_FontString_SetShadowOffset);
        set("SetShadowColor", lua_FontString_SetShadowColor);
    }
    // Anything still unimplemented stays a no-op rather than an error, which is
    // what keeps a large addon running while the surface is filled in.
    // The same enumerated set the frame metatable uses, and for the same
    // reason: a region carries data fields beside its methods, and answering
    // both with a no-op makes a field that was never set look present.
    // Built once and shared. This used to compile a fresh chunk for every
    // region created, which over a FrameXML load is thousands of compiles of
    // the same three lines.
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_region_mt");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_dostring(L,
            "local known = __WoweeWidgetMethods or {} "
            "return { __index = function(_, k) "
            "  if type(k)=='string' and known[k] then return function() end end "
            "end }");
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "wowee_region_mt");
    }
    lua_setmetatable(L, -2);
}

/// Read just the colour out of a font object, for the states this renderer
/// draws by colour alone. The face and size come from the normal font: a
/// button whose label changed size on hover would jump about.
static void applyStateColor(lua_State* L, int fontIndex, float out[4], bool& flag) {
    if (lua_isstring(L, fontIndex)) {
        lua_getglobal(L, lua_tostring(L, fontIndex));
    } else {
        lua_pushvalue(L, fontIndex);
    }
    if (lua_istable(L, -1)) {
        const char* keys[4] = {"r", "g", "b", "a"};
        for (int i = 0; i < 4; ++i) {
            lua_getfield(L, -1, keys[i]);
            if (lua_isnumber(L, -1)) {
                out[i] = static_cast<float>(lua_tonumber(L, -1));
                flag = true;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

int lua_Frame_SetHighlightFontObject(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        applyStateColor(L, 2, w->highlightColor, w->hasHighlightColor);
    }
    return 0;
}

int lua_Frame_SetDisabledFontObject(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        applyStateColor(L, 2, w->disabledColor, w->hasDisabledColor);
    }
    return 0;
}

/// The four components of a state colour, as SetTextColor takes them.
///
/// Missing components read as white, which is what SetTextColor does with no
/// arguments - a three-argument call is a colour with no alpha, not a colour
/// that is transparent.
static void applyStateComponents(lua_State* L, float out[4], bool& flag) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<float>(luaL_optnumber(L, 2 + i, 1.0));
    }
    flag = true;
}

/// Button:SetDisabledTextColor(r, g, b, a) - the label's colour while the
/// button is disabled, given as components rather than as a font object.
///
/// The pair to SetDisabledFontObject, which reaches the same two fields by
/// reading those components out of a font object's table. Only the font-object
/// half was bound, and the components half is the one uidropdownmenu.lua uses
/// to grey an entry - on the button every dropdown in the interface is built
/// from. So UIDropDownMenu_AddButton raised on the first disabled entry it was
/// given, and raising while a file is being read takes the whole file with it:
/// the video options and the interface options both died there on a 1.12
/// interface, and no dropdown that names a disabled entry could be built.
int lua_Frame_SetDisabledTextColor(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        applyStateComponents(L, w->disabledColor, w->hasDisabledColor);
    }
    return 0;
}

/// Button:SetHighlightTextColor(r, g, b, a) - the same for the hover colour.
///
/// Bound with its sibling rather than after the next report. The two are one
/// mechanism, they are declared together everywhere they are documented, and
/// the cost of the missing one is not a wrong colour but the file it is called
/// from.
int lua_Frame_SetHighlightTextColor(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        applyStateComponents(L, w->highlightColor, w->hasHighlightColor);
    }
    return 0;
}

int lua_Frame_SetPushedTextOffset(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->pushedTextOffsetX = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        w->pushedTextOffsetY = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    }
    return 0;
}

int lua_Frame_GetPushedTextOffset(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->pushedTextOffsetX : 0.0);
    lua_pushnumber(L, w ? w->pushedTextOffsetY : 0.0);
    return 2;
}

int lua_Frame_SetHitRectInsets(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->hitInsetLeft   = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        w->hitInsetRight  = static_cast<float>(luaL_optnumber(L, 3, 0.0));
        w->hitInsetTop    = static_cast<float>(luaL_optnumber(L, 4, 0.0));
        w->hitInsetBottom = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    }
    return 0;
}

int lua_Frame_GetHitRectInsets(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->hitInsetLeft   : 0.0);
    lua_pushnumber(L, w ? w->hitInsetRight  : 0.0);
    lua_pushnumber(L, w ? w->hitInsetTop    : 0.0);
    lua_pushnumber(L, w ? w->hitInsetBottom : 0.0);
    return 4;
}

/// SetUserPlaced marks a frame as positioned by the player.
///
/// The tree already tracks this - it is what stops the interface's own layout
/// pass moving a window the player has dragged - but the two calls that read
/// and set it answered as no-ops, so a frame restored from saved variables was
/// not treated as placed and could be shifted out from under its own position.
int lua_Frame_SetUserPlaced(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->userMoved = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Frame_IsUserPlaced(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->userMoved);
    return 1;
}

/// The value a bar is showing, as distinct from the one it was told to reach.
///
/// WoW animates between them; this draws the value directly, so the two are
/// the same number. Answering honestly matters because the smoothing code
/// compares them and loops while they differ - against a no-op returning
/// nothing, that comparison never settles.
int lua_StatusBar_GetCurrentValue(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->barValue : 0.0);
    return 1;
}

int lua_StatusBar_SetDisplayValue(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->barValue = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    }
    return 0;
}

/// frame:GetChildren() - every child frame, as a list of return values.
///
/// The count is the payload here, not any one value: FrameXML spreads it
/// straight into a vararg call, so answering nothing is not a wrong answer but
/// an empty one. The call happens, the callee's `...` is empty, and its work
/// silently does not occur - the same shape that once left every chat window
/// registered for no message at all.
///
/// `ApplyUnitButtonConfiguration(frame:GetChildren())` is the live case: it is
/// how the secure templates recurse into a unit button's nested frames, so
/// with nothing returned the top level was configured and everything under it
/// was not.
///
/// Frames only. Textures and font strings are a frame's *regions* in WoW and
/// come back from GetRegions, which nothing here calls.
int lua_Frame_GetChildren(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (!tree || id == 0) return 0;
    const auto* w = tree->get(id);
    if (!w) return 0;

    lua_getglobal(L, "__WoweeFramesByWid");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    const int registry = lua_gettop(L);

    // Room for all of them before any are pushed. Lua guarantees only a small
    // slack above the arguments, and UIParent has hundreds of children - the
    // first run of this corrupted the heap rather than raising, because
    // lua_pushinteger past the top writes outside the stack.
    if (!lua_checkstack(L, static_cast<int>(w->children.size()) + 2)) {
        lua_pop(L, 1);
        LOG_WARNING("GetChildren: no room for ", w->children.size(),
                    " children of '", w->name, "'; answering none");
        return 0;
    }

    int pushed = 0;
    for (const uint32_t childId : w->children) {
        const auto* child = tree->get(childId);
        if (!child || child->kind != wowee::ui::WidgetKind::Frame) continue;
        lua_pushinteger(L, static_cast<lua_Integer>(childId));
        lua_rawget(L, registry);
        if (lua_istable(L, -1)) {
            ++pushed;
        } else {
            // A widget with no Lua table behind it is not a child anyone can
            // be handed; dropping it keeps the list free of nils, which a
            // vararg callee would count as children.
            lua_pop(L, 1);
        }
    }
    lua_remove(L, registry);    // drop the registry, keep the children above it
    return pushed;
}

int lua_Frame_EnableKeyboard(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->keyboardEnabled = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Frame_IsKeyboardEnabled(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->keyboardEnabled);
    return 1;
}

int lua_Frame_SetPropagateKeyboardInput(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->propagateKeys = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Frame_SetToplevel(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->topLevel = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Frame_IsToplevel(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->topLevel);
    return 1;
}

int lua_Frame_Raise(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (tree && id) tree->raise(id);
    return 0;
}

int lua_Frame_Lower(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t id = widgetIdOf(L, 1);
    if (tree && id) tree->lower(id);
    return 0;
}

int lua_Frame_SetClampedToScreen(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->clampedToScreen = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_Frame_IsClampedToScreen(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, w && w->clampedToScreen);
    return 1;
}

/// SetClampRectInsets(left, right, top, bottom) - how far past each screen
/// edge a clamped frame is allowed to sit.
///
/// It answered with a no-op, so every clamped frame was held fully on screen.
/// The world map says what that costs in a comment beside its own call -
/// SetClampRectInsets(0, 0, 0, -60), "don't overlap the xp/rep bars" - and the
/// chat frame, which is clamped and movable and asks to overhang on three
/// sides, could be dragged to places the real client does not allow.
int lua_Frame_SetClampRectInsets(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    w->clampInsetL = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    w->clampInsetR = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    w->clampInsetT = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    w->clampInsetB = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    return 0;
}

int lua_Frame_SetBackdrop(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    if (!lua_istable(L, 2)) {          // SetBackdrop(nil) clears it
        w->hasBackdrop = false;
        return 0;
    }
    w->hasBackdrop = true;
    auto str = [&](const char* key, std::string& out) {
        lua_getfield(L, 2, key);
        if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
        lua_pop(L, 1);
    };
    auto num = [&](const char* key, float& out) {
        lua_getfield(L, 2, key);
        if (lua_isnumber(L, -1)) out = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    };
    str("bgFile", w->bgFile);
    str("edgeFile", w->edgeFile);
    num("edgeSize", w->edgeSize);
    // tileSize describes the background's repeat, and edgeSize the border tile.
    // Where only one is given the other is the sensible stand-in.
    num("tileSize", w->edgeSize);
    num("edgeSize", w->edgeSize);
    lua_getfield(L, 2, "tile");
    w->tileBackground = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 2, "insets");
    if (lua_istable(L, -1)) {
        auto inset = [&](const char* key, float& out) {
            lua_getfield(L, -1, key);
            if (lua_isnumber(L, -1)) out = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        };
        inset("left", w->insetLeft);
        inset("right", w->insetRight);
        inset("top", w->insetTop);
        inset("bottom", w->insetBottom);
    }
    lua_pop(L, 1);
    return 0;
}

int lua_Frame_SetBackdropColor(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->backdropColor[0] = static_cast<float>(luaL_optnumber(L, 2, 1.0));
        w->backdropColor[1] = static_cast<float>(luaL_optnumber(L, 3, 1.0));
        w->backdropColor[2] = static_cast<float>(luaL_optnumber(L, 4, 1.0));
        w->backdropColor[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    }
    return 0;
}

int lua_Frame_SetBackdropBorderColor(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->borderColor[0] = static_cast<float>(luaL_optnumber(L, 2, 1.0));
        w->borderColor[1] = static_cast<float>(luaL_optnumber(L, 3, 1.0));
        w->borderColor[2] = static_cast<float>(luaL_optnumber(L, 4, 1.0));
        w->borderColor[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    }
    return 0;
}

int lua_StatusBar_SetMinMaxValues(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->barMin = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        w->barMax = static_cast<float>(luaL_optnumber(L, 3, 1.0));
    }
    return 0;
}

int lua_StatusBar_GetMinMaxValues(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->barMin : 0.0);
    lua_pushnumber(L, w ? w->barMax : 1.0);
    return 2;
}

int lua_StatusBar_SetValue(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const float value = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    const bool changed = (value != w->barValue);
    w->barValue = value;

    // A value set from code fires OnValueChanged, as in WoW - for a status bar
    // as well as a slider. The note here used to say a status bar does not,
    // and that was wrong: StatusBar carries the script too, and FrameXML
    // leans on it.
    //
    // The experience bar is the case that showed it. mainmenubar.xml:160 puts
    // TextStatusBar_OnValueChanged in exactly this script, and that is what
    // rewrites the percentage - so the number sat at whatever it was when the
    // bar was last touched, and only corrected itself when the cursor crossed
    // the bar and OnEnter refreshed the text by another route. Killing a mob
    // moved the fill and left the percentage stale.
    //
    // Called through the table so the handler runs with self, and only on a
    // real change, because several of these set the value they already have on
    // every update - which also stops a handler that sets its own bar from
    // recurring.
    if (changed) {
        lua_getfield(L, 1, "__scripts");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "OnValueChanged");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, 1);
                lua_pushnumber(L, value);
                if (pcallScript(L, "OnValueChanged", 2, 0) != 0)
                    recordScriptError(L, "OnValueChanged");
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}

/// ColorSelect:SetColorRGB(r, g, b) - the colour the picker is showing.
///
/// Fires OnColorSelect the way a slider fires OnValueChanged, because that
/// script is the whole contract: colorpickerframe.xml puts the swatch update
/// and the caller's own `func` inside it, so a colour set without firing is a
/// colour nothing hears about.
///
/// The pair was unanswered, and ten call sites read the getter - every
/// ChangeChatColor from the chat colour menu, the arena tabard's three
/// swatches, the chat config panel. They were handing nil to functions that
/// take r, g, b, so accepting a colour set the colour to nothing.
int lua_ColorSelect_SetColorRGB(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const float r = static_cast<float>(luaL_optnumber(L, 2, 1.0));
    const float g = static_cast<float>(luaL_optnumber(L, 3, 1.0));
    const float b = static_cast<float>(luaL_optnumber(L, 4, 1.0));
    const bool changed = (r != w->pickerColor[0] || g != w->pickerColor[1] ||
                          b != w->pickerColor[2]);
    w->pickerColor[0] = r;
    w->pickerColor[1] = g;
    w->pickerColor[2] = b;
    // The wheel and the bar move in HSV, so a colour set from outside has to
    // land there too - otherwise opening the picker on a colour puts both
    // thumbs wherever the last colour left them.
    const float hueBefore = w->pickerHSV[0];
    wowee::ui::rgbToHsv(w->pickerColor, w->pickerHSV);
    // Grey and black have no hue to report, and taking the zero rgbToHsv
    // gives back would swing the wheel thumb to red every time the value bar
    // reached the bottom.
    if (w->pickerHSV[1] <= 0.0f) w->pickerHSV[0] = hueBefore;
    if (!changed) return 0;

    lua_getfield(L, 1, "__scripts");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "OnColorSelect");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);
            lua_pushnumber(L, r);
            lua_pushnumber(L, g);
            lua_pushnumber(L, b);
            if (pcallScript(L, "OnColorSelect", 4, 0) != 0)
                recordScriptError(L, "OnColorSelect");
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 0;
}

int lua_ColorSelect_GetColorRGB(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    for (int i = 0; i < 3; ++i) lua_pushnumber(L, w ? w->pickerColor[i] : 1.0);
    return 3;
}

/// SetCooldown(start, duration) - both on GetTime's clock. A zero duration is
/// how FrameXML clears one, and it must read as nothing running rather than as
/// a sweep that never finishes.
/// Remember an edit box whose text was set from code, so its OnTextChanged can
/// be run after the script that set it - which is when WoW runs one.
///
/// Deferred rather than immediate, and the difference is not academic.
/// MoneyInputFrame_SetCopper calls SetNumber and increments its expectChanges
/// counter on the *next line*; a handler that runs inside SetNumber sees the
/// counter before that increment, clears it, and the increment then does
/// nil + 1. Queuing here and draining in dispatchOnUpdate puts the handler
/// after the whole of SetCopper, which is the order the interface is written
/// for.
///
/// The queue is a Lua table rather than a list of widget pointers so that a
/// frame going away between the set and the drain is Lua's problem rather than
/// a dangling read. Same reason __WoweeOnUpdateFrames is a table.
static void queueTextChanged(lua_State* L, int frameIndex) {
    const int abs = frameIndex > 0 ? frameIndex : lua_gettop(L) + frameIndex + 1;
    lua_getglobal(L, "__WoweePendingTextChanged");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    const int n = static_cast<int>(lua_objlen(L, -1));
    lua_pushvalue(L, abs);
    lua_rawseti(L, -2, n + 1);
    lua_pop(L, 1);
}

/// An edit box keeps its own text, so SetText on one is not the font string's.
/// FrameXML uses the same name for both and the widget decides which it means.
int lua_EditBox_SetText(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    w->editText = luaL_optstring(L, 2, "");
    w->cursorPos = w->editText.size();
    // The text no longer came from the history, so the arrows start again from
    // the newest line rather than resuming from the middle of a walk.
    w->editHistoryPos = -1;
    // OnTextSet is the counterpart to OnTextChanged for a text set from code,
    // and nothing fired it. The chat box declares it and runs
    // ChatEdit_ParseText, which is what reads a leading /w or /g and switches
    // the box's channel - so a message put into the box by anything but typing
    // went out on whatever channel was already selected.
    //
    // Guarded, because a handler is free to set the text again and this is
    // called from inside one. The only handler in this interface does not, but
    // an addon's may, and a Lua loop that feeds itself takes the process with
    // it rather than raising.
    static bool inSetText = false;
    if (!inSetText) {
        inSetText = true;
        callScriptOnTable(L, 1, "OnTextSet", 0);
        inSetText = false;
    }
    queueTextChanged(L, 1);
    // Not fired here, deferred - the reasoning, and what it cost while nothing
    // fired at all:, and this is not because it
    // should not be - WoW fires it for a text set from code as well as for
    // typing, and FrameXML is written expecting that. It is because firing it
    // *synchronously* is wrong, which was measured rather than reasoned:
    //
    // MoneyInputFrame_SetCopper counts the boxes it is about to change so its
    // own edits are not mistaken for the player's. It calls SetNumber and then
    // increments expectChanges on the next line. A synchronous OnTextChanged
    // runs between those two, sees expectChanges at 0, sets it to nil, and the
    // increment then does nil + 1 - moneyinputframe.lua:48, every time money
    // is put into a trade or a mail.
    //
    // So WoW's is deferred: queued during the call and run after the script
    // that caused it. Firing it needs that queue, which this engine has no
    // mechanism for.
    //
    // What it costs today, measured: SetCopper(frame, 12345) changes all three
    // boxes and leaves expectChanges at 3, and nothing ever decrements it. The
    // next two edits the player makes to that money frame take the swallow
    // branch - onValueChangedFunc does not run, so the amount is set and the
    // total beside it does not move until the third keystroke. Reachable from
    // the send-mail money box, the trade money box, and the money popups.
    return 0;
}

int lua_EditBox_GetText(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushstring(L, w ? w->editText.c_str() : "");
    return 1;
}

/// SetNumber(n) - the counterpart to GetNumber, which reads the same field.
///
/// Absent, so it fell to the no-op catch-all and the box kept whatever it had.
/// MoneyInputFrame_SetCopper is built on this: it compares GetNumber against
/// the value it wants and calls SetNumber where they differ, so with the
/// setter doing nothing the comparison stayed false forever. Opening a trade
/// runs MoneyInputFrame_SetCopper(TradePlayerInputMoneyFrame, 0) to clear the
/// three fields, and they held the last amount instead.
///
/// Whole numbers are written without a decimal tail: these are copper, silver
/// and gold counts, and a money box does not show "12.000000".
int lua_EditBox_SetNumber(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const double value = luaL_optnumber(L, 2, 0.0);
    char buf[32];
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    } else {
        snprintf(buf, sizeof(buf), "%g", value);
    }
    w->editText = buf;
    w->cursorPos = w->editText.size();
    queueTextChanged(L, 1);
    // Deferred, not fired here, for the reason written out at SetText above: WoW fires
    // one and defers it, and firing it synchronously breaks the caller that
    // needs it most.
    return 0;
}

/// AddHistoryLine(text) - remember a line that was sent from this box.
///
/// FrameXML calls this from every place a chat line is submitted and then
/// leaves the walking of it to the client, which is why nothing happened:
/// the method fell to the no-op catch-all, so the history was always empty
/// and the up arrow had nothing to recall.
///
/// A repeat of the line already at the top is not stored again - sending the
/// same thing twice should not need two presses to step past - and the list is
/// capped the way WoW's is, oldest dropped first.
int lua_EditBox_SetHistoryLines(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    w->editHistoryLines = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (static_cast<int>(w->editHistory.size()) > w->editHistoryLines) {
        w->editHistory.resize(static_cast<size_t>(std::max(0, w->editHistoryLines)));
    }
    return 0;
}

/// ClearHistory() - forget what has been typed into this box.
///
/// The chat window does it twice: when a window is closed and reused, and when
/// a temporary one is opened for a whisper. Both are the same intent - the box
/// is being handed to a different conversation, and stepping up through the
/// arrow keys should not walk into the previous one's lines.
///
/// The cap is left alone, since it is a property of the box rather than of
/// what is in it.
int lua_EditBox_ClearHistory(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w || !w->isEditBox) return 0;
    w->editHistory.clear();
    w->editHistoryPos = -1;
    return 0;
}

/// SetCountInvisibleLetters(flag) - charge the markup against the limit.
int lua_EditBox_SetCountInvisibleLetters(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->countInvisibleLetters = lua_isnone(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    }
    return 0;
}

int lua_EditBox_GetCountInvisibleLetters(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushboolean(L, (w && w->countInvisibleLetters) ? 1 : 0);
    return 1;
}

int lua_EditBox_GetHistoryLines(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->editHistoryLines : 0);
    return 1;
}

int lua_EditBox_SetIgnoreArrows(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (w) w->editIgnoreArrows = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_EditBox_AddHistoryLine(lua_State* L) {
    auto* w = widgetOf(L, 1);
    const char* text = luaL_optstring(L, 2, "");
    if (!w || !text || !*text) return 0;
    // What the box was declared to keep. Several ask for none outright - the
    // money fields, the mail recipient - and a hardcoded number here kept
    // history in boxes that said not to.
    if (w->editHistoryLines <= 0) return 0;
    if (w->editHistory.empty() || w->editHistory.back() != text) {
        w->editHistory.emplace_back(text);
        while (static_cast<int>(w->editHistory.size()) > w->editHistoryLines) {
            w->editHistory.erase(w->editHistory.begin());
        }
    }
    w->editHistoryPos = -1;
    return 0;
}

int lua_EditBox_GetNumber(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? std::atof(w->editText.c_str()) : 0.0);
    return 1;
}

int lua_EditBox_Insert(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const std::string add = luaL_optstring(L, 2, "");
    const size_t at = std::min(w->cursorPos, w->editText.size());
    w->editText.insert(at, add);
    w->cursorPos = at + add.size();
    return 0;
}

int lua_MessageFrame_SetPadding(lua_State* L) {
    // WoW takes a horizontal and a vertical padding; only the vertical one
    // changes anything here, because message lines are drawn from the frame's
    // left edge rather than inset.
    if (auto* w = widgetOf(L, 1)) {
        w->messagePadding = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    }
    return 0;
}

int lua_MessageFrame_GetPadding(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, 0.0);
    lua_pushnumber(L, w ? w->messagePadding : 0.0);
    return 2;
}

int lua_EditBox_SetTextInsets(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->textInsetLeft   = static_cast<float>(luaL_optnumber(L, 2, 0.0));
        w->textInsetRight  = static_cast<float>(luaL_optnumber(L, 3, 0.0));
        w->textInsetTop    = static_cast<float>(luaL_optnumber(L, 4, 0.0));
        w->textInsetBottom = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    }
    return 0;
}

int lua_EditBox_GetTextInsets(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->textInsetLeft   : 0.0);
    lua_pushnumber(L, w ? w->textInsetRight  : 0.0);
    lua_pushnumber(L, w ? w->textInsetTop    : 0.0);
    lua_pushnumber(L, w ? w->textInsetBottom : 0.0);
    return 4;
}

int lua_EditBox_SetMaxLetters(lua_State* L) {
    if (auto* w = widgetOf(L, 1))
        w->editMaxLetters = static_cast<int>(luaL_optnumber(L, 2, 0));
    return 0;
}

int lua_EditBox_SetNumeric(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->editNumeric = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_EditBox_SetAutoFocus(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->editAutoFocus = lua_toboolean(L, 2) != 0;
    return 0;
}

int lua_EditBox_SetMultiLine(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) w->editMultiLine = lua_toboolean(L, 2) != 0;
    return 0;
}

/// HighlightText([start, stop]) - select a run, or all of it.
///
/// The one caller that matters is autocomplete: it writes the completed name
/// with SetText and highlights the part it added, so the next character typed
/// replaces the completion instead of landing after it. With this a no-op,
/// typing "Thr" into a whisper completed to "Thrall" and the next keystroke
/// made "Thralla".
///
/// No arguments means all of it, which is what WoW does and what the chat
/// edit box relies on when it takes focus.
int lua_EditBox_HighlightText(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w || !w->isEditBox) return 0;
    const size_t len = w->editText.size();
    size_t a = 0, b = len;
    if (lua_isnumber(L, 2)) a = static_cast<size_t>(std::max(0.0, lua_tonumber(L, 2)));
    if (lua_isnumber(L, 3)) b = static_cast<size_t>(std::max(0.0, lua_tonumber(L, 3)));
    if (a > len) a = len;
    if (b > len) b = len;
    if (a > b) std::swap(a, b);
    w->selStart = a;
    w->selEnd = b;
    // An empty run is no selection: FrameXML calls this with equal offsets to
    // clear one, and a zero-width highlight that still counted would eat the
    // next character typed.
    w->hasSelection = (b > a);
    return 0;
}

int lua_EditBox_SetCursorPosition(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    const double at = luaL_optnumber(L, 2, 0.0);
    // Snapped to a place the caret can be. Stepping keeps it on a boundary and
    // erasing removes exactly one step, so a caret dropped inside a link by a
    // number from Lua is the one way left to split an escape.
    w->cursorPos = ui::caretSnap(w->editText, static_cast<size_t>(std::clamp(
        at, 0.0, static_cast<double>(w->editText.size()))));
    // Moving the caret ends the selection, as it does in any text field. A
    // selection left behind would swallow the next character typed, somewhere
    // far from whatever set it.
    w->hasSelection = false;
    return 0;
}

int lua_EditBox_GetCursorPosition(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? static_cast<double>(w->cursorPos) : 0.0);
    return 1;
}

/// GetNumLetters() - how many characters the box holds, not how many bytes.
///
/// The macro editor writes its "%d/255 Characters Used" counter from this on
/// every keystroke. Unanswered it was nil, and a nil through string.format is
/// the counter reading back its own format string.
int lua_EditBox_GetNumLetters(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (!w) { lua_pushnumber(L, 0); return 1; }
    int chars = 0;
    for (char c : w->editText) {
        // A continuation byte is 10xxxxxx; every other byte opens a character.
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++chars;
    }
    lua_pushnumber(L, chars);
    return 1;
}

/// How many *characters* precede the cursor, where GetCursorPosition counts
/// bytes. The two agree for plain ASCII and diverge the moment anything
/// accented is typed, which is why the interface asks for this one by name
/// wherever it is doing arithmetic on a position.
///
/// It was in the no-op method list, so it answered nil - and autocomplete.lua
/// writes `self:GetUTF8CursorPosition() - strlenutf8(command) - 1`, which
/// raises on nil rather than misbehaving. Typing a slash command or a player
/// name took the chat frame's autocomplete down with it.
int lua_EditBox_GetUTF8CursorPosition(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    if (!w) { lua_pushnumber(L, 0); return 1; }
    const size_t upTo = std::min(w->cursorPos, w->editText.size());
    int chars = 0;
    for (size_t i = 0; i < upTo; ++i) {
        // A continuation byte is 10xxxxxx; every other byte opens a character.
        if ((static_cast<unsigned char>(w->editText[i]) & 0xC0) != 0x80) ++chars;
    }
    lua_pushnumber(L, chars);
    return 1;
}

int lua_Cooldown_SetCooldown(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->cooldownStart = luaL_optnumber(L, 2, 0.0);
        w->cooldownDuration = luaL_optnumber(L, 3, 0.0);
    }
    return 0;
}

int lua_Cooldown_GetCooldownTimes(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    // Milliseconds, which is what this one answers in.
    lua_pushnumber(L, w ? w->cooldownStart * 1000.0 : 0.0);
    lua_pushnumber(L, w ? w->cooldownDuration * 1000.0 : 0.0);
    return 2;
}


int lua_Slider_SetValueStep(lua_State* L) {
    if (auto* w = widgetOf(L, 1))
        w->sliderStep = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    return 0;
}

int lua_Slider_GetValueStep(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->sliderStep : 0.0);
    return 1;
}

/// The draggable part. Given a path rather than a texture here, the same way
/// the button art setters take one.
/// The four ColorSelect region setters, which do nothing but say what a region
/// is for. The regions themselves are ordinary textures with ordinary anchors;
/// what makes one a colour wheel is that the renderer draws every hue into it,
/// and what makes one a thumb is that it is moved to wherever the current
/// colour sits. Neither can be told from the XML alone - <ColorWheelTexture>
/// carries no file, because the art is generated.
static int setColorRole(lua_State* L, wowee::ui::Widget::ColorRole role) {
    auto* tree = wowee::addons::getWidgetTree(L);
    if (!tree) return 0;
    if (auto* region = tree->get(widgetIdOf(L, 2))) region->colorRole = role;
    return 0;
}

int lua_ColorSelect_SetWheelTexture(lua_State* L) {
    return setColorRole(L, wowee::ui::Widget::ColorRole::Wheel);
}

int lua_ColorSelect_SetWheelThumbTexture(lua_State* L) {
    return setColorRole(L, wowee::ui::Widget::ColorRole::WheelThumb);
}

int lua_ColorSelect_SetValueTexture(lua_State* L) {
    return setColorRole(L, wowee::ui::Widget::ColorRole::Value);
}

int lua_ColorSelect_SetValueThumbTexture(lua_State* L) {
    return setColorRole(L, wowee::ui::Widget::ColorRole::ValueThumb);
}

int lua_Slider_SetThumbTexture(lua_State* L) {
    auto* w = widgetOf(L, 1);
    if (!w) return 0;
    if (lua_isstring(L, 2)) {
        w->thumbTexture = lua_tostring(L, 2);
    } else if (lua_istable(L, 2)) {
        auto* tree = wowee::addons::getWidgetTree(L);
        const uint32_t tid = widgetIdOf(L, 2);
        const auto* t = tree ? tree->get(tid) : nullptr;
        if (t) {
            w->thumbTexture = t->texturePath;
            // Kept as a region too, not just a file path. It is the thing
            // FrameXML shows and hides by name - ScrollFrame_OnScrollRangeChanged
            // hides `<bar>ThumbTexture` when a list fits - and the thing the
            // layout has to move along the track, since a <ThumbTexture>
            // carries a size and no anchors and would otherwise sit centred on
            // the bar forever.
            w->thumbRegion = tid;
        }
    }
    return 0;
}

int lua_StatusBar_GetValue(lua_State* L) {
    const auto* w = widgetOf(L, 1);
    lua_pushnumber(L, w ? w->barValue : 0.0);
    return 1;
}

int lua_StatusBar_SetStatusBarTexture(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        // Takes a path or an existing texture object, and addons use both.
        if (lua_isstring(L, 2)) w->barTexture = lua_tostring(L, 2);
        else if (lua_istable(L, 2)) {
            if (auto* tex = widgetOf(L, 2)) w->barTexture = tex->texturePath;
        }
    }
    return 0;
}

int lua_StatusBar_SetStatusBarColor(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        w->barColor[0] = static_cast<float>(luaL_optnumber(L, 2, 1.0));
        w->barColor[1] = static_cast<float>(luaL_optnumber(L, 3, 1.0));
        w->barColor[2] = static_cast<float>(luaL_optnumber(L, 4, 1.0));
        w->barColor[3] = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    }
    return 0;
}

int lua_StatusBar_SetOrientation(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) {
        const std::string o = luaL_optstring(L, 2, "HORIZONTAL");
        w->barVertical = (o == "VERTICAL");
    }
    return 0;
}

// Frame method: frame:CreateTexture(name, layer) → a real region
static int lua_Frame_CreateTexture(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t parent = widgetIdOf(L, 1);
    const char* name = luaL_optstring(L, 2, "");
    const char* layer = luaL_optstring(L, 3, "ARTWORK");

    lua_newtable(L);
    if (tree) {
        const uint32_t id = tree->create(wowee::ui::WidgetKind::Texture, parent, name ? name : "");
        if (auto* w = tree->get(id)) {
            w->layer = wowee::ui::parseDrawLayer(layer);
            w->objectType = "Texture";
        }
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        lua_setfield(L, -2, "__wid");
    }
    // A region's parent, which is the frame it was created on.
    //
    // This was never recorded, so GetParent() answered nil for every texture
    // and every font string in the interface - CreateFrame writes __parent and
    // these two did not. It is not a rare call: the calendar reaches a day's
    // shading through `darkTop:GetParent()`, which is how the gap surfaced,
    // and a region asking its owner for a size or a frame level is an ordinary
    // shape in FrameXML.
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "__parent");
    installRegionMethods(L, /*isTexture=*/true, /*isFontString=*/false);
    if (name && *name) {
        lua_pushvalue(L, -1);
        lua_setglobal(L, name);
    }
    return 1;
}

// Frame method: frame:CreateFontString(name, layer, template) → a real region
static int lua_Frame_CreateFontString(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const uint32_t parent = widgetIdOf(L, 1);
    const char* name = luaL_optstring(L, 2, "");
    const char* layer = luaL_optstring(L, 3, "ARTWORK");

    lua_newtable(L);
    if (tree) {
        const uint32_t id = tree->create(wowee::ui::WidgetKind::FontString, parent, name ? name : "");
        if (auto* w = tree->get(id)) {
            w->layer = wowee::ui::parseDrawLayer(layer);
            w->objectType = "FontString";
        }
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        lua_setfield(L, -2, "__wid");
    }
    // A region's parent, which is the frame it was created on.
    //
    // This was never recorded, so GetParent() answered nil for every texture
    // and every font string in the interface - CreateFrame writes __parent and
    // these two did not. It is not a rare call: the calendar reaches a day's
    // shading through `darkTop:GetParent()`, which is how the gap surfaced,
    // and a region asking its owner for a size or a frame level is an ordinary
    // shape in FrameXML.
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "__parent");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "_text");
    installRegionMethods(L, /*isTexture=*/false, /*isFontString=*/true);
    if (name && *name) {
        lua_pushvalue(L, -1);
        lua_setglobal(L, name);
    }
    return 1;
}

static int lua_GetFramerate(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(ImGui::GetIO().Framerate));
    return 1;
}

// GetCursorPosition() → x, y - pixels, measured from the BOTTOM left.
//
// Pixels rather than interface units is right, and deliberate: every caller
// divides by UIParent:GetScale() itself. channelframe.lua does exactly that on
// the line after asking, and then anchors what it dragged to BOTTOMLEFT - which
// is the half that was wrong. ImGui measures the cursor from the top, so y came
// back mirrored and anything positioned from it landed as far from the bottom
// as the cursor was from the top.
static int lua_GetCursorPosition(lua_State* L) {
    // Interface units with y growing upward - the same conversion the input
    // path and GetMouseFocus make, and the space every frame coordinate is in.
    //
    // This alone answered in raw pixels. Callers divide by GetEffectiveScale
    // and then use the result as a frame position, and effective scale at the
    // root is one, so the pixels went straight through: on a 1080-tall window
    // every such position was out by forty percent, and on a 4K one by nearly
    // three times. The loot window opened under the mouse landed well off it.
    // Through ui::mouseToTreeSpace, which is the one definition of it. This
    // was a third hand-written copy - the flip in application.cpp, the scale
    // in dispatchMouse, and both again here - and the copies are how the
    // hyperlink hit test came to be filed in one space and tested in another.
    const auto& io = ImGui::GetIO();
    auto* tree = wowee::addons::getWidgetTree(L);
    // Somewhere on the screen, always. ImGui reports -FLT_MAX for both axes
    // when it has no cursor to report - the window unfocused, or nothing moved
    // yet - and that went out as an answer. Callers do arithmetic on this and
    // hand the result to SetPoint: lootframe.lua subtracts 175 from it and
    // anchors the loot window there, so a loot opened at that moment was
    // positioned at infinity and simply not on screen. The middle is the
    // honest answer to "where is the cursor" when there is no cursor, and it
    // is the one place anything positioned from it stays visible.
    float px = io.MousePos.x;
    float py = io.MousePos.y;
    if (!ImGui::IsMousePosValid(&io.MousePos)) {
        px = io.DisplaySize.x * 0.5f;
        py = io.DisplaySize.y * 0.5f;
    }
    ui::mouseToTreeSpace(px, py, io.DisplaySize.y, tree ? tree->uiScale() : 1.0f);
    lua_pushnumber(L, px);
    lua_pushnumber(L, py);
    return 2;
}

// GetScreenWidth() → width
/// The screen in interface units, not pixels - which is what FrameXML means
/// by it. On a 1528-tall window GetScreenHeight() is 768, the same as it would
/// be on any other, and a frame sized against it comes out the same size.
static int lua_GetScreenWidth(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const auto* root = tree ? tree->get(tree->rootId()) : nullptr;
    if (root && root->rectW > 0.0f) { lua_pushnumber(L, root->rectW); return 1; }
    auto* svc = getLuaServices(L);
    auto* window = svc ? svc->window : nullptr;
    lua_pushnumber(L, window ? window->getWidth() : 1920);
    return 1;
}

// GetScreenHeight() → height
static int lua_GetScreenHeight(lua_State* L) {
    auto* tree = wowee::addons::getWidgetTree(L);
    const auto* root = tree ? tree->get(tree->rootId()) : nullptr;
    if (root && root->rectH > 0.0f) { lua_pushnumber(L, root->rectH); return 1; }
    auto* svc = getLuaServices(L);
    auto* window = svc ? svc->window : nullptr;
    lua_pushnumber(L, window ? window->getHeight() : 1080);
    return 1;
}

// CreateFrame(frameType, name, parent, template)
static int lua_CreateFrame(lua_State* L) {
    const char* frameType = luaL_optstring(L, 1, "Frame");
    const char* name = luaL_optstring(L, 2, nullptr);
    // Which of the per-type methods go on this frame; see where they are set,
    // below the metatable.
    bool createdStatusBar = false;
    bool createdSlider = false;

    // Create the frame table
    lua_newtable(L);

    // Record the parent table, not only the widget id. GetParent() is
    // everywhere in FrameXML - a nested button's OnLoad opens with
    // self:GetParent().toggle = self - and it answered nil for every frame
    // ever created, because only an explicit SetParent recorded one. That
    // failed the template declaring the button, so the button's owner never
    // got its size, and the loop sizing a list by its first button's height
    // divided by zero.
    if (lua_istable(L, 3)) {
        lua_pushvalue(L, 3);
        lua_setfield(L, -2, "__parent");
    } else if (lua_gettop(L) >= 3 && lua_isnil(L, 3) && !lua_isstring(L, 3)) {
        // An explicit nil is a frame with no parent, which is a real thing in
        // WoW and the only way anything survives UIParent:Hide(). GetParent()
        // on one answers nil, so no __parent is written.
    } else {
        // A name, or the argument left off entirely, which means UIParent -
        // what an addon means by omitting it.
        if (lua_isstring(L, 3)) lua_getglobal(L, lua_tostring(L, 3));
        else lua_getglobal(L, "UIParent");
        if (lua_istable(L, -1)) lua_setfield(L, -2, "__parent");
        else lua_pop(L, 1);
    }

    // Back it with a real widget so its geometry is somewhere the renderer can
    // reach. Parent is the third argument when given, and UIParent otherwise,
    // which is what an addon means by leaving it out.
    if (auto* tree = wowee::addons::getWidgetTree(L)) {
        uint32_t parent = 0;
        if (lua_istable(L, 3)) {
            parent = widgetIdOf(L, 3);
        } else if (lua_isstring(L, 3)) {
            lua_getglobal(L, lua_tostring(L, 3));
            if (lua_istable(L, -1)) parent = widgetIdOf(L, lua_gettop(L));
            lua_pop(L, 1);
        } else if (!(lua_gettop(L) >= 3 && lua_isnil(L, 3))) {
            // Omitted, not nil: UIParent, and named here so the widget agrees
            // with the __parent written above. Zero would hang it off the
            // screen instead, which is what an explicit nil asks for.
            //
            // Through the global rather than the tree's own id, because
            // uiparent.xml declares UIParent like any other frame and nothing
            // deduplicates by name - so the widget FrameXML's UIParent points
            // at is the one that file made, and the tree's is the placeholder
            // that stood in before any of it was read. Fall back to that one
            // for anything created before it loads.
            lua_getglobal(L, "UIParent");
            if (lua_istable(L, -1)) parent = widgetIdOf(L, lua_gettop(L));
            lua_pop(L, 1);
            if (parent == 0) parent = tree->uiParentId();
        }
        const uint32_t id = tree->create(wowee::ui::WidgetKind::Frame, parent,
                                         name ? name : "");
        // A Button takes the mouse without being asked; a plain Frame does not,
        // which is what EnableMouse is for.
        if (auto* w = tree->get(id)) {
            const std::string ft = frameType ? frameType : "Frame";
            // Kept as it was asked for, so GetObjectType and IsObjectType
            // can answer with it rather than with "Frame" for everything.
            w->objectType = ft;
            w->mouseEnabled = (ft == "Button" || ft == "CheckButton");
            w->isStatusBar = (ft == "StatusBar");
            createdStatusBar = w->isStatusBar;
            createdSlider = (ft == "Slider");
            // A slider takes the mouse by nature: it exists to be dragged.
            w->isSlider = (ft == "Slider");
            // And it runs top to bottom unless it says otherwise. A Slider's
            // orientation defaults to VERTICAL in WoW, the opposite way round
            // from a StatusBar - which is why UIPanelScrollBarTemplate declares
            // no orientation at all while OptionsSliderTemplate, the horizontal
            // one, spells out HORIZONTAL. Every scroll bar in the interface is
            // built from that template, so defaulting them to horizontal read
            // the cursor's x across a bar 16 units wide: dragging up and down
            // moved nothing, and the grip had a negative span to travel. An
            // explicit SetOrientation from the XML still overrides this, since
            // the emitter writes it after the frame is created.
            if (w->isSlider) w->barVertical = true;
            w->isCooldown = (ft == "Cooldown");
            // Marked at creation rather than only when a child is set, so a
            // scroll frame clips what is under it even while it is empty.
            if (ft == "ScrollFrame") tree->markScrollFrame(id);
            // An edit box is clicked into, so it takes the mouse as well.
            w->isEditBox = (ft == "EditBox");
            if (w->isEditBox) {
                w->mouseEnabled = true;
                lua_pushboolean(L, 1);
                lua_setfield(L, -2, "__isEditBox");
            }
            if (w->isSlider) w->mouseEnabled = true;
        }
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        lua_setfield(L, -2, "__wid");

        // Remember the table against its widget id so input dispatch can get
        // back from a hit to the frame whose scripts must run.
        lua_getglobal(L, "__WoweeFramesByWid");
        if (lua_istable(L, -1)) {
            lua_pushinteger(L, static_cast<lua_Integer>(id));
            lua_pushvalue(L, -3);
            lua_rawset(L, -3);
        }
        lua_pop(L, 1);
    }

    // Set frame name
    if (name && *name) {
        lua_pushstring(L, name);
        lua_setfield(L, -2, "__name");
        // Also set as a global so other addons can find it by name
        lua_pushvalue(L, -1);
        lua_setglobal(L, name);
    }

    // The fifth argument is the frame's id, and it has to be in place before
    // the templates and the OnLoad run - they are what read it. A frame built
    // this way is usually one of a numbered set, and its handler finds the rest
    // of the set through its own id: the temporary chat window opens with
    // _G["ChatFrame"..self:GetID()], so an id of zero has it looking up a
    // window that does not exist and indexing nothing.
    if (lua_isnumber(L, 5)) {
        lua_pushnumber(L, lua_tonumber(L, 5));
        lua_setfield(L, -2, "__id");
    }

    // Set initial visibility
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__visible");

    // Apply frame metatable with methods
    lua_getglobal(L, "__WoweeFrameMT");
    lua_setmetatable(L, -2);

    // GetCurrentValue and SetDisplayValue belong to a StatusBar, and are put on
    // the frame itself rather than on the metatable every frame shares, because
    // FrameXML asks whether they exist and means it.
    //
    // BlizzardOptionsPanel_Slider_Refresh reads a slider's value with
    // `if ( slider.GetCurrentValue ) then ... elseif ( slider.cvar ) then` -
    // the first branch is for the handful of controls that define a closure of
    // that name themselves, and every other slider is meant to fall through to
    // its CVar. With the pair on the shared table the test never failed, so
    // every options slider read a status bar's value, which is zero, and wrote
    // that back when the panel closed. Opening Video Options and closing it set
    // the UI scale to the smallest the range allows.
    if (createdStatusBar) {
        lua_pushcfunction(L, lua_StatusBar_GetCurrentValue);
        lua_setfield(L, -2, "GetCurrentValue");
        lua_pushcfunction(L, lua_StatusBar_SetDisplayValue);
        lua_setfield(L, -2, "SetDisplayValue");
    } else if (createdSlider) {
        // A slider gets SetDisplayValue and not GetCurrentValue. Setting the
        // displayed value is what the graphics-quality presets do to move a
        // slider without writing its CVar, and for a slider that is simply
        // SetValue - the display and the value are the same number. Leaving
        // GetCurrentValue off is the point: the refresh reads a slider's
        // CVar only when it is absent.
        lua_pushcfunction(L, lua_StatusBar_SetValue);
        lua_setfield(L, -2, "SetDisplayValue");
    }

    // The fourth argument names a template, which FrameXML uses constantly:
    // CreateFrame("BUTTON", name, self, "OptionsListButtonTemplate"). Ignoring
    // it was not merely a missing feature. OptionsList_OnLoad makes one button,
    // divides the list's height by that button's height to decide how many fit,
    // and loops to that number - so a template that never arrives means no
    // size, a height of zero, a count of (h-8)/0, and Lua divides by zero
    // happily. The loop then creates frames under fresh names until memory runs
    // out, which is exactly what froze the client on VideoOptionsFrame.
    //
    // Applied after the metatable, so the template's body can call methods on
    // what it is given.
    if (const char* templates = lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr) {
        const std::string list(templates);
        size_t start = 0;
        while (start <= list.size()) {
            const size_t comma = list.find(',', start);
            std::string one = list.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            const size_t b = one.find_first_not_of(" \t");
            const size_t e = one.find_last_not_of(" \t");
            one = (b == std::string::npos) ? std::string() : one.substr(b, e - b + 1);

            if (!one.empty()) {
                lua_getglobal(L, "__WoweeTemplates");
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, one.c_str());
                    if (lua_isfunction(L, -1)) {
                        lua_pushvalue(L, -3);            // the frame
                        if (lua_pcall(L, 1, 0, 0) != 0) {
                            // Once per template. A template that fails fails
                            // for every frame using it, and the loop this very
                            // failure causes then repeats it: one run wrote the
                            // same line 675,000 times, which cost more than the
                            // fault it was reporting.
                            static std::set<std::string> reported;
                            if (reported.insert(one).second) {
                                const char* terr = lua_tostring(L, -1);
                                LOG_WARNING("CreateFrame: template '", one, "' failed: ",
                                            terr ? terr : "?");
                                // Recorded as well as logged. A template that
                                // does not apply leaves every frame using it
                                // built but unconfigured - no size, no
                                // scripts, no children - which is the exact
                                // shape of "the panel is there and does
                                // nothing", and it belongs in the file a
                                // player can send rather than only in a log
                                // nobody captured.
                                if (auto* e = engineFrom(L)) {
                                    e->noteLuaError("template '" + one + "' failed: " +
                                                    (terr ? terr : "?"));
                                }
                            }
                            lua_pop(L, 1);               // error
                        }
                    } else {
                        // A template this interface does not define. The XML
                        // path says so - the emitter writes an else branch
                        // calling __WoweeMissingTemplate - and this path, the
                        // one CreateFrame takes at runtime, said nothing at
                        // all: the frame came back built, unconfigured and
                        // without the children the template declares, and the
                        // first thing to read one of those children by name
                        // died on a nil far from here.
                        //
                        // That is how the client's own options panel is lost
                        // on a 1.12 interface. It asks for
                        // InterfaceOptionsCheckButtonTemplate, which is
                        // WotLK's; vanilla loads no OptionsPanelTemplates.xml
                        // at all, so nothing applied, _G[name .. "Text"] was
                        // never created, and the whole category failed to
                        // register with one Lua error naming a line in a
                        // string.
                        //
                        // Once per name: a template that is missing is missing
                        // for every frame that inherits it, and one of those
                        // loops wrote the same line 675,000 times.
                        static std::set<std::string> missingReported;
                        if (missingReported.insert(one).second) {
                            LOG_WARNING("CreateFrame: template '", one,
                                        "' is not defined by this interface - the "
                                        "frame is built without it, and anything "
                                        "reading a child it declares will find "
                                        "nothing");
                        }
                        lua_pop(L, 1);                   // not a function
                    }
                }
                lua_pop(L, 1);                           // __WoweeTemplates
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }

        // Built from a template here, so it is loaded here - which is what
        // CreateFrame does in the real client. The XML path does not pass a
        // template to this function; it applies them separately and fires
        // OnLoad once, after the frame's own body. So this covers exactly the
        // frames Lua builds, and OptionsList_OnLoad builds a list of them:
        // their OnLoad is what gives each button the .text it is asked for
        // moments later.
        lua_getfield(L, -1, "__scripts");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "OnLoad");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -3);            // the frame
                if (pcallScript(L, "OnLoad", 1, 0) != 0) {
                    const char* oerr = lua_tostring(L, -1);
                    LOG_WARNING("CreateFrame: OnLoad failed: ", oerr ? oerr : "?");
                    // Same weight as a template that will not apply: a frame
                    // whose OnLoad stopped part way is built and half
                    // configured, and everything the rest of that handler
                    // would have done simply did not happen.
                    if (auto* e = engineFrom(L))
                        e->noteLuaError(std::string("OnLoad failed: ") + (oerr ? oerr : "?"));
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    return 1;
}

}  // namespace

// External linkage, as it was before this file existed: nothing in the
// client calls it, and inside the anonymous namespace above that reads as
// dead code rather than as the binding it is.
int lua_Cooldown_Clear(lua_State* L) {
    if (auto* w = widgetOf(L, 1)) { w->cooldownStart = 0.0; w->cooldownDuration = 0.0; }
    return 0;
}


const struct luaL_Reg frameMethods[] = {
    {"RegisterEvent",   lua_Frame_RegisterEvent},
    {"IsEventRegistered", lua_Frame_IsEventRegistered},
    {"UnregisterEvent", lua_Frame_UnregisterEvent},
    {"UnregisterAllEvents", lua_Frame_UnregisterAllEvents},
    {"SetScript",       lua_Frame_SetScript},
    {"GetScript",       lua_Frame_GetScript},
    {"GetName",         lua_Frame_GetName},
    {"Show",            lua_Region_Show},
    {"Hide",            lua_Region_Hide},
    {"IsShown",         lua_Region_IsShown},
    {"IsVisible",       lua_Region_IsVisible},
    // Geometry goes through the widget tree. The older table-field
    // versions kept the numbers where only Lua could see them, which is
    // why a frame could be sized and positioned and still never appear.
    {"SetPoint",        lua_Region_SetPoint},
    // A frame method too, and not only a region one: <StatusBar
    // drawLayer="BORDER"> is emitted as a call on the bar, and a bar is a
    // frame. Absent here it fell to the no-op fallback, so all twenty of
    // the bars that declare a layer were silently ignored - the trace
    // under WOWEE_WIDGET_TRACE=1 is what named them.
    {"SetDrawLayer",    lua_Region_SetDrawLayer},
    {"ClearAllPoints",  lua_Region_ClearAllPoints},
    {"SetAllPoints",    lua_Region_SetAllPoints},
    {"SetSize",         lua_Region_SetSize},
    {"SetWidth",        lua_Region_SetWidth},
    {"SetHeight",       lua_Region_SetHeight},
    // Frames, not regions: only a frame is dragged or moved. These live in
    // this table rather than the shared region one because that one is
    // installed on textures and font strings alone - putting them there
    // gave the methods to everything except the things that use them.
    {"SetMovable",      lua_Frame_SetMovable},
    {"SetRotation",     lua_Model_SetFacing},
    {"SetFacing",       lua_Model_SetFacing},
    {"GetFacing",       lua_Model_GetFacing},
    {"IsMovable",       lua_Frame_IsMovable},
    {"RegisterForDrag", lua_Frame_RegisterForDrag},
    {"StartMoving",     lua_Frame_StartMoving},
    {"StopMovingOrSizing", lua_Frame_StopMovingOrSizing},
    {"StartSizing",     lua_Frame_StartSizing},
    {"SetResizable",    lua_Frame_SetResizable},
    {"IsResizable",     lua_Frame_IsResizable},
    {"SetMinResize",    lua_Frame_SetMinResize},
    {"SetMaxResize",    lua_Frame_SetMaxResize},
    {"GetWidth",        lua_Region_GetWidth},
    {"SetScale",        lua_Region_SetScale},
    {"GetScale",        lua_Region_GetScale},
    {"GetEffectiveScale", lua_Region_GetEffectiveScale},
    {"GetTextWidth",    lua_Region_GetTextWidth},
    {"GetStringWidth",  lua_Region_GetTextWidth},
    {"GetTextHeight",   lua_Region_GetTextHeight},
    {"GetStringHeight", lua_Region_GetTextHeight},
    {"GetFieldSize",    lua_Region_GetFieldSize},
    {"GetVertexColor",  lua_Region_GetVertexColor},
    {"GetInputLanguage", lua_EditBox_GetInputLanguage},
    {"SetColorRGB",     lua_ColorSelect_SetColorRGB},
    {"GetColorRGB",     lua_ColorSelect_GetColorRGB},
    {"GetFontObject",   lua_FontString_GetFontObject},
    {"SetMinimumWidth", lua_Frame_SetMinimumWidth},
    {"GetMinimumWidth", lua_Frame_GetMinimumWidth},
    {"GetHeight",       lua_Region_GetHeight},
    {"GetLeft",         lua_Region_GetLeft},
    {"GetRight",        lua_Region_GetRight},
    {"GetBottom",       lua_Region_GetBottom},
    {"GetTop",          lua_Region_GetTop},
    {"GetRect",         lua_Region_GetRect},
    {"IsMouseOver",     lua_Region_IsMouseOver},
    {"GetFrameLevel",   lua_Frame_GetFrameLevel},
    {"GetNumPoints",    lua_Region_GetNumPoints},
    {"AddMessage",      lua_MessageFrame_AddMessage},
    {"AddLine",         lua_Tooltip_AddLine},
    {"SetOwner",        lua_Tooltip_SetOwner},
    {"SetAction",       lua_Tooltip_SetAction},
    {"SetInventoryItem", lua_Tooltip_SetInventoryItem},
    {"SetBagItem",      lua_Tooltip_SetBagItem},
    {"SetQuestLogSpecialItem", lua_Tooltip_SetQuestLogSpecialItem},
    {"SetSocketGem",          lua_Tooltip_SetSocketGem},
    {"SetExistingSocketGem",  lua_Tooltip_SetExistingSocketGem},
    {"SetSocketedItem",       lua_Tooltip_SetSocketedItem},
    {"SetCurrencyToken", lua_Tooltip_SetCurrencyToken},
    // Nothing is pinned under the bags - that is a saved choice this
    // client does not keep - so this answers false rather than
    // describing an arbitrary row.
    {"SetBackpackToken", lua_Tooltip_ReturnFalse},
    {"SetGuildBankItem", lua_Tooltip_SetGuildBankItem},
    {"SetSpellByID",    lua_Tooltip_SetSpellByID},
    {"SetQuestLogRewardSpell", lua_Tooltip_SetQuestLogRewardSpell},
    {"SetEquipmentSet",        lua_Tooltip_SetEquipmentSet},
    {"SetLFGCompletionReward", lua_Tooltip_SetLFGCompletionReward},
    {"SetGlyph",               lua_Tooltip_SetGlyph},
    {"SetHyperlink",    lua_Tooltip_SetHyperlink},
    // On frames as well as on font strings, where these were already
    // registered. A chat frame is asked for its own font - not a label's -
    // by FCF_SetChatWindowFontSize, which reads the face and flags off the
    // frame, puts the chosen size between them and sets it back. With the
    // pair answering only on font strings, that read fell through to the
    // no-op, the size went nowhere, and the font-size menu did nothing.
    // A message frame draws its lines at the widget's own fontHeight, so
    // setting it is all that was missing.
    {"GetFont",         lua_FontString_GetFont},
    {"SetFont",         lua_FontString_SetFont},
    {"SetTalent",       lua_Tooltip_SetTalent},
    {"SetAuctionItem",  lua_Tooltip_SetAuctionItem},
    {"_WoweeAppendItemEnchants", lua_Tooltip_AppendItemEnchants},
    {"SetTradeSkillItem", lua_Tooltip_SetTradeSkillItem},
    {"SetUnit",         lua_Tooltip_SetUnit},
    {"IsUnit",          lua_Tooltip_IsUnit},
    {"AddDoubleLine",   lua_Tooltip_AddDoubleLine},
    {"ClearLines",      lua_Tooltip_ClearLines},
    {"SetFrameStack",   lua_Tooltip_SetFrameStack},
    {"AppendText",      lua_Tooltip_AppendText},
    {"NumLines",        lua_Tooltip_NumLines},
    {"Clear",           lua_MessageFrame_Clear},
    {"GetNumMessages",  lua_MessageFrame_GetNumMessages},
    {"GetMessageInfo",  lua_MessageFrame_GetMessageInfo},
    {"RemoveMessagesByAccessID", lua_MessageFrame_RemoveMessagesByAccessID},
    {"SetMaxLines",     lua_MessageFrame_SetMaxLines},
    {"SetWordWrap",     lua_FontString_SetWordWrap},
    {"CanWordWrap",     lua_FontString_CanWordWrap},
    {"SetNonSpaceWrap", lua_FontString_SetNonSpaceWrap},
    {"SetReverse",      lua_Cooldown_SetReverse},
    {"SetDrawEdge",     lua_Cooldown_SetDrawEdge},
    {"SetTimeVisible",  lua_MessageFrame_SetTimeVisible},
    {"GetTimeVisible",  lua_MessageFrame_GetTimeVisible},
    {"SetFadeDuration", lua_MessageFrame_SetFadeDuration},
    {"SetInsertMode",   lua_MessageFrame_SetInsertMode},
    {"ScrollUp",        lua_MessageFrame_ScrollUp},
    {"ScrollDown",      lua_MessageFrame_ScrollDown},
    {"ScrollToBottom",  lua_MessageFrame_ScrollToBottom},
    {"Enable",          lua_Button_Enable},
    {"SetChecked",      lua_CheckButton_SetChecked},
    {"SetButtonState",  lua_Button_SetButtonState},
    {"GetButtonState",  lua_Button_GetButtonState},
    {"LockHighlight",   lua_Button_LockHighlight},
    {"UnlockHighlight", lua_Button_UnlockHighlight},
    {"GetChecked",      lua_CheckButton_GetChecked},
    {"Disable",         lua_Button_Disable},
    {"IsEnabled",       lua_Button_IsEnabled},
    {"SetScrollChild",  lua_ScrollFrame_SetScrollChild},
    {"SetVerticalScroll",   lua_ScrollFrame_SetVerticalScroll},
    {"SetHorizontalScroll", lua_ScrollFrame_SetHorizontalScroll},
    {"GetVerticalScroll",   lua_ScrollFrame_GetVerticalScroll},
    {"GetHorizontalScroll", lua_ScrollFrame_GetHorizontalScroll},
    {"GetVerticalScrollRange",   lua_ScrollFrame_GetVerticalScrollRange},
    {"GetHorizontalScrollRange", lua_ScrollFrame_GetHorizontalScrollRange},
    {"GetObjectType",   lua_Region_GetObjectType},
    {"IsObjectType",    lua_Region_IsObjectType},
    {"GetPoint",        lua_Region_GetPoint},
    {"SetZoom",         lua_Minimap_SetZoom},
    {"GetZoom",         lua_Minimap_GetZoom},
    {"GetZoomLevels",   lua_Minimap_GetZoomLevels},
    {"PingLocation",    lua_Minimap_PingLocation},
    {"GetCenter",       lua_Region_GetCenter},
    {"SetAlpha",        lua_Region_SetAlpha},
    {"GetAlpha",        lua_Region_GetAlpha},
    {"EnableMouse",     lua_Frame_EnableMouse},
    {"SetHyperlinksEnabled", lua_Frame_SetHyperlinksEnabled},
    {"SetMotionScriptsWhileDisabled", lua_Frame_SetMotionScriptsWhileDisabled},
    {"GetMotionScriptsWhileDisabled", lua_Frame_GetMotionScriptsWhileDisabled},
    {"IsMouseEnabled",  lua_Frame_IsMouseEnabled},
    {"SetNormalFontObject",   lua_Frame_SetNormalFontObject},
    {"SetTextColor",          lua_FontString_SetTextColor},
    {"SetTextFontObject",     lua_Frame_SetNormalFontObject},
    {"SetHighlightFontObject", lua_Frame_SetHighlightFontObject},
    {"SetDisabledFontObject",  lua_Frame_SetDisabledFontObject},
    {"SetDisabledTextColor",   lua_Frame_SetDisabledTextColor},
    {"SetHighlightTextColor",  lua_Frame_SetHighlightTextColor},
    {"SetPushedTextOffset",   lua_Frame_SetPushedTextOffset},
    {"GetPushedTextOffset",   lua_Frame_GetPushedTextOffset},
    {"SetHitRectInsets",      lua_Frame_SetHitRectInsets},
    {"GetHitRectInsets",      lua_Frame_GetHitRectInsets},
    {"SetUserPlaced",         lua_Frame_SetUserPlaced},
    {"IsUserPlaced",          lua_Frame_IsUserPlaced},
    {"EnableKeyboard",        lua_Frame_EnableKeyboard},
    {"IsKeyboardEnabled",     lua_Frame_IsKeyboardEnabled},
    {"SetPropagateKeyboardInput", lua_Frame_SetPropagateKeyboardInput},
    {"SetToplevel",           lua_Frame_SetToplevel},
    {"IsToplevel",            lua_Frame_IsToplevel},
    {"Raise",                 lua_Frame_Raise},
    {"Lower",                 lua_Frame_Lower},
    {"SetClampedToScreen",    lua_Frame_SetClampedToScreen},
    {"SetClampRectInsets",    lua_Frame_SetClampRectInsets},
    {"IsClampedToScreen",     lua_Frame_IsClampedToScreen},
    {"SetBackdrop",           lua_Frame_SetBackdrop},
    {"SetBackdropColor",      lua_Frame_SetBackdropColor},
    {"SetBackdropBorderColor",lua_Frame_SetBackdropBorderColor},
    {"SetMinMaxValues",       lua_StatusBar_SetMinMaxValues},
    {"GetMinMaxValues",       lua_StatusBar_GetMinMaxValues},
    {"SetValue",              lua_StatusBar_SetValue},
    {"GetValue",              lua_StatusBar_GetValue},
    {"SetStatusBarTexture",   lua_StatusBar_SetStatusBarTexture},
    {"SetStatusBarColor",     lua_StatusBar_SetStatusBarColor},
    {"SetOrientation",        lua_StatusBar_SetOrientation},
    {"SetValueStep",          lua_Slider_SetValueStep},
    {"GetValueStep",          lua_Slider_GetValueStep},
    {"SetThumbTexture",       lua_Slider_SetThumbTexture},
    {"SetColorWheelTexture",      lua_ColorSelect_SetWheelTexture},
    {"SetColorWheelThumbTexture", lua_ColorSelect_SetWheelThumbTexture},
    {"SetColorValueTexture",      lua_ColorSelect_SetValueTexture},
    {"SetColorValueThumbTexture", lua_ColorSelect_SetValueThumbTexture},
    {"SetCooldown",           lua_Cooldown_SetCooldown},
    {"GetNumber",             lua_EditBox_GetNumber},
    {"SetNumber",             lua_EditBox_SetNumber},
    {"AddHistoryLine",        lua_EditBox_AddHistoryLine},
    {"SetHistoryLines",       lua_EditBox_SetHistoryLines},
    {"ClearHistory",          lua_EditBox_ClearHistory},
    {"SetCountInvisibleLetters", lua_EditBox_SetCountInvisibleLetters},
    {"GetCountInvisibleLetters", lua_EditBox_GetCountInvisibleLetters},
    {"GetHistoryLines",       lua_EditBox_GetHistoryLines},
    {"SetIgnoreArrows",       lua_EditBox_SetIgnoreArrows},
    {"Insert",                lua_EditBox_Insert},
    {"SetMaxLetters",         lua_EditBox_SetMaxLetters},        // The limit here is applied against the text's size in bytes, which is
    // what SetMaxBytes asks for; SetMaxLetters is the same field because
    // this counts the same way for both. Reporting it back matters more
    // than the distinction: an edit box that answers nothing for its limit
    // is one FrameXML will not stop typing into.
    {"SetMaxBytes",          lua_EditBox_SetMaxLetters},
    {"SetTextInsets",        lua_EditBox_SetTextInsets},
    {"SetPadding",           lua_MessageFrame_SetPadding},
    {"GetPadding",           lua_MessageFrame_GetPadding},
    {"GetTextInsets",        lua_EditBox_GetTextInsets},
    {"SetNumeric",            lua_EditBox_SetNumeric},
    {"SetMultiLine",          lua_EditBox_SetMultiLine},
    {"SetAutoFocus",          lua_EditBox_SetAutoFocus},
    {"SetCursorPosition",     lua_EditBox_SetCursorPosition},
    {"HighlightText",         lua_EditBox_HighlightText},
    {"GetCursorPosition",     lua_EditBox_GetCursorPosition},
    {"GetNumLetters",         lua_EditBox_GetNumLetters},
    {"GetUTF8CursorPosition", lua_EditBox_GetUTF8CursorPosition},
    {"SetFocus",              lua_EditBox_SetFocus},
    {"ClearFocus",            lua_EditBox_ClearFocus},
    {"HasFocus",              lua_EditBox_HasFocus},
    {"GetCooldownTimes",      lua_Cooldown_GetCooldownTimes},
    {"SetFrameStrata",  lua_Frame_SetFrameStrata},
    {"GetFrameStrata",  lua_Frame_GetFrameStrata},
    {"SetFrameLevel",   lua_Frame_SetFrameLevel},
    {"SetParent",       lua_Region_SetParent},
    {"GetParent",       lua_Region_GetParent},
    {"GetChildren",     lua_Frame_GetChildren},
    {"CreateTexture",   lua_Frame_CreateTexture},
    {"CreateFontString", lua_Frame_CreateFontString},
    {nullptr, nullptr}
};

void applyFrameMethods(lua_State* L) {
    lua_getglobal(L, "__WoweeFrameMT");
    for (const luaL_Reg* r = frameMethods; r->name; r++) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_pop(L, 1);
}

/// The frame metatable and the C methods on it.
///
/// One unit because it is one stack frame: the metatable is created here,
/// stays on the stack while every method is written onto it, and leaves it
/// only at the closing lua_setglobal. Everything after this reaches the
/// same table through the __WoweeFrameMT global instead.
void LuaEngine::registerWidgetMethods() {
    // Frame metatable with methods
    lua_newtable(L_);  // metatable
    lua_pushvalue(L_, -1);
    lua_setfield(L_, -2, "__index"); // metatable.__index = metatable

    for (const luaL_Reg* r = frameMethods; r->name; r++) {
        lua_pushcfunction(L_, r->func);
        lua_setfield(L_, -2, r->name);
    }
    lua_setglobal(L_, "__WoweeFrameMT");
}

/// The Lua half of the widget surface: no-op methods, the animation system,
/// button art, and the catch-all that answers an unimplemented method.
///
/// Written in Lua rather than C because it is bookkeeping - an animation
/// group is a list and a timer - and because one loop there covers every
/// button slot without another entry in the C method table.
void LuaEngine::registerWidgetStubLua() {
    // Commonly called frame methods that are no-ops for now, so an addon
    // calling one gets silence rather than an error.
    //
    // Anything bound in C above must not appear here. These run afterwards and
    // simply overwrite it, turning a working method into a no-op that still
    // answers - EnableMouse was defined here and so no frame ever took the
    // mouse, however plainly the call read in the addon.
    bootstrap(
        "local mt = __WoweeFrameMT\n"

        "function mt:EnableMouseWheel(enable)\n"
        "    __WoweeSetWheelEnabled(self, enable ~= false)\n"
        "end\n"
        // Its other half, which WoW has and which is the only way to ask
        // whether a frame takes the wheel at all - a scroll frame takes it by
        // being one, so the answer is not simply "whatever was passed to
        // EnableMouseWheel".
        "function mt:IsMouseWheelEnabled()\n"
        "    return __WoweeIsWheelEnabled(self)\n"
        "end\n"
        // A scroll frame's range is recomputed after every layout, so asking
        // for it again has nothing to do - but answering rather than falling
        // through to the no-op list keeps it out of a report whose whole
        // purpose is naming things that are genuinely absent.
        "function mt:UpdateScrollChildRect() end\n"
        // ItemButton:GetInventorySlot() - which equipment slot this button
        // stands for. Every caller in FrameXML is a bank button, and the answer
        // is the arithmetic BankButtonIDToInvSlotID already does: the general
        // slots follow the equipment, the bank bags follow those, and `isBag`
        // - set by the button's own OnLoad - says which.
        //
        // It answered nil, and BankFrameItemButton_Update reads it straight
        // into GetInventoryItemTexture, so every bank slot came back with no
        // texture and no item. The bank drew as an empty bank however full it
        // was. The tooltip, the pick-up and the bag click all read it too.
        // GameTooltip:SetTracking() - what the minimap's tracking button is
        // hunting for. Written here rather than in C because everything it
        // needs is already answered: GetNumTrackingTypes and GetTrackingInfo
        // read the player's known tracking spells and which one is running.
        //
        // The button is on the minimap, which is drawn by FrameXML on the
        // default tier, so the empty tooltip a no-op left was on screen for
        // anyone who hovered it.
        "function mt:SetTracking()\n"
        "    local active\n"
        "    for i = 1, GetNumTrackingTypes() do\n"
        "        local name, _, on = GetTrackingInfo(i)\n"
        "        if on then active = name break end\n"
        "    end\n"
        "    self:SetText(active or MINIMAP_TRACKING or 'Tracking')\n"
        "    self:Show()\n"
        "end\n"
        // A model frame's dressing room. Written here because the two verbs
        // it forwards to live where item links are already parsed, and the
        // alternative was a third copy of that parser.
        "function mt:TryOn(link) __WoweeTryOn(self, link) end\n"
        "function mt:Undress() __WoweeUndress(self) end\n"
        // SetUnit and RefreshUnit are what a model frame is told to show. The
        // frame is driven by name from the render loop - the paperdoll, the
        // inspect window and the dressing room each have their own - so what
        // these have to do is answer rather than raise.
        // Model:SetCreature(displayId) - what a companion, a stabled pet or
        // any other creature preview is told to show. Written onto the frame
        // and built by the render loop, which is where the offscreen views
        // are; zero clears it.
        "function mt:SetCreature(id) __WoweeSetModelCreature(self, id) end\n"
        "function mt:RefreshUnit() end\n"
        "function mt:GetInventorySlot()\n"
        "    return BankButtonIDToInvSlotID(self:GetID(), self.isBag)\n"
        "end\n"
        // Message frames here do not scroll: every line is drawn and the frame
        // is always showing its newest, which is what AtBottom asks. Answering
        // false would have the interface offer a scroll-to-bottom button that
        // does nothing.
        "function mt:AtBottom() return true end\n"
        // Click() runs the frame's own OnClick, with the same PreClick and
        // PostClick around it that a real press produces. FrameXML activates
        // buttons this way - a keybinding that presses an action button, a
        // dropdown that picks its default - and it answered as a no-op, so
        // none of those did anything.
        // A scripted click flips a check button exactly as a real one does, so
        // the two paths cannot disagree about what the button now says. The
        // scripts are read first because a frame with none is not clicked at
        // all - and then neither is it toggled.
        "function mt:Click(button, down)\n"
        "    local s = rawget(self, '__scripts')\n"
        "    if not s then return end\n"
        "    button = button or 'LeftButton'\n"
        "    if self:GetObjectType() == 'CheckButton' and self:IsEnabled() then\n"
        "        self:SetChecked(not self:GetChecked())\n"
        "    end\n"
        "    if s.PreClick then s.PreClick(self, button, down) end\n"
        "    if s.OnClick then s.OnClick(self, button, down) end\n"
        "    if s.PostClick then s.PostClick(self, button, down) end\n"
        "end\n"
    );

    // Animations. Written in Lua because it is almost entirely bookkeeping -
    // what is playing, how far through, in what order - and the only thing it
    // cannot do from here is move a frame without disturbing its anchors.
    //
    // Nothing existed before this: CreateAnimationGroup was not defined, so a
    // frame that declared one got the missing-API fallback and every call on
    // the group returned nil. FrameXML animates the tutorial pointer and the
    // alert frames this way, and addons use it far more.
    bootstrap(
        "local mt = __WoweeFrameMT\n"
        "__WoweePlayingAnimations = {}\n"
        "local playing = __WoweePlayingAnimations\n"

        "local animMeta = {}\n"
        "animMeta.__index = animMeta\n"
        "function animMeta:SetDuration(d) self.duration = d or 0 end\n"
        "function animMeta:GetDuration() return self.duration or 0 end\n"
        "function animMeta:SetChange(c) self.change = c end\n"
        "function animMeta:GetChange() return self.change end\n"
        "function animMeta:SetFromAlpha(a) self.fromAlpha = a end\n"
        "function animMeta:SetToAlpha(a) self.toAlpha = a end\n"
        "function animMeta:SetOffset(x, y) self.offsetX, self.offsetY = x, y end\n"
        "function animMeta:GetOffset() return self.offsetX or 0, self.offsetY or 0 end\n"
        // An animation carries scripts of its own, and OnFinished on the
        // *animation* is how FrameXML hides a faded-out frame:
        // alertframes.xml puts `self:GetRegionParent():Hide()` there. Only the
        // group's OnFinished was ever called, so an achievement banner faded to
        // nothing and stayed on screen forever.
        "function animMeta:SetScript(k, f) self[k] = f end\n"
        "function animMeta:GetScript(k) return self[k] end\n"
        "function animMeta:GetRegionParent() return self.group and self.group.parent end\n"
        "function animMeta:SetOrder(o) self.order = o or 1 end\n"
        "function animMeta:GetOrder() return self.order or 1 end\n"
        "function animMeta:SetStartDelay(d) self.startDelay = d or 0 end\n"
        "function animMeta:GetStartDelay() return self.startDelay or 0 end\n"
        "function animMeta:SetEndDelay(d) self.endDelay = d or 0 end\n"
        "function animMeta:SetSmoothing(s) self.smoothing = s end\n"
        "function animMeta:SetScale(x, y) self.scaleX, self.scaleY = x, y end\n"
        "function animMeta:SetDegrees(d) self.degrees = d end\n"
        "function animMeta:GetProgress() return self.progress or 0 end\n"
        // The same progress with the animation's own easing applied, which is
        // what anything driving a value off an animation actually wants.
        // The calendar reads it directly -
        // flashTexture:SetAlpha(CalendarViewEventFlashTimer:GetSmoothProgress())
        // on an <Animation smoothing="OUT"> - and a missing *method* is not a
        // nil to be checked but a hard error, so the whole event view went
        // down on the line that makes a highlight pulse.
        "function animMeta:GetSmoothProgress()\n"
        "    local t = self.progress or 0\n"
        "    if t < 0 then t = 0 elseif t > 1 then t = 1 end\n"
        "    local s = self.smoothing\n"
        "    if s == 'IN' then return t * t end\n"
        "    if s == 'OUT' then return t * (2 - t) end\n"
        "    if s == 'IN_OUT' then\n"
        "        if t < 0.5 then return 2 * t * t end\n"
        "        local u = 1 - t\n"
        "        return 1 - 2 * u * u\n"
        "    end\n"
        "    if s == 'OUT_IN' then\n"
        "        if t < 0.5 then local u = t * 2 return u * (2 - u) * 0.5 end\n"
        "        local u = (t - 0.5) * 2\n"
        "        return 0.5 + u * u * 0.5\n"
        "    end\n"
        // No smoothing named, or one this does not model: the linear progress
        // is the honest answer and reads as a steady fade rather than nothing.
        "    return t\n"
        "end\n"
        "function animMeta:GetElapsed() return self.elapsed or 0 end\n"
        "function animMeta:SetParent(p) self.parent = p end\n"
        "function animMeta:GetRegionParent() return self.group and self.group.parent end\n"
        "function animMeta:SetTarget(t) self.target = t end\n"
        "function animMeta:IsDelaying() return (self.elapsed or 0) < (self.startDelay or 0) end\n"
        "function animMeta:IsPlaying() return self.group and self.group:IsPlaying() end\n"
        // An animation answers Play, Pause, Stop and Finish as well as its
        // group does, and acts on the group when it does. Leaving these off
        // was worse than having no animations at all: an undefined
        // TutorialFrameCallOutPulser was a harmless fallback object that
        // swallowed :Stop(), and a real table without the method is a hard
        // error that took the whole file down with it.
        "function animMeta:Play()   if self.group then self.group:Play()   end end\n"
        "function animMeta:Stop()   if self.group then self.group:Stop()   end end\n"
        "function animMeta:Pause()  if self.group then self.group:Pause()  end end\n"
        "function animMeta:Resume() if self.group then self.group:Resume() end end\n"
        "function animMeta:Finish() if self.group then self.group:Finish() end end\n"
        "function animMeta:GetSmoothing() return self.smoothing end\n"
        "function animMeta:GetOrder() return self.order or 1 end\n"
        "function animMeta:SetScript(k, f) self[k] = f end\n"
        "function animMeta:GetScript(k) return self[k] end\n"

        "local groupMeta = {}\n"
        "groupMeta.__index = groupMeta\n"
        "function groupMeta:CreateAnimation(kind, name)\n"
        "    local a = setmetatable({kind = kind or 'Alpha', group = self,\n"
        "                            duration = 0, order = 1, startDelay = 0}, animMeta)\n"
        "    table.insert(self.animations, a)\n"
        "    if name then _G[name] = a end\n"
        "    return a\n"
        "end\n"
        "function groupMeta:GetAnimations() return unpack(self.animations) end\n"
        "function groupMeta:SetLooping(m) self.looping = m end\n"
        "function groupMeta:GetLooping() return self.looping or 'NONE' end\n"
        "function groupMeta:IsPlaying() return self.isPlaying == true end\n"
        "function groupMeta:IsDone() return self.isPlaying ~= true end\n"
        "function groupMeta:SetScript(k, f) self[k] = f end\n"
        "function groupMeta:GetScript(k) return self[k] end\n"
        "function groupMeta:HookScript(k, f)\n"
        "    local prev = self[k]\n"
        "    self[k] = function(...) if prev then prev(...) end f(...) end\n"
        "end\n"
        "function groupMeta:SetParent(p) self.parent = p end\n"
        "function groupMeta:GetParent() return self.parent end\n"
        "function groupMeta:GetDuration()\n"
        "    local total = 0\n"
        "    for _, a in ipairs(self.animations) do\n"
        "        local t = (a.startDelay or 0) + (a.duration or 0)\n"
        "        if t > total then total = t end\n"
        "    end\n"
        "    return total\n"
        "end\n"
        // The frame's alpha at the moment Play is called is what an Alpha
        // animation's change is relative to. Captured here rather than at
        // creation, because a group replayed later starts from wherever the
        // frame is then.
        "function groupMeta:Play()\n"
        "    self.isPlaying = true\n"
        "    self.reversed = false\n"
        "    self.baseAlpha = self.parent and self.parent:GetAlpha() or 1\n"
        "    for _, a in ipairs(self.animations) do a.elapsed = 0 a.progress = 0 a.finished = nil end\n"
        "    playing[self] = true\n"
        "    if self.OnPlay then self:OnPlay() end\n"
        "end\n"
        "function groupMeta:Stop()\n"
        "    self.isPlaying = false\n"
        "    playing[self] = nil\n"
        // Put back what the animations moved, or a stopped group leaves the
        // frame transparent or displaced with nothing to restore it.
        "    if self.parent then\n"
        "        if self.baseAlpha then self.parent:SetAlpha(self.baseAlpha) end\n"
        "        __WoweeSetAnimOffset(self.parent, 0, 0)\n"
        "    end\n"
        "    if self.OnStop then self:OnStop() end\n"
        "end\n"
        "function groupMeta:Finish()\n"
        "    self.isPlaying = false\n"
        "    playing[self] = nil\n"
        "    if self.OnFinished then self:OnFinished() end\n"
        "end\n"
        "function groupMeta:Pause() self.paused = true end\n"
        "function groupMeta:Resume() self.paused = nil end\n"

        // Stop every group on this frame at once. A frame method rather than a
        // group one, and the only animation call FrameXML makes without
        // holding the group: blizzard_glyphui.lua does sparkle:StopAnimating()
        // whenever a glyph slot empties, which is every time the glyph tab is
        // opened on a character with a free socket. Missing, that raised and
        // took GlyphFrame_UpdateGlyphSlot with it.
        "function mt:StopAnimating()\n"
        "    if self.__animGroups then\n"
        "        for _, g in ipairs(self.__animGroups) do g:Stop() end\n"
        "    end\n"
        "end\n"

        "function mt:CreateAnimationGroup(name)\n"
        "    local g = setmetatable({parent = self, animations = {}}, groupMeta)\n"
        "    if name then _G[name] = g end\n"
        "    self.__animGroups = self.__animGroups or {}\n"
        "    table.insert(self.__animGroups, g)\n"
        "    return g\n"
        "end\n"

        // Advanced once a frame from dispatchOnUpdate.
        "function __WoweeTickAnimations(elapsed)\n"
        "    for g in pairs(playing) do\n"
        "        if g.paused then\n"
        "        else\n"
        "            local anyRunning = false\n"
        "            local dx, dy = 0, 0\n"
        "            local alpha = g.baseAlpha or 1\n"
        "            for _, a in ipairs(g.animations) do\n"
        "                a.elapsed = (a.elapsed or 0) + elapsed\n"
        "                local t = a.elapsed - (a.startDelay or 0)\n"
        "                local d = a.duration or 0\n"
        "                if t < 0 then\n"
        "                    anyRunning = true\n"
        "                elseif d <= 0 then\n"
        "                    a.progress = 1\n"
        "                else\n"
        "                    local p = t / d\n"
        "                    local done = false\n"
        "                    if p >= 1 then p = 1 done = true else anyRunning = true end\n"
        "                    if g.reversed then p = 1 - p end\n"
        "                    a.progress = p\n"
        // Once per run, and before the group finishes, because the frame this
        // hides is the one the group is still animating.
        "                    if done and not a.finished then\n"
        "                        a.finished = true\n"
        "                        if a.OnFinished then a:OnFinished() end\n"
        "                    end\n"
        "                    if a.kind == 'Alpha' then\n"
        "                        if a.fromAlpha and a.toAlpha then\n"
        "                            alpha = a.fromAlpha + (a.toAlpha - a.fromAlpha) * p\n"
        "                        elseif a.change then\n"
        "                            alpha = (g.baseAlpha or 1) + a.change * p\n"
        "                        end\n"
        "                    elseif a.kind == 'Translation' then\n"
        "                        dx = dx + (a.offsetX or 0) * p\n"
        "                        dy = dy + (a.offsetY or 0) * p\n"
        "                    elseif a.kind == 'Scale' then\n"
        "                        local sx = a.scaleX\n"
        "                        if sx and g.parent then\n"
        "                            g.parent:SetScale(1 + (sx - 1) * p)\n"
        "                        end\n"
        "                    end\n"
        "                end\n"
        "            end\n"
        "            if g.parent then\n"
        "                if alpha < 0 then alpha = 0 elseif alpha > 1 then alpha = 1 end\n"
        "                g.parent:SetAlpha(alpha)\n"
        "                __WoweeSetAnimOffset(g.parent, dx, dy)\n"
        "            end\n"
        "            if not anyRunning then\n"
        "                local mode = g.looping or 'NONE'\n"
        "                if mode == 'REPEAT' or mode == 'BOUNCE' then\n"
        // BOUNCE plays back the way it came; REPEAT starts over. Either way the
        // clocks reset, or the next round finishes instantly.
        "                    if mode == 'BOUNCE' then g.reversed = not g.reversed end\n"
        "                    for _, a in ipairs(g.animations) do a.elapsed = 0 a.finished = nil end\n"
        "                    if g.OnLoop then g:OnLoop() end\n"
        "                else\n"
        "                    g:Finish()\n"
        "                end\n"
        "            end\n"
        "        end\n"
        "    end\n"
        "end\n"

        // SetID and GetID are defined further down, in the later chunk that
        // binds the same metatable, and that copy wins. The pair here was
        // byte-identical to it - same body, same __id key - so nothing
        // depended on which one ran. Removed so the duplicate-definition
        // check has nothing left to report but real faults.
        // The four edges are real bindings now, applied after this block.
        // Left here they would only be a silent fallback if that order ever
        // changed, and every frame reporting itself at the origin is worse
        // than none of them answering.
        // GetPoint and GetNumPoints are real bindings now, applied after this
        // block. Leaving the flat versions here would only be a silent
        // fallback if that order ever changed, and a constant point is worse
        // than none: it is where every frame goes.
        // Recorded, because a frame only receives the clicks it asks for.
        // FrameXML calls RegisterForClicks("LeftButtonUp", "RightButtonUp") on
        // the frames that want a context menu, and without this every frame
        // would answer a right-click whether it wanted one or not.
        "function mt:RegisterForClicks(...)\n"
        "    local set = {}\n"
        "    for i = 1, select('#', ...) do set[select(i, ...)] = true end\n"
        "    self.__clicks = set\n"
        "end\n"

        // SetAttribute and GetAttribute are defined further down, on the same
        // metatable, in a later chunk that overwrites whatever is here - so an
        // earlier pair is dead the moment it is written. The pair that used to
        // sit here kept its values under a different key and took one argument
        // where the real one takes three, and it is on the path every unit
        // frame's click goes through: SecureButton_GetModifiedAttribute asks
        // GetAttribute(prefix, name, suffix). Had the chunks ever been
        // reordered, clicking a unit frame would have stopped targeting and
        // right-clicking would have stopped opening a menu, with nothing to
        // say why.
        "function mt:HookScript(scriptType, fn)\n"
        "    local orig = self.__scripts and self.__scripts[scriptType]\n"
        "    if orig then\n"
        "        self:SetScript(scriptType, function(...) orig(...); fn(...) end)\n"
        "    else\n"
        "        self:SetScript(scriptType, fn)\n"
        "    end\n"
        "end\n"
        // IsMouseOver is not here. It was, answering a flat false, and it sat
        // among these no-ops as though it were one - but there is a real
        // implementation of it in C, registered earlier and therefore losing.
        // That one tests the cursor against the frame's own rect, and
        // dispatchMouse keeps sLastMouseX_ in interface units for no other
        // reason than to feed it.
        //
        // Sixteen files ask. WorldMapFrame_OnUpdate gates the area label under
        // the cursor on it, so the map never named the zone being pointed at;
        // the consolidated buff tooltip never knew the mouse had left it; and
        // the hybrid scroll frames could not tell they were being hovered.
    );

    // Button art, which XML declares as <NormalTexture>, <HighlightTexture>,
    // <ButtonText> and so on. The catch-all below would answer these with a
    // no-op, which is worse than it sounds: the setter would appear to work and
    // the matching getter would hand back nil, so button:GetNormalTexture()
    // :SetVertexColor(...) - which FrameXML does constantly to grey out an
    // unusable action - fails somewhere far from the cause.
    bootstrap(
        "local mt = __WoweeFrameMT\n"
        // A path is as valid an argument as a texture, and FrameXML uses both:
        // LoadMicroButtonTextures does
        // self:SetDisabledTexture("Interface\\Buttons\\...-Disabled"), then the
        // next line asks for it back and calls SetDesaturated on it. Storing
        // the string verbatim handed a string back, and a string has no widget
        // methods at all. A path makes or updates the slot's own texture.
        "for _, slot in ipairs({'NormalTexture', 'PushedTexture', 'HighlightTexture',\n"
        "                       'DisabledTexture', 'CheckedTexture',\n"
        "                       'DisabledCheckedTexture'}) do\n"
        "    local key = '__' .. slot\n"
        "    local layer = (slot == 'HighlightTexture') and 'HIGHLIGHT' or 'ARTWORK'\n"
        "    mt['Set' .. slot] = function(self, tex)\n"
        "        if type(tex) == 'string' then\n"
        "            local existing = self[key]\n"
        "            if type(existing) == 'table' then existing:SetTexture(tex) return end\n"
        "            local made = self:CreateTexture(nil, layer)\n"
        "            made:SetTexture(tex)\n"
        "            made:SetAllPoints(self)\n"
        "            __WoweeSetButtonArt(made, slot)\n"
        "            self[key] = made\n"
        "            return\n"
        "        end\n"
        "        if type(tex) == 'table' then __WoweeSetButtonArt(tex, slot) end\n"
        // Nothing, which is how a button is emptied - SendMailFrame_Update
        // hands SetNormalTexture the texture GetSendMailItem answered, and that
        // is nil for a slot with nothing on it. Dropping the reference alone
        // left the texture this slot had already made still parented, still
        // anchored and still drawing, so an attachment taken off a letter kept
        // its icon and a letter that had been sent kept all of them.
        "        if tex == nil or tex == false then\n"
        "            local existing = self[key]\n"
        "            if type(existing) == 'table' then existing:SetTexture(nil) end\n"
        "        end\n"
        "        self[key] = tex\n"
        "    end\n"
        "    mt['Get' .. slot] = function(self) return self[key] end\n"
        "end\n"
        // Attributes, and the OnAttributeChanged they fire.
        //
        // This is how FrameXML passes state to a handler without a global:
        // UIDropDownMenu_Initialize does
        // UIDropDownMenuDelegate:SetAttribute("initmenu", frame), and the
        // delegate's OnAttributeChanged is what actually sets
        // UIDROPDOWNMENU_INIT_MENU. No-opping SetAttribute left that nil, so
        // every menu built afterwards indexed nothing.
        // Formats and sets in one call, which is how FrameXML writes most of
        // its labels - 114 places, the character sheet's "Level 14 Human Mage"
        // among them. Unimplemented, every one of those kept whatever
        // placeholder its XML carried: that line read "Level level race class"
        // because that is literally what paperdollframe.xml says.
        //
        // Guarded, because a format string and its arguments disagreeing is a
        // Lua error, and taking down the file that asked for a label is worse
        // than showing the unformatted string.
        "function mt:SetFormattedText(fmt, ...)\n"
        "    if type(fmt) ~= 'string' then return end\n"
        "    local ok, out = pcall(string.format, fmt, ...)\n"
        "    self:SetText(ok and out or fmt)\n"
        "end\n"
        "function mt:SetAttribute(name, value)\n"
        "    self.__attributes = self.__attributes or {}\n"
        "    self.__attributes[name] = value\n"
        "    local handler = self.__scripts and self.__scripts.OnAttributeChanged\n"
        "    if handler then handler(self, name, value) end\n"
        "end\n"
        // The three-argument form names one attribute in pieces, and falls
        // back to the bare name when no piece-specific value was set - which
        // is how every action button works. ActionButton_OnLoad sets "type",
        // and the secure code asks for it as prefix "", name "type", suffix
        // "1", because the suffix for LeftButton is "1". Without the fallback
        // the lookup was for "type1", found nothing, and the click ran no
        // handler at all: the button was hit, and no spell was cast.
        // A '*' stands in for either piece, and both have to be tried.
        //
        // SecureUnitButton_OnLoad sets "*type1" and "*type2" - the asterisk
        // meaning "whatever modifier is held". Asking for prefix "", name
        // "type", suffix "1" looked for "type1", then for "type", and found
        // neither, so clicking a unit frame ran no handler at all: the player
        // frame did not target and right-clicking it opened no menu. Both
        // symptoms, one missing lookup.
        //
        // The order is the client's: most specific first, the bare name last.
        "function mt:GetAttribute(a, b, c)\n"
        "    if not self.__attributes then return nil end\n"
        "    if b == nil then return self.__attributes[a] end\n"
        "    local at = self.__attributes\n"
        "    local p, s = a or '', c or ''\n"
        "    local v = at[p .. b .. s]\n"
        "    if v == nil then v = at['*' .. b .. s] end\n"
        "    if v == nil then v = at[p .. b .. '*'] end\n"
        "    if v == nil then v = at['*' .. b .. '*'] end\n"
        "    if v == nil then v = at[b] end\n"
        "    return v\n"
        "end\n"
        // A scroll frame's content frame.
        // Nothing to scroll until the tree has been laid out, and zero is the
        // honest answer then. ScrollFrame_OnScrollRangeChanged compares the
        // bar value against this the moment a scroll frame is built.
        // The scroll getters are real bindings now, applied after this block.
        // Zero when unset, which is what the real client answers and what
        // FrameXML concatenates into a name without checking.
        "function mt:SetID(id) self.__id = id end\n"
        "function mt:GetID() return self.__id or 0 end\n"
        "function mt:GetScrollChild() return self.__scrollChild end\n"
        "function mt:SetFontString(fs) self.__fontString = fs end\n"
        // Made on demand when a button is asked for one it has not been
        // given. Every button has a font string in the real client, and
        // FrameXML assumes it: FCF_SetTabColor does
        // minFrame:GetFontString():SetTextColor(...) without checking.
        // The fill of a status bar, as an object. This client keeps a bar's
        // fill as a texture *path* on the bar itself, so there is no region to
        // hand back and one is made on demand - the same answer GetFontString
        // gives one line below, and for the same reason.
        //
        // It was nil, and blizzard_achievementui does
        // `self:GetStatusBarTexture():SetDrawLayer("BORDER")` in the OnLoad of
        // a virtual template, so every achievement progress bar raised there.
        //
        // What comes back is a real region: the layering and tinting calls
        // FrameXML makes on it are recorded rather than refused. They do not
        // drive the fill, which is drawn from the path - an honest stand-in
        // rather than a working one, and it is the raise that was the bug.
        "function mt:GetStatusBarTexture()\n"
        "    if not self.__barTexture then\n"
        "        self.__barTexture = self:CreateTexture(nil, 'ARTWORK')\n"
        "    end\n"
        "    return self.__barTexture\n"
        "end\n"
        "function mt:GetFontString()\n"
        "    if not self.__fontString then\n"
        "        self.__fontString = self:CreateFontString(nil, 'OVERLAY')\n"
        "    end\n"
        "    return self.__fontString\n"
        "end\n"
        // A button's text is its font string's text; keeping them apart means
        // SetText on the button quietly does nothing, which is how a bar full
        // of blank buttons happens.
        // An edit box keeps its own text; a button shows its font string's.
        // FrameXML calls SetText on both and the widget decides which it means.
        "function mt:SetText(text, r, g, b)\n"
        // OnTextSet belongs to the box, not to the typing: it fires when the
        // text is *set* rather than entered, which is how the chat box learns
        // that something put a channel prefix in it. Declared on the chat edit
        // box and dispatched by nothing until now.
        "    if self.__isEditBox then\n"
        "        __WoweeEditSetText(self, text)\n"
        "        local h = self.__scripts and self.__scripts.OnTextSet\n"
        "        if h then h(self) end\n"
        "        return\n"
        "    end\n"
        // A tooltip's SetText is its first line, not a font string's text.
        // It answers whether it took the call, so this can stop there.
        "    if __WoweeTooltipSetText(self, text, r, g, b) then return end\n"
        "    self.__text = text\n"
        // A button with no font string yet gets one, rather than storing the
        // text where nothing can draw it.
        //
        // Most buttons declare <ButtonText> and arrive with one. Those that
        // only carry text="DEFAULTS" and inherit a template with a NormalFont
        // and no ButtonText - UIPanelButtonGrayTemplate is the one in the
        // options frames - had nowhere to put it: GetText answered correctly
        // and the button drew blank. GetFontString above already creates one
        // on demand and is the same call, so this is the existing answer
        // rather than a second one.
        //
        // Only for a button. A plain frame's SetText is a value it keeps, and
        // giving every frame a font string would draw labels nothing asked
        // for.
        "    if not self.__fontString and self.IsObjectType\n"
        "       and self:IsObjectType('Button') then\n"
        "        self:GetFontString()\n"
        "        if self.__fontString then\n"
        "            self.__fontString:SetPoint('CENTER')\n"
        "            local f = self.GetNormalFontObject and self:GetNormalFontObject()\n"
        "            if f and self.__fontString.SetFontObject then\n"
        "                self.__fontString:SetFontObject(f)\n"
        "            end\n"
        "        end\n"
        "    end\n"
        "    if self.__fontString then self.__fontString:SetText(text) end\n"
        "end\n"
        "function mt:GetText()\n"
        "    if self.__isEditBox then return __WoweeEditGetText(self) end\n"
        "    if self.__fontString then return self.__fontString:GetText() end\n"
        "    return self.__text\n"
        "end\n"
    );

    // Catch-all for unimplemented widget methods. Frames are logic-only stubs (not
    // natively rendered), so UI-heavy addons call many widget methods we don't model
    // (sliders: SetMinMaxValues/SetValue; check buttons: SetChecked; buttons:
    // SetNormalTexture; etc.). Without this, the first such call raises "attempt to
    // call a nil value" and aborts the addon before it can register its slash commands.
    // WoW widget methods are PascalCase, so an unknown key starting with an uppercase
    // letter is treated as an unimplemented method (harmless no-op); anything else
    // falls through to nil so ordinary addon fields keep their normal (falsy) meaning.
    // The widget methods this stands in for, named rather than guessed at.
    //
    // Answering every PascalCase key with a no-op was wrong for data. A field
    // is PascalCase as readily as a method - textStatusBar.TextString is the
    // one that surfaced it - and a function is truthy, so FrameXML's own
    // "if (x.Field) then use it" ran the branch against something that was
    // never there. Methods and data cannot be told apart by shape: of the 307
    // method names FrameXML calls, eighteen read as nouns (AppendText,
    // NumLines, PageUp, AtBottom), and of the PascalCase fields it assigns,
    // several are method names held in a table.
    //
    // So the set is enumerated: every method FrameXML calls on a widget, plus
    // the standard widget API for addons. A name in it answers with a no-op;
    // anything else is data and answers nil, which is what it would be.
    bootstrap(
        "__WoweeWidgetMethods = {\n"
        "AddDoubleLine=1,AddHistoryLine=1,AddLine=1,AddMessage=1,AddTexture=1,\n"
        "AddToAutoHide=1,AllowAttributeChanges=1,Animate=1,\n"
        "CallMethod=1,CanSaveTabardNow=1,ChildUpdate=1,Clear=1,ClearAllPoints=1,\n"
        "ClearBinding=1,ClearBindings=1,ClearFocus=1,ClearHistory=1,ClearLines=1,\n"
        "ClearModel=1,CreateFontString=1,CreatePlayerArrowFrame=1,\n"
        "CreateTexture=1,CreateTitleRegion=1,CycleVariation=1,Disable=1,DrawQuestBlob=1,\n"
        "Dress=1,Enable=1,EnableKeyboard=1,EnableMouse=1,EnableMouseWheel=1,\n"
        "EnableSubtitles=1,FadeOut=1,Free=1,GetAlpha=1,GetAnchorType=1,GetAttribute=1,\n"
        "GetBackdrop=1,GetBottom=1,GetButtonState=1,GetCenter=1,GetChecked=1,\n"
        "GetCheckedTexture=1,GetChildList=1,GetChildren=1,\n"
        "GetCursorPosition=1,GetDisabledCheckedTexture=1,\n"
        "GetDisabledTexture=1,GetDrawLayer=1,GetEffectiveAttribute=1,\n"
        "GetEffectiveScale=1,GetFileHeight=1,GetFileWidth=1,\n"
        "GetFontString=1,GetFrame=1,GetFrameLevel=1,GetFrameRef=1,\n"
        "GetFrameStrata=1,GetHeight=1,GetHighlightTexture=1,GetHorizontalScroll=1,\n"
        "GetHorizontalScrollRange=1,GetID=1,\n"
        "GetItem=1,GetLeft=1,GetLowerEmblemTexture=1,GetMessageInfo=1,\n"
        "GetMinMaxValues=1,GetMousePosition=1,GetName=1,GetNormalTexture=1,GetNumber=1,\n"
        "GetNumChildren=1,GetNumMessages=1,GetNumPoints=1,GetNumTooltips=1,\n"
        "GetObjectType=1,GetParent=1,GetPoint=1,GetPushedTexture=1,GetRect=1,\n"
        "GetRegionParent=1,GetRegions=1,GetRight=1,GetScale=1,GetScript=1,\n"
        "GetScrollChild=1,GetSize=1,GetSpacing=1,\n"
        "GetStringHeight=1,GetStringWidth=1,GetTexCoord=1,GetText=1,\n"
        "GetTextHeight=1,GetTexture=1,GetTextWidth=1,GetTooltipIndex=1,GetTop=1,\n"
        // GetUTF8CursorPosition is a real binding now, applied after this set.
        // A real method wins the lookup either way, but a name left here is a
        // claim that nothing implements it, and autocomplete's arithmetic is
        // the reason it could not stay a no-op.
        "GetUIPanel=1,GetUpperEmblemTexture=1,GetValue=1,\n"
        "GetVerticalScroll=1,GetVerticalScrollRange=1,GetWidth=1,\n"
        "GetZoom=1,GetZoomLevels=1,HasFocus=1,HasScript=1,Hide=1,HideUIPanel=1,\n"
        "HighlightText=1,HookScript=1,IgnoreDepth=1,InitializeTabardColors=1,Insert=1,\n"
        "IsEnabled=1,IsEquippedItem=1,IsEventRegistered=1,IsMouseEnabled=1,\n"
        "IsObjectType=1,IsProtected=1,IsShown=1,IsUnderMouse=1,\n"
        "IsUnit=1,IsVisible=1,LockHighlight=1,Lower=1,MoveUIPanel=1,\n"
        "New=1,NumLines=1,OnFinished=1,OnUpdate=1,PageDown=1,PageUp=1,\n"
        // PingLocation is a real binding now, applied after this set.
        "Play=1,Raise=1,RefreshValue=1,RegisterAutoHide=1,RegisterEvent=1,\n"
        "RegisterForClicks=1,RegisterForDrag=1,ReleaseFrame=1,\n"
        "RemoveMessagesByAccessID=1,ReplaceIconTexture=1,Reset=1,Reuse=1,Run=1,\n"
        "RunAttribute=1,RunFor=1,Save=1,ScrollDown=1,ScrollToBottom=1,ScrollUp=1,\n"
        "SelectWindow=1,SetAction=1,SetAllPoints=1,SetAlpha=1,SetAlphaGradient=1,\n"
        "SetAnchorType=1,SetAttribute=1,SetBackdrop=1,\n"
        "SetBackdropBorderColor=1,SetBackdropColor=1,SetBagItem=1,SetBinding=1,\n"
        "SetBindingClick=1,SetBindingItem=1,SetBindingMacro=1,SetBindingSpell=1,\n"
        "SetBlendMode=1,SetBorderAlpha=1,SetBorderScalar=1,SetBorderTexture=1,\n"
        "SetButtonState=1,SetCamera=1,SetChecked=1,SetCheckedTexture=1,\n"
        "SetClampedToScreen=1,SetCooldown=1,\n"
        "SetCursorPosition=1,SetDesaturated=1,SetDisabledCheckedTexture=1,\n"
        "SetDisabledFontObject=1,SetDisabledTexture=1,SetDrawLayer=1,\n"
        "SetFacing=1,SetFillAlpha=1,SetFillTexture=1,SetFocus=1,\n"
        "SetFontObject=1,SetFontString=1,SetFormattedText=1,SetFrameLevel=1,\n"
        "SetFrameRate=1,SetFrameStrata=1,SetHeight=1,SetHighlightFontObject=1,\n"
        "SetHighlightTexture=1,SetHitRectInsets=1,SetHorizontalScroll=1,\n"
        // SetHyperlinksEnabled is a real binding now, applied after this set.
        "SetHyperlinkCompareItem=1,SetID=1,\n"
        "SetInventoryItem=1,SetJustifyH=1,SetJustifyV=1,\n"
        "SetLight=1,SetMaxBytes=1,\n"
        "SetMaxLetters=1,\n"
        "SetMinMaxValues=1,SetModel=1,SetModelScale=1,\n"
        "SetMovable=1,SetMultiLine=1,SetNormalFontObject=1,SetNormalTexture=1,\n"
        "SetNumber=1,SetNumeric=1,SetOwner=1,SetParent=1,SetPetAction=1,\n"
        "SetPlayerTextureHeight=1,SetPlayerTextureWidth=1,SetPoint=1,SetPosition=1,\n"
        "SetPossession=1,SetPropagateKeyboardInput=1,SetPushedTexture=1,SetQuestItem=1,\n"

        "SetRotation=1,SetScale=1,SetScript=1,\n"
        "SetScrollChild=1,SetSelection=1,SetSequence=1,\n"
        "SetSequenceTime=1,SetShadowOffset=1,SetShown=1,SetSize=1,\n"
        "SetSpacing=1,SetSpell=1,SetSpellByID=1,SetStartDelay=1,SetStatusBarColor=1,\n"
        // Tooltip setters for things this client cannot describe yet. They
        // belong here rather than nowhere: a name the metatable does not answer
        // comes back nil, and GameTooltip:SetTalent(...) on nil is "attempt to
        // call method", which takes down the handler that was only trying to
        // show a tooltip. Hovering a talent did that, and the talent, auction
        // and trade skill panels all load. A no-op leaves the tooltip empty and
        // is recorded, so it stays visible as a gap instead of a crash.
        // SetTalent has left this list because it is implemented now. Leaving
        // it would not have broken anything - the lookup rawgets the real
        // method table first and only falls through to here - but this set says
        // "cannot describe it yet", and a name in it that works reads as a gap
        // that is not there, in the one place someone would check.
        "SetSocketGem=1,SetSocketedItem=1,SetExistingSocketGem=1,\n"
        "SetScrollOffset=1,RegisterAllEvents=1,\n"
        "SetStatusBarTexture=1,SetTexCoord=1,SetText=1,\n"
        "SetTexture=1,SetToplevel=1,\n"
        "SetUIPanel=1,SetUnit=1,\n"
        "SetUnitBuff=1,SetUnitDebuff=1,SetValue=1,SetValueStep=1,\n"
        "SetVertexColor=1,SetVerticalScroll=1,SetWidth=1,SetZoom=1,Show=1,ShowUIPanel=1,\n"
        "ShowUIPanelFailed=1,StartMovie=1,StartMoving=1,Stop=1,\n"
        "StopMovie=1,StopMovingOrSizing=1,ToggleInputLanguage=1,\n"
        "UIParentManageFramePositions=1,UnlockHighlight=1,UnregisterAllEvents=1,\n"
        "UnregisterAutoHide=1,UnregisterEvent=1,UpdateColorByID=1,\n"
        "UpdateMouseOverTooltip=1,UpdateTooltip=1,\n"
        "UpdateUIPanelPositions=1,\n"
        "}\n"
    );
    // WOWEE_WIDGET_TRACE=1 asks the missing-API report to say which object
    // reached a method as well as which method it was. Set before the chunk
    // that reads it, and only there: the report's whole value is being read
    // after a session, and a session run for that reason can afford the cost.
    {
        const char* trace = std::getenv("WOWEE_WIDGET_TRACE");
        const bool on = trace && *trace && std::string(trace) != "0";
        lua_pushboolean(L_, on ? 1 : 0);
        lua_setglobal(L_, "__WoweeWidgetTrace");
        if (on) {
            LOG_WARNING("WOWEE_WIDGET_TRACE is on: every widget method lookup "
                        "goes through Lua, and the missing-API report will name "
                        "the object as well as the method");
        }
    }
    bootstrap(
        "local mt = __WoweeFrameMT\n"
        "local methods = mt\n"
        "local known = __WoweeWidgetMethods\n"
        "local noop = function() end\n"
        "local seen = {}\n"
        // __index is the method table itself, with the fallback moved to a
        // metatable on that table.
        //
        // FrameXML reaches through it to call the original of an overridden
        // method - BlizzardOptionsPanel_Slider_Enable is
        // getmetatable(slider).__index.Enable(slider) - which needs a table
        // there. A function answered every lookup correctly and broke every one
        // of those.
        "setmetatable(mt, { __index = function(tbl, key)\n"
        "    local v = rawget(methods, key)\n"
        "    if v ~= nil then return v end\n"
        "    if type(key) ~= 'string' then return nil end\n"
        // A name in the set answers with a no-op - and is recorded, because a
        // method that quietly does nothing is indistinguishable from one that
        // works. SetFormattedText sat in this set unimplemented while 114
        // labels across FrameXML kept their XML placeholder text, and nothing
        // anywhere said so.
        //
        // What is NOT recorded, and cannot be from here, is which object
        // asked. This is the metatable's own __index, so the arguments are the
        // method table and the key - the frame that started the lookup is not
        // passed and there is no way to reach it. Recording it would mean
        // making every frame's __index a function instead of `mt.__index = mt`,
        // which puts a Lua call in front of every method lookup in the
        // interface.
        //
        // Worth knowing because the omission costs real time: the report says
        // "GetFont" and not which kind of object reached it, and GetFont
        // answered correctly on font strings for as long as it has existed. It
        // only fell through here when a chat *frame* asked for its own font.
        // Reading the method name alone sent me to WatchFrame, which reads its
        // font off a font string and was never affected.
        //
        // WOWEE_WIDGET_TRACE=1 gets the answer for the no-op family without
        // paying that: see the branch below, which records at call time rather
        // than at lookup time. It cannot help the family below that, which
        // answers nil - a nil cannot record anything, and the caller raises on
        // the next line and names itself in the traceback.
        "    if known[key] then\n"
        "        if not seen[key] then\n"
        "            seen[key] = true\n"
        "            if __WoweeRecordMissingApi then __WoweeRecordMissingApi('noop:' .. key) end\n"
        "        end\n"
        // Which object asked, where a run has said it wants to know.
        //
        // Not by making __index a function: FrameXML reaches through it -
        // getmetatable(slider).__index.Enable(slider) - and indexing a function
        // raises, which is the fault the comment above this metatable exists to
        // record. The instance arrives anyway, one step later: `frame:Method()`
        // calls whatever the lookup returned with the frame as its first
        // argument. So the no-op is what learns the name, at call time, and the
        // lookup stays a rawget.
        "        if not __WoweeWidgetTrace then return noop end\n"
        "        return function(self, ...)\n"
        "            local who = '?'\n"
        "            if type(self) == 'table' then\n"
        "                local kind = rawget(methods, 'GetObjectType')\n"
        "                who = (rawget(self, '__name') or '(unnamed)') .. ':' ..\n"
        "                      (kind and kind(self) or '?')\n"
        "            end\n"
        "            local at = 'noop:' .. key .. ' on ' .. who\n"
        "            if not seen[at] then\n"
        "                seen[at] = true\n"
        "                if __WoweeRecordMissingApi then __WoweeRecordMissingApi(at) end\n"
        "            end\n"
        "        end\n"
        "    end\n"
        // Recorded once so a method missing from the set is visible rather
        // than silently answering nil, which is the failure this trades for.
        // Not On*: those are script handler names, and reading one as a field
        // is how FrameXML asks whether a handler is set. Nil is the right
        // answer there, so recording it would be reporting correct behaviour
        // as a gap.
        "    if string.find(key, '^%u') and not string.find(key, '^On%u')\n"
        "       and not seen[key] then\n"
        "        seen[key] = true\n"
        "        if __WoweeRecordMissingApi then __WoweeRecordMissingApi('widget:' .. key) end\n"
        "    end\n"
        "    return nil\n"
        "end })\n"
        // The lookup itself goes through the method table, so a frame finds its
        // methods by rawget and anything unknown falls through to the function
        // above.
        "mt.__index = mt\n"
    );
}

/// CreateFrame, the screen and cursor globals, and the tables the frame
/// dispatch reads.
///
/// Opens by re-applying the C methods over the Lua block above: that block
/// exists to give unimplemented methods a harmless no-op and it runs later,
/// so any name it shares with a real binding would silently replace it.
void LuaEngine::registerFrameGlobals() {
    // The fallback is installed at the very end of initialize(), not here.
    // Everything below is still bootstrap Lua, and much of it opens with the
    // "LibStub = LibStub or {}" idiom - which reads nil only while _G answers
    // honestly. With the fallback already in place those never see nil, and
    // hang their tables off the fallback object instead of a fresh one.

    // Put the C bindings back over anything the Lua above defined with the same
    // name. That block exists to give unimplemented methods a harmless no-op,
    // and it runs later, so any name it shares with a real binding silently
    // replaces it - a method that answers and does nothing, which is far harder
    // to spot than one that errors. EnableMouse was lost this way and no frame
    // took the mouse at all; SetBackdrop and its two colour setters were about
    // to go the same way. Ordering the two makes the class of mistake
    // impossible rather than something to keep noticing.
    applyFrameMethods(L_);

    // CreateFrame function
    lua_pushcfunction(L_, lua_EditBox_SetText);
    lua_setglobal(L_, "__WoweeEditSetText");
    lua_pushcfunction(L_, lua_Tooltip_SetText);
    lua_setglobal(L_, "__WoweeTooltipSetText");
    lua_pushcfunction(L_, lua_EditBox_GetText);
    lua_setglobal(L_, "__WoweeEditGetText");

    lua_pushcfunction(L_, lua_CreateFrame);
    lua_setglobal(L_, "CreateFrame");

    // Cursor/screen/FPS functions
    lua_pushcfunction(L_, lua_GetCursorPosition);
    lua_setglobal(L_, "GetCursorPosition");
    lua_pushcfunction(L_, lua_WoweeReportFrame);
    lua_setglobal(L_, "__WoweeReportFrame");
    lua_pushcfunction(L_, lua_GetScreenWidth);
    lua_setglobal(L_, "GetScreenWidth");
    lua_pushcfunction(L_, lua_GetScreenHeight);
    lua_setglobal(L_, "GetScreenHeight");
    lua_pushcfunction(L_, lua_GetFramerate);
    lua_setglobal(L_, "GetFramerate");

    // Frame event dispatch table
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweeFrameEvents");

    // OnUpdate frame tracking table
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweeOnUpdateFrames");

    // Edit boxes whose text was set from code and still owe an OnTextChanged.
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweePendingTextChanged");

    // Frames shown that owe an OnAnimFinished - see queueAnimFinished.
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweePendingAnimFinished");

    // widget id -> frame table, so a hit test can find the scripts to run.
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweeFramesByWid");

    // Reached from EnableMouseWheel, which is written in Lua on the metatable
    // so it applies to every frame without another entry in the method list.
    lua_pushcfunction(L_, lua_Frame_SetWheelEnabled);
    lua_setglobal(L_, "__WoweeSetWheelEnabled");

    lua_pushcfunction(L_, lua_Frame_IsWheelEnabled);
    lua_setglobal(L_, "__WoweeIsWheelEnabled");

    // Reached from the button-art setters, which are written in Lua so they
    // cover every slot from one loop.
    lua_pushcfunction(L_, lua_Texture_SetButtonArt);
    lua_setglobal(L_, "__WoweeSetButtonArt");
}

}  // namespace addons
}  // namespace wowee
