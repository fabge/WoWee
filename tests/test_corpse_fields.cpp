#include <catch_amalgamated.hpp>

#include "game/corpse_fields.hpp"

using wowee::game::corpseIsReclaimableBy;
using wowee::game::kCorpseFlagBones;

namespace {
constexpr uint64_t kPlayer = 0x0000000000018E48ull;
constexpr uint64_t kSomeoneElse = 0x0000000000018E49ull;
}  // namespace

TEST_CASE("the player's own live corpse is reclaimable", "[corpse]") {
    CHECK(corpseIsReclaimableBy(kPlayer, static_cast<uint32_t>(kPlayer), kPlayer, 0));
}

TEST_CASE("the player's own bones are not", "[corpse]") {
    // The reported fault: bones from an earlier death keep the owner's guid,
    // re-enter view as the player walks, and overwrote the cached corpse
    // position - so the ghost was sent to whichever corpse was mentioned last.
    CHECK_FALSE(corpseIsReclaimableBy(kPlayer, static_cast<uint32_t>(kPlayer),
                                      kPlayer, kCorpseFlagBones));
}

TEST_CASE("another player's corpse is never reclaimable", "[corpse]") {
    CHECK_FALSE(corpseIsReclaimableBy(kSomeoneElse,
                                      static_cast<uint32_t>(kSomeoneElse), kPlayer, 0));
    CHECK_FALSE(corpseIsReclaimableBy(kSomeoneElse,
                                      static_cast<uint32_t>(kSomeoneElse), kPlayer,
                                      kCorpseFlagBones));
}

TEST_CASE("flags other than bones do not disqualify a corpse", "[corpse]") {
    // Hide-helm, hide-cloak and lootable all ride in the same field.
    CHECK(corpseIsReclaimableBy(kPlayer, static_cast<uint32_t>(kPlayer), kPlayer,
                                0x08 | 0x10 | 0x20));
}

TEST_CASE("the owner is matched on the low word alone as well", "[corpse]") {
    // An initial update mask omits fields whose value is zero, so a corpse
    // whose owner has a zero high word arrives with only the low one.
    CHECK(corpseIsReclaimableBy(0, static_cast<uint32_t>(kPlayer), kPlayer, 0));
}

TEST_CASE("an absent flags field reads as a live corpse", "[corpse]") {
    // Zero is what a missing field answers, and it is the right way round: a
    // live corpse is the one that may carry no flags at all, and bones always
    // carry a set bit, which no update mask omits.
    CHECK(corpseIsReclaimableBy(kPlayer, static_cast<uint32_t>(kPlayer), kPlayer, 0));
}
