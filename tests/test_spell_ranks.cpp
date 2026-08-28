#include <catch_amalgamated.hpp>

#include "game/spell_ranks.hpp"

#include <map>

using wowee::game::partitionHighestRanks;
using wowee::game::spellRankNumber;

TEST_CASE("a rank string parses to its number", "[spell][rank]") {
    CHECK(spellRankNumber("Rank 1") == 1);
    CHECK(spellRankNumber("Rank 4") == 4);
    CHECK(spellRankNumber("Rank 12") == 12);
}

TEST_CASE("a spell with no rank string is rank zero", "[spell][rank]") {
    // Most spells have none, and each is the only one of its name - so zero is
    // its own highest rank, not a spell that loses to everything.
    CHECK(spellRankNumber("") == 0);
}

TEST_CASE("rank text is matched whatever its case", "[spell][rank]") {
    CHECK(spellRankNumber("rank 3") == 3);
    CHECK(spellRankNumber("RANK 3") == 3);
}

TEST_CASE("text that is not a rank is rank zero", "[spell][rank]") {
    CHECK(spellRankNumber("Apprentice") == 0);
    CHECK(spellRankNumber("Racial Passive") == 0);
    CHECK(spellRankNumber("Rank") == 0);
}

namespace {

// Two ranks of Lightning Bolt and Earth Shock, one Stoneclaw Totem - the
// reported spellbook.
const std::map<uint32_t, std::pair<std::string, int>> kBook{
    {403, {"Lightning Bolt", 1}}, {529, {"Lightning Bolt", 2}},
    {8042, {"Earth Shock", 1}},   {8044, {"Earth Shock", 2}},
    {5730, {"Stoneclaw Totem", 1}},
    {2484, {"Earthbind Totem", 0}},
};
const auto kNameOf = [](uint32_t id) { return kBook.at(id).first; };
const auto kRankOf = [](uint32_t id) { return kBook.at(id).second; };

}  // namespace

TEST_CASE("the highest rank of each name moves to the front", "[spell][rank]") {
    std::vector<uint32_t> ids{403, 529, 8042, 8044, 2484};
    const size_t highest = partitionHighestRanks(ids, kNameOf, kRankOf);
    CHECK(highest == 3);
    // Lightning Bolt r2, Earth Shock r2, Earthbind Totem, then the leftovers.
    CHECK(std::vector<uint32_t>(ids.begin(), ids.begin() + 3) ==
          std::vector<uint32_t>{529, 8044, 2484});
}

TEST_CASE("order within each group is kept", "[spell][rank]") {
    // The caller has already sorted by name and the book reads in that order.
    std::vector<uint32_t> ids{8044, 529, 8042, 403};
    partitionHighestRanks(ids, kNameOf, kRankOf);
    CHECK(ids == std::vector<uint32_t>{8044, 529, 8042, 403});
}

TEST_CASE("a spell with one rank is its own highest", "[spell][rank]") {
    std::vector<uint32_t> ids{5730, 2484};
    CHECK(partitionHighestRanks(ids, kNameOf, kRankOf) == 2);
}

TEST_CASE("an empty book collapses to nothing", "[spell][rank]") {
    std::vector<uint32_t> ids;
    CHECK(partitionHighestRanks(ids, kNameOf, kRankOf) == 0);
}

TEST_CASE("three ranks collapse to one", "[spell][rank]") {
    const std::map<uint32_t, std::pair<std::string, int>> book{
        {1, {"Healing Wave", 1}}, {2, {"Healing Wave", 2}}, {3, {"Healing Wave", 3}}};
    std::vector<uint32_t> ids{1, 2, 3};
    const size_t highest = partitionHighestRanks(
        ids, [&](uint32_t i) { return book.at(i).first; },
        [&](uint32_t i) { return book.at(i).second; });
    CHECK(highest == 1);
    CHECK(ids.front() == 3);
}
