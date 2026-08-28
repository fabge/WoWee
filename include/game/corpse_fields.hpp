#pragma once

#include <cstdint>

namespace wowee::game {

/// Corpse update-field indices, 3.3.5.
///
/// `OBJECT_END` is 6, and the corpse block is laid out from there. Written out
/// because the owner index was a bare `6` at its one call site with the
/// arithmetic in a comment, and the flags field below needs the same sum done
/// again.
enum CorpseField : uint16_t {
    kCorpseFieldOwner = 6,          ///< +0x00, two words: low then high
    kCorpseFieldDisplayId = 10,     ///< +0x04
    kCorpseFieldFlags = 33,         ///< +0x1B
    kCorpseFieldDynamicFlags = 34,  ///< +0x1C
};

/// Set on a corpse the player can no longer return to.
///
/// A corpse becomes bones when its owner resurrects somewhere else, and the
/// bones stay in the world afterwards - still carrying the owner's guid, so
/// "is this mine?" answers yes for something that is no longer a destination.
constexpr uint32_t kCorpseFlagBones = 0x01;

/// Whether a corpse create block describes a corpse this player can run back
/// to: theirs, and not bones left over from an earlier death.
///
/// Reported as "release spirit jumps around when you have died multiple times
/// in the same spot and you have to run to different spots one after another".
/// A player has at most one live corpse, but old bones remain as objects and
/// re-enter view as the player moves - and each arrival overwrote the cached
/// corpse position, so the ghost was sent to whichever of the two the object
/// stream had mentioned last.
///
/// A missing flags field reads as zero, which is not bones - the right way
/// round, since an initial update mask omits fields whose value is zero and a
/// live corpse is exactly the one that may carry no flags at all.
constexpr bool corpseIsReclaimableBy(uint64_t ownerGuid, uint32_t ownerLow,
                                     uint64_t playerGuid, uint32_t corpseFlags) {
    const bool mine = ownerGuid == playerGuid ||
                      ownerLow == static_cast<uint32_t>(playerGuid);
    return mine && (corpseFlags & kCorpseFlagBones) == 0;
}

}  // namespace wowee::game
