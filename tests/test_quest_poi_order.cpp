#include <catch_amalgamated.hpp>

#include "game/quest_poi_order.hpp"

TEST_CASE("completed quest POIs form a dense prefix for FrameXML", "[quest][poi]") {
    std::vector<uint32_t> ids{10, 20, 30, 40, 50};
    const std::set<uint32_t> completed{20, 50};

    wowee::game::orderQuestPoisForFrameXml(ids, completed);

    CHECK(ids == std::vector<uint32_t>{20, 50, 10, 30, 40});
}

TEST_CASE("quest POI ordering remains stable within each group", "[quest][poi]") {
    std::vector<uint32_t> ids{4, 3, 2, 1};
    const std::set<uint32_t> completed{4, 2};

    wowee::game::orderQuestPoisForFrameXml(ids, completed);

    CHECK(ids == std::vector<uint32_t>{4, 2, 3, 1});
}
