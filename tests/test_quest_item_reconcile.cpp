// QuestHandler::reconcileItemObjectivesFromInventory against the bag.
//
// In 3.3.5a the server never pushes item-objective counts. Kill credit arrives
// packed into the quest-log update fields, but the count for "collect 1 Bundle
// of Furs" is the client's own job: it counts matching item IDs in the bags.
// This reconcile is the only thing that ever writes QuestLogEntry::itemCounts,
// so if it declines to look at a quest, that quest's objective line reads zero
// for the rest of its life.
//
// Which is what happened. It began with `if (quest.complete) continue;`, and
// the server marks a collect quest complete from its update fields the moment
// the item is looted - before any inventory rebuild ran. So the quest log drew
// "Bundle of Furs: 0/1" on a quest it was simultaneously marking (Complete),
// and the reward panel's Complete Quest button did nothing.
//
// The first test to exercise a src/game/ handler rather than a header-only
// pure function: it links wowee_game. Before the subsystem libraries existed
// this needed 59 translation units named by hand, which is why the fix of
// 2026-08-26 shipped without one.
#include <catch_amalgamated.hpp>

#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/quest_handler.hpp"

using wowee::game::GameHandler;
using wowee::game::GameServices;
using wowee::game::QuestHandler;

namespace {

// A quest log holding one collect objective, seeded directly.
QuestHandler::QuestLogEntry collectQuest(uint32_t questId, uint32_t itemId,
                                         uint32_t required, bool complete) {
    QuestHandler::QuestLogEntry q;
    q.questId = questId;
    q.title = "Bundle of Furs";
    q.complete = complete;
    q.itemObjectives[0].itemId = itemId;
    q.itemObjectives[0].required = required;
    return q;
}

}  // namespace

TEST_CASE("a complete quest still has its item objective counted",
          "[quest][inventory]") {
    GameServices services{};
    GameHandler handler(services);
    QuestHandler& quests = *handler.getQuestHandler();

    // The exact shape of the bug: the server already said complete, and the
    // client has the item in the bag but has never counted it.
    quests.questLogRef().push_back(collectQuest(1234, 5678, 1, /*complete=*/true));

    quests.reconcileItemObjectivesFromInventory({{5678, 1}});

    const auto& log = quests.getQuestLog();
    REQUIRE(log.size() == 1);
    // Zero here is the regression: the objective line would read "0/1".
    CHECK(log[0].itemCounts.at(5678) == 1);
    CHECK(log[0].requiredItemCounts.at(5678) == 1);
}

TEST_CASE("carrying more than the objective needs does not overcount",
          "[quest][inventory]") {
    GameServices services{};
    GameHandler handler(services);
    QuestHandler& quests = *handler.getQuestHandler();
    quests.questLogRef().push_back(collectQuest(1, 42, 5, /*complete=*/false));

    quests.reconcileItemObjectivesFromInventory({{42, 99}});

    // FrameXML draws the fraction verbatim: "99/5" is a visible defect.
    CHECK(quests.getQuestLog()[0].itemCounts.at(42) == 5);
}

TEST_CASE("an emptied bag lets the objective count fall again",
          "[quest][inventory]") {
    GameServices services{};
    GameHandler handler(services);
    QuestHandler& quests = *handler.getQuestHandler();
    quests.questLogRef().push_back(collectQuest(1, 42, 3, /*complete=*/false));

    quests.reconcileItemObjectivesFromInventory({{42, 3}});
    REQUIRE(quests.getQuestLog()[0].itemCounts.at(42) == 3);

    // Destroying or selling the item is a real thing a player does, and a
    // stock client walks the count back down rather than latching it.
    quests.reconcileItemObjectivesFromInventory({});
    CHECK(quests.getQuestLog()[0].itemCounts.at(42) == 0);
}

TEST_CASE("an objective with no item id is left alone", "[quest][inventory]") {
    GameServices services{};
    GameHandler handler(services);
    QuestHandler& quests = *handler.getQuestHandler();
    // Most quests fill only the first slot or two of the six.
    quests.questLogRef().push_back(collectQuest(1, 0, 0, /*complete=*/false));

    quests.reconcileItemObjectivesFromInventory({{0, 7}});

    CHECK(quests.getQuestLog()[0].itemCounts.empty());
}
