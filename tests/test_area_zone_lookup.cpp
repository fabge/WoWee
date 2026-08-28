#include <catch_amalgamated.hpp>

#include "rendering/world_map/area_zone_lookup.hpp"

using wowee::rendering::world_map::zoneIndexForAreaViaParents;

namespace {

// Mulgore is a WorldMapArea zone at index 3; Camp Narache and Red Cloud Mesa
// sit under it, and Bloodhoof Village sits under Camp Narache.
constexpr uint32_t kMulgore = 215;
constexpr uint32_t kCampNarache = 220;
constexpr uint32_t kRedCloudMesa = 221;
constexpr uint32_t kBloodhoofVillage = 222;
constexpr uint32_t kHyjal = 616;  // real area, no WorldMapArea row

const std::unordered_map<uint32_t, uint32_t> kParents{
    {kCampNarache, kMulgore},
    {kRedCloudMesa, kMulgore},
    {kBloodhoofVillage, kCampNarache},
};

const std::unordered_map<uint32_t, int> kZones{{kMulgore, 3}};

}  // namespace

TEST_CASE("a zone resolves to itself", "[worldmap][area]") {
    CHECK(zoneIndexForAreaViaParents(kMulgore, kParents, kZones) == 3);
}

TEST_CASE("a sub-area resolves to its zone", "[worldmap][area]") {
    // The reported fault: 73% of the ZMP's cells name a sub-area, and every
    // one of them answered -1, so the map hit-tested bounding boxes instead.
    CHECK(zoneIndexForAreaViaParents(kCampNarache, kParents, kZones) == 3);
    CHECK(zoneIndexForAreaViaParents(kRedCloudMesa, kParents, kZones) == 3);
}

TEST_CASE("a sub-area two levels down resolves", "[worldmap][area]") {
    CHECK(zoneIndexForAreaViaParents(kBloodhoofVillage, kParents, kZones) == 3);
}

TEST_CASE("an area with no zone above it stays unresolved", "[worldmap][area]") {
    // Hyjal and the Gates of Ahn'Qiraj have no map of their own on the
    // continent, and answering some neighbour for them would be worse than
    // answering nothing.
    CHECK(zoneIndexForAreaViaParents(kHyjal, kParents, kZones) == -1);
}

TEST_CASE("area zero is not an area", "[worldmap][area]") {
    CHECK(zoneIndexForAreaViaParents(0, kParents, kZones) == -1);
}

TEST_CASE("a cycle in the parent chain terminates", "[worldmap][area]") {
    const std::unordered_map<uint32_t, uint32_t> cyclic{{10, 11}, {11, 12}, {12, 10}};
    CHECK(zoneIndexForAreaViaParents(10, cyclic, kZones) == -1);
}

TEST_CASE("an area that is its own parent terminates", "[worldmap][area]") {
    const std::unordered_map<uint32_t, uint32_t> selfish{{10, 10}};
    CHECK(zoneIndexForAreaViaParents(10, selfish, kZones) == -1);
}
