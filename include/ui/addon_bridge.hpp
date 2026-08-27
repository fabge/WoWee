#pragma once

// What this client's own interface needs from the addon system.
//
// src/ui reached into src/addons for nine symbols - AddonManager::reload,
// runScript, fireEvent, setAddonEnabled, isAddonEnabled, TocFile::getTitle,
// LuaEngine::dispatchSlashCommand, openInterfaceQuestLog and
// noteClientSettingChanged - against fifty the other way, and that was one of
// the two cycles left in the library graph.
//
// The traffic is real: the chat box runs Lua and dispatches slash commands, the
// addon panel lists and toggles addons, /reload reloads them, a quest link opens
// the interface's quest log, and a settings control tells the CVar store that a
// setting it drives moved. None of it is a reach for the composition root, so
// none of it is fixed by injection: what src/ui needs is a *narrower* thing than
// AddonManager, which is exactly what an interface is for.
//
// Declared here, in src/ui, because src/ui owns the requirement. Implemented in
// src/addons, which already depends on src/ui for fifty other symbols, and wired
// by Application into UIServices.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::ui {

/// One row of the addon panel.
struct AddonListEntry {
    std::string name;     ///< The folder name, which is the id everything uses.
    std::string title;    ///< The .toc's Title, or the folder name when it has none.
    std::string version;  ///< Empty when the .toc does not say.
    std::string author;   ///< Empty when the .toc does not say.
    bool enabled = false;
};

class AddonBridge {
public:
    virtual ~AddonBridge() = default;

    /// Whether the Lua engine is up. A bridge exists from the moment the addon
    /// manager does; this is whether it can run anything yet, which is what
    /// the chat box tells the player apart from "no addon system at all".
    [[nodiscard]] virtual bool isRunning() const = 0;

    /// Run a chunk of Lua as the player typed it. False if it did not compile.
    virtual bool runScript(const std::string& lua) = 0;

    /// Raise a FrameXML event by name.
    virtual void fireEvent(const std::string& event,
                           const std::vector<std::string>& args = {}) = 0;

    /// Offer a slash command to the interface. True when it handled it, in
    /// which case this client must not also act on it.
    virtual bool dispatchSlashCommand(const std::string& command,
                                      const std::string& args) = 0;

    /// Reload the interface, as /reload does.
    virtual void reload() = 0;

    /// Every addon found, in the order the panel should list them.
    [[nodiscard]] virtual std::vector<AddonListEntry> listAddons() const = 0;

    /// Turn one on or off by folder name. Takes effect at the next load.
    virtual void setAddonEnabled(const std::string& name, bool enabled) = 0;

    /// Open the interface's quest log at one quest, as a quest link does.
    virtual void openQuestLog(uint32_t questId) = 0;

    /// Tell the CVar store that a setting a CVar drives has moved elsewhere.
    ///
    /// Six settings are driven by a Blizzard control and three of those also
    /// have a control in this client's own window; the store is applied over
    /// the settings file at start-up, so without this a change made here was
    /// undone at the next start by a CVar nobody had touched.
    virtual void noteSettingChanged(const std::string& key,
                                    const std::string& value) = 0;
};

} // namespace wowee::ui
