#include <catch_amalgamated.hpp>

#include "game/quest_zone.hpp"

#include <map>

using wowee::game::questIsInZone;

namespace {

// Mulgore holds Camp Narache and Bloodhoof Village; Durotar is somewhere else.
constexpr uint32_t kMulgore = 215;
constexpr uint32_t kCampNarache = 220;
constexpr uint32_t kBloodhoofVillage = 222;
constexpr uint32_t kDurotar = 14;

const std::function<uint32_t(uint32_t)> kResolve = [](uint32_t area) -> uint32_t {
    static const std::map<uint32_t, uint32_t> parents{
        {kCampNarache, kMulgore}, {kBloodhoofVillage, kMulgore}};
    const auto it = parents.find(area);
    return it == parents.end() ? area : it->second;
};

}  // namespace

TEST_CASE("a quest filed under the zone itself is in the zone", "[quest][zone]") {
    CHECK(questIsInZone(static_cast<int32_t>(kMulgore), kMulgore, kResolve));
}

TEST_CASE("a quest filed under a sub-area is still in the zone", "[quest][zone]") {
    // The reported fault: standing in Mulgore, a quest filed under Camp
    // Narache was dropped from the tracker because the two names differ.
    CHECK(questIsInZone(static_cast<int32_t>(kCampNarache), kMulgore, kResolve));
    CHECK(questIsInZone(static_cast<int32_t>(kBloodhoofVillage), kMulgore, kResolve));
}

TEST_CASE("a quest in another zone is filtered out", "[quest][zone]") {
    CHECK_FALSE(questIsInZone(static_cast<int32_t>(kDurotar), kMulgore, kResolve));
}

TEST_CASE("an unknown zone filters nothing", "[quest][zone]") {
    // No zone means the terrain under the player has not resolved yet, which
    // is every login. Filtering there empties the tracker, and an empty
    // tracker collapses itself and disables the button that reopens it.
    CHECK(questIsInZone(static_cast<int32_t>(kDurotar), 0, kResolve));
}

TEST_CASE("a quest with no area is kept", "[quest][zone]") {
    // Negative is a QuestSort - class, Epic, a profession - and zero is a
    // quest whose query response has not landed. Neither names a place, so
    // neither can be shown to be elsewhere.
    CHECK(questIsInZone(-1, kMulgore, kResolve));
    CHECK(questIsInZone(0, kMulgore, kResolve));
}

TEST_CASE("without a resolver the area is compared as given", "[quest][zone]") {
    CHECK(questIsInZone(static_cast<int32_t>(kMulgore), kMulgore, nullptr));
    CHECK_FALSE(questIsInZone(static_cast<int32_t>(kCampNarache), kMulgore, nullptr));
}
