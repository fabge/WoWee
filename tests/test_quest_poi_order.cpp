#include <catch_amalgamated.hpp>

#include "game/quest_poi_order.hpp"

#include <algorithm>
#include <vector>

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

// The map's own rule for "complete", which is not the quest log's. The two
// disagreeing is what put a gap back in the button range and raised in
// QuestPOI_HideButtons on every world map update.
TEST_CASE("the map counts a quest complete the way FrameXML does", "[quest][poi]") {
    using wowee::game::worldMapCountsQuestComplete;

    // A finished quest, whatever else is true of it.
    CHECK(worldMapCountsQuestComplete(1, 3, 0, 0));
    CHECK(worldMapCountsQuestComplete(1, 0, 0, 500));

    // A failed one never is: WorldMapFrame_UpdateQuests sets isComplete false
    // and does not go on to the objectives test.
    CHECK_FALSE(worldMapCountsQuestComplete(-1, 0, 1000, 0));

    // And the branch the quest log has no equivalent for: no objectives, and
    // the money is covered. This is the one that bit - a quest whose
    // objectives never arrived reads as complete to the map.
    CHECK(worldMapCountsQuestComplete(0, 0, 1000, 500));
    CHECK(worldMapCountsQuestComplete(0, 0, 0, 0));
    CHECK_FALSE(worldMapCountsQuestComplete(0, 0, 100, 500));
    CHECK_FALSE(worldMapCountsQuestComplete(0, 2, 1000, 0));
}

// What the ordering is actually for, checked the way the interface uses it:
// walk the ordered list exactly as WorldMapFrame_UpdateQuests does and confirm
// both button ranges come out dense and one-based. A gap here is a nil index
// in QuestPOI_HideButtons, which takes the whole map update down with it.
namespace {

/// The two index sequences FrameXML would produce for an ordered POI list.
/// COMPLETE_SWAP takes the visible index; NUMERIC takes it less the number of
/// completed quests seen so far.
struct ButtonRanges {
    std::vector<int> completeSwap;
    std::vector<int> numeric;
};

ButtonRanges frameXmlButtonIndices(const std::vector<uint32_t>& ordered,
                                   const std::set<uint32_t>& completed) {
    ButtonRanges ranges;
    int completedSeen = 0;
    for (size_t i = 0; i < ordered.size(); ++i) {
        const int index = static_cast<int>(i) + 1;
        if (completed.contains(ordered[i])) {
            ++completedSeen;                      // WorldMapFrame_GetQuestFrame
            ranges.completeSwap.push_back(index); // WorldMapFrame_DisplayQuestPOI
        } else {
            ranges.numeric.push_back(index - completedSeen);
        }
    }
    return ranges;
}

bool isDenseFromOne(const std::vector<int>& indices) {
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] != static_cast<int>(i) + 1) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("the ordered list gives FrameXML dense button ranges", "[quest][poi]") {
    // The shape from the 2026-08-28 session: completed quests scattered
    // through the list, two of them behind incomplete ones.
    std::vector<uint32_t> ids{101, 102, 103, 104, 105, 106, 107, 108, 109};
    const std::set<uint32_t> completed{101, 102, 103, 108, 109};

    const auto before = frameXmlButtonIndices(ids, completed);
    CHECK(before.completeSwap == std::vector<int>{1, 2, 3, 8, 9});
    CHECK_FALSE(isDenseFromOne(before.completeSwap));  // the gap that raised

    wowee::game::orderQuestPoisForFrameXml(ids, completed);

    const auto after = frameXmlButtonIndices(ids, completed);
    CHECK(isDenseFromOne(after.completeSwap));
    CHECK(isDenseFromOne(after.numeric));
    CHECK(after.completeSwap.size() == 5);
    CHECK(after.numeric.size() == 4);
}

TEST_CASE("dense ranges hold for every arrangement of five quests", "[quest][poi]") {
    // Exhaustive rather than illustrative: whichever quests the server marks,
    // and in whatever order it sends them, the interface must never be handed
    // an index it has no button for.
    const std::vector<uint32_t> base{1, 2, 3, 4, 5};
    for (unsigned mask = 0; mask < 32u; ++mask) {
        std::set<uint32_t> completed;
        for (unsigned bit = 0; bit < 5u; ++bit) {
            if ((mask & (1u << bit)) != 0) completed.insert(base[bit]);
        }
        std::vector<uint32_t> ids = base;
        do {
            std::vector<uint32_t> ordered = ids;
            wowee::game::orderQuestPoisForFrameXml(ordered, completed);
            const auto ranges = frameXmlButtonIndices(ordered, completed);
            INFO("mask " << mask);
            CHECK(isDenseFromOne(ranges.completeSwap));
            CHECK(isDenseFromOne(ranges.numeric));
        } while (std::next_permutation(ids.begin(), ids.end()));
    }
}
