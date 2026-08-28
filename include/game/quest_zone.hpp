#pragma once

#include <cstdint>
#include <functional>

namespace wowee::game {

/// Whether a quest belongs to the zone the player is standing in.
///
/// `zoneOrSort` is the quest's AreaTable id when positive and a QuestSort id
/// when negative. The area it names is often a *sub*-area - Camp Narache
/// rather than Mulgore - so comparing it with the player's zone directly, or
/// comparing the two names it produces, drops quests that are plainly in the
/// zone the moment the player walks into a named corner of one. Both sides are
/// resolved to their zone before they are compared.
///
/// Anything that cannot be placed is kept. A QuestSort group - class, Epic, a
/// profession - names no area, and a quest whose query response has not landed
/// has no zone yet. The tracker's zone filter hides what it excludes, and when
/// it excludes everything WatchFrame_Update collapses the tracker *and*
/// disables the button that would open it again, so an unplaceable quest has
/// to count as shown: too many lines is a nuisance, a tracker that cannot be
/// opened is a dead frame.
inline bool questIsInZone(int32_t zoneOrSort, uint32_t currentZoneId,
                          const std::function<uint32_t(uint32_t)>& resolveAreaZone) {
    if (currentZoneId == 0) return true;   // the zone is not known yet
    if (zoneOrSort <= 0) return true;      // a sort group, or not yet queried
    const auto area = static_cast<uint32_t>(zoneOrSort);
    const uint32_t zone = resolveAreaZone ? resolveAreaZone(area) : area;
    return zone == currentZoneId;
}

}  // namespace wowee::game
