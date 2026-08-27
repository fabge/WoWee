// The addon system's side of ui::AddonBridge.
//
// Nothing here decides anything; it is the narrow face src/ui asked for, over
// the AddonManager and the two free functions it also reached for. Lives in
// src/addons because that is what it forwards to, and because src/addons
// already depends on src/ui for fifty other symbols - putting it here is what
// makes the dependency one-directional.

#include "ui/addon_bridge.hpp"

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"

#include <memory>
#include <utility>

namespace wowee::addons {

namespace {

class AddonBridgeImpl final : public ui::AddonBridge {
public:
    AddonBridgeImpl(AddonManager& manager, game::GameHandler* gameHandler)
        : manager_(manager), gameHandler_(gameHandler) {}

    bool isRunning() const override { return manager_.isInitialized(); }

    bool runScript(const std::string& lua) override {
        return manager_.runScript(lua);
    }

    void fireEvent(const std::string& event,
                   const std::vector<std::string>& args) override {
        manager_.fireEvent(event, args);
    }

    bool dispatchSlashCommand(const std::string& command,
                              const std::string& args) override {
        auto* engine = manager_.getLuaEngine();
        return engine && engine->dispatchSlashCommand(command, args);
    }

    void reload() override { manager_.reload(); }

    std::vector<ui::AddonListEntry> listAddons() const override {
        std::vector<ui::AddonListEntry> out;
        const auto& addons = manager_.getAddons();
        out.reserve(addons.size());
        for (const auto& addon : addons) {
            ui::AddonListEntry entry;
            entry.name = addon.addonName;
            entry.title = addon.getTitle();
            // Absent directives are an empty string rather than a missing key,
            // because the panel draws them as a line and an absent one is a
            // line it leaves out.
            if (auto it = addon.directives.find("Version"); it != addon.directives.end())
                entry.version = it->second;
            if (auto it = addon.directives.find("Author"); it != addon.directives.end())
                entry.author = it->second;
            entry.enabled = manager_.isAddonEnabled(addon.addonName);
            out.push_back(std::move(entry));
        }
        return out;
    }

    void setAddonEnabled(const std::string& name, bool enabled) override {
        manager_.setAddonEnabled(name, enabled);
    }

    void openQuestLog(uint32_t questId) override {
        if (gameHandler_) openInterfaceQuestLog(*gameHandler_, questId);
    }

    void noteSettingChanged(const std::string& key, const std::string& value) override {
        noteClientSettingChanged(key, value);
    }

private:
    AddonManager& manager_;
    game::GameHandler* gameHandler_ = nullptr;
};

}  // namespace

std::unique_ptr<ui::AddonBridge> makeAddonBridge(AddonManager& manager,
                                                 game::GameHandler* gameHandler) {
    return std::make_unique<AddonBridgeImpl>(manager, gameHandler);
}

}  // namespace wowee::addons
