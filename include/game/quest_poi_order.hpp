#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace wowee::game {

/// The WotLK world-map Lua numbers completed POIs with their visible index and
/// incomplete POIs with `index - completedSeen`. That only forms dense button
/// ranges when completed quests precede incomplete ones.
inline void orderQuestPoisForFrameXml(std::vector<uint32_t>& questIds,
                                      const std::set<uint32_t>& completed) {
    std::stable_partition(questIds.begin(), questIds.end(),
                          [&](uint32_t id) { return completed.contains(id); });
}

}  // namespace wowee::game
