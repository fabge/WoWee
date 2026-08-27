#pragma once

#include <functional>
#include <string>

namespace wowee {

// Forward declarations
namespace core { 
    class Window;
    class EntitySpawner;
    class AppearanceComposer;
    class WorldLoader;
}
namespace rendering { class Renderer; }
namespace pipeline { class AssetManager; }
namespace game { 
    class GameHandler;
    class ExpansionRegistry;
}
namespace audio { class AudioCoordinator; }

namespace ui {

class AddonBridge;
class RenderLocator;

/**
 * UI Services - Dependency injection container for UI components.
 * 
 * Break the singleton Phase B
 * 
 * Replaces Application::getInstance() calls throughout UI code.
 * Application creates this struct and injects it into UIManager,
 * which propagates it to GameScreen and all child UI components.
 * 
 * Owned by Application, shared as const pointers (non-owning).
 */
struct UIServices {
    core::Window* window = nullptr;
    rendering::Renderer* renderer = nullptr;
    pipeline::AssetManager* assetManager = nullptr;
    game::GameHandler* gameHandler = nullptr;
    game::ExpansionRegistry* expansionRegistry = nullptr;
    /// The addon system, through the narrow face src/ui asked for rather than
    /// as an AddonManager: reaching for the concrete type was one of the two
    /// remaining cycles in the library graph. See ui/addon_bridge.hpp.
    AddonBridge* addonBridge = nullptr;
    audio::AudioCoordinator* audioCoordinator = nullptr;
    
    // Extracted classes (also available individually for Phase A compatibility)
    core::EntitySpawner* entitySpawner = nullptr;
    core::AppearanceComposer* appearanceComposer = nullptr;
    core::WorldLoader* worldLoader = nullptr;
    
    /// Where a unit is on screen - position, bounds, foot z - for nameplates,
    /// combat text and picking. See ui/render_locator.hpp.
    RenderLocator* renderLocator = nullptr;

    /// Point the asset pipeline and the protocol tables at another expansion,
    /// and reload them. The login screen's expansion picker is the only caller;
    /// there is no service that owns the operation, so it is handed down as the
    /// two calls it is rather than as a pointer to Application.
    std::function<bool(const std::string& expansionId)> setAssetExpansionOverride;
    std::function<void()> reloadExpansionData;

    // Helper to check if core services are wired
    [[nodiscard]] bool isValid() const {
        return window && renderer && assetManager && gameHandler;
    }
};

/// The same services, for the handful of free functions in src/ui that have no
/// parameter to receive them - an icon cache that needs the window to upload
/// through, a scene pick that needs render bounds.
///
/// This is a narrower global than the one it replaced, not the absence of one,
/// and it is worth being plain about that: `Application::getInstance()` handed
/// out the whole composition root and made every translation unit that touched
/// it depend on src/core. This hands out the set src/ui was already given.
/// Everything with a `services_` member or a context struct uses that instead;
/// this is for the five helpers that have neither.
///
/// Set by Application wherever it calls UIManager::setServices, and stored by
/// value, so nothing here can outlive a pointer.
void setUiServices(const UIServices& services);
[[nodiscard]] const UIServices& uiServices();

} // namespace ui
} // namespace wowee
