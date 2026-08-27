#pragma once

// The addon system's side of ui::AddonBridge. See src/addons/addon_bridge.cpp.

#include <memory>

namespace wowee {
namespace game { class GameHandler; }
namespace ui { class AddonBridge; }
namespace addons {

class AddonManager;

/// Build the narrow face src/ui uses to reach the addon system.
///
/// The GameHandler is for the quest-log link and may be null; everything else
/// works without one.
std::unique_ptr<ui::AddonBridge> makeAddonBridge(AddonManager& manager,
                                                 game::GameHandler* gameHandler);

}  // namespace addons
}  // namespace wowee
