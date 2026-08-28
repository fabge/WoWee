#pragma once

#include <cstdint>
#include <unordered_map>

namespace wowee::rendering::world_map {

/// The zone index for an AreaTable id, following the parent chain when the id
/// is a sub-area.
///
/// The continent map hit-tests through a ZMP: a 128x128 grid naming, per
/// pixel, the AreaTable id under it. Those ids sit at whatever depth the area
/// does. Of Kalimdor's 3851 non-empty cells only 1030 name a WorldMapArea zone
/// directly; the other 2821 are sub-areas. Resolving only the direct hits left
/// three quarters of the map with no answer, and the caller then fell back to
/// hit-testing zone *bounding boxes* - which overlap badly for any zone that
/// is not a rectangle.
///
/// Returns the first ancestor that has a zone, not the top of the chain: the
/// root of a sub-area's chain is not always a zone with a map of its own.
///
/// Depth-capped, because a malformed or custom DBC can name a cycle and this
/// runs under the cursor.
inline int zoneIndexForAreaViaParents(
        uint32_t areaId,
        const std::unordered_map<uint32_t, uint32_t>& parents,
        const std::unordered_map<uint32_t, int>& zoneIndexByAreaId) {
    if (areaId == 0) return -1;
    if (const auto it = zoneIndexByAreaId.find(areaId);
        it != zoneIndexByAreaId.end()) {
        return it->second;
    }
    uint32_t current = areaId;
    for (int depth = 0; depth < 16; ++depth) {
        const auto pit = parents.find(current);
        if (pit == parents.end() || pit->second == 0 || pit->second == current) break;
        current = pit->second;
        if (const auto zit = zoneIndexByAreaId.find(current);
            zit != zoneIndexByAreaId.end()) {
            return zit->second;
        }
    }
    return -1;
}

}  // namespace wowee::rendering::world_map
