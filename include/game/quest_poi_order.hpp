#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace wowee::game {

/// Whether the world map counts a quest as complete, spelled the way
/// WorldMapFrame_UpdateQuests spells it:
///
///     if     ( isComplete and isComplete < 0 ) then isComplete = false;
///     elseif ( numObjectives == 0 and playerMoney >= requiredMoney ) then
///                                                   isComplete = true;
///     end
///
/// `isComplete` is GetQuestLogTitle's seventh value: 1 finished, -1 failed,
/// absent otherwise, which is a 0 here.
///
/// This has to be the map's rule and not the quest log's, because the order
/// below is what makes the map's own button arithmetic come out dense. A quest
/// the server has not marked complete but whose objectives never arrived is
/// complete to the map and incomplete to us, and one such quest in the middle
/// of the list is a gap in the button range - which is the crash this whole
/// file exists to keep away.
[[nodiscard]] inline bool worldMapCountsQuestComplete(int isComplete,
                                                      int numObjectives,
                                                      int64_t playerMoney,
                                                      int64_t requiredMoney) {
    if (isComplete < 0) return false;          // failed, and nothing else runs
    if (isComplete > 0) return true;
    return numObjectives == 0 && playerMoney >= requiredMoney;
}

/// The WotLK world-map Lua numbers completed POIs with their visible index and
/// incomplete POIs with `index - completedSeen`. That only forms dense button
/// ranges when completed quests precede incomplete ones.
///
/// `completed` must be judged by worldMapCountsQuestComplete above. Judging it
/// any other way puts the two halves out of step and reopens the gap.
inline void orderQuestPoisForFrameXml(std::vector<uint32_t>& questIds,
                                      const std::set<uint32_t>& completed) {
    std::stable_partition(questIds.begin(), questIds.end(),
                          [&](uint32_t id) { return completed.contains(id); });
}

}  // namespace wowee::game
