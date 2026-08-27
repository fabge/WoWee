#pragma once

#include <cstdint>

namespace wowee {
namespace game {

/// What to call a map on screen - "Eastern Kingdoms", "The Stockade".
/// Returns nullptr for a map with no friendly name, which is most of them.
const char* mapDisplayName(uint32_t mapId);

/// The WDT directory a map's terrain lives in - "Azeroth", "Shadowfang".
///
/// The fallback for when Map.dbc is unavailable. The names must match the
/// directory names case-insensitively, which they do because AssetManager
/// lowercases every path. Returns "" for an unknown map.
const char* mapWdtName(uint32_t mapId);

} // namespace game
} // namespace wowee
