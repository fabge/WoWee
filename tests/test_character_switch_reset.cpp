// What the previous character leaves behind.
//
// This client has now shipped the same bug three times, in three different
// clusters, and the shape never changes: a per-character value is written only
// from an update field, and an update field is sent when the server has a value
// for it - so a character who has none of a thing is never told it is zero and
// keeps whatever the last one had.
//
//   2026-08-26  the pet's action bar, stance and spell list. A hunter's pet
//               was still in the mage's spellbook.
//   2026-08-27  the pet's stats. A hunter's pet attack power was still
//               readable from a mage, and the paperdoll's pet tab drew it.
//   2026-08-27  eight player fields. A mage after a druid kept the druid's
//               shapeshift form; a character with no title wore the previous
//               one's; the rested XP, honor and arena points on the screen
//               were somebody else's.
//
// selectCharacter needs a socket, so nothing could test what it cleared. The
// clearing is resetStateForCharacterSwitch now, and this is that test.
#include <catch_amalgamated.hpp>

#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/spell_handler.hpp"

using wowee::game::GameHandler;
using wowee::game::GameServices;

TEST_CASE("a character switch clears the previous character's own fields",
          "[character][switch]") {
    GameServices services{};
    GameHandler handler(services);

    // What SMSG_UPDATE_OBJECT leaves behind for a titled druid with rested XP
    // and some honor saved up.
    handler.chosenTitleBitRef() = 42;
    handler.shapeshiftFormIdRef() = 5;   // bear
    handler.playerHonorPointsRef() = 1234;
    handler.playerArenaPointsRef() = 99;
    handler.playerRestedXpRef() = 5000;
    handler.playerManaRegenRef() = 12.5f;
    handler.playerManaRegenCastingRef() = 4.0f;
    handler.playerTransportStickyGuidRef() = 0xDEADBEEF;
    handler.playerTransportStickyTimerRef() = 8.0f;

    REQUIRE(handler.getChosenTitleBit() == 42);
    REQUIRE(handler.getShapeshiftFormId() == 5);
    REQUIRE(handler.getHonorPoints() == 1234);

    handler.resetStateForCharacterSwitch();

    // -1 rather than 0: "no title chosen" is not title bit zero.
    CHECK(handler.getChosenTitleBit() == -1);
    CHECK(handler.getShapeshiftFormId() == 0);
    CHECK(handler.getHonorPoints() == 0);
    CHECK(handler.getArenaPoints() == 0);
    CHECK(handler.getPlayerRestedXp() == 0);
    CHECK(handler.getManaRegen() == 0.0f);
    CHECK(handler.getManaRegenCasting() == 0.0f);
    // A stale sticky transport is a new character riding a boat they are
    // nowhere near: this one deliberately outlives the server's own mention of
    // it by a few seconds, so it has to be cleared explicitly.
    CHECK(handler.playerTransportStickyGuidRef() == 0);
    CHECK(handler.playerTransportStickyTimerRef() == 0.0f);
}

TEST_CASE("a character switch clears the display switches and the corpse's map",
          "[character][switch]") {
    // Found on 2026-08-27 by the sweep this fix came with, once it could see
    // the player-field applier. All five come home in PLAYER_FLAGS or with the
    // corpse - and a character whose flags are all zero has no PLAYER_FLAGS to
    // send, so hiding a helm on one character hid it on the next one who had
    // never touched the switch.
    GameServices services{};
    GameHandler handler(services);

    handler.helmVisibleRef() = false;
    handler.cloakVisibleRef() = false;
    handler.isRestingRef() = true;
    handler.corpseMapIdRef() = 571;      // Northrend
    handler.repopPendingRef() = true;

    handler.resetStateForCharacterSwitch();

    // Both default to visible: hidden is the choice, shown is the absence of one.
    CHECK(handler.helmVisibleRef());
    CHECK(handler.cloakVisibleRef());
    CHECK_FALSE(handler.isRestingRef());
    // canRetrieveCorpse compares this against the map the player is on, so a
    // stale one is a wrong answer about somebody else's corpse.
    CHECK(handler.corpseMapIdRef() == 0u);
    CHECK_FALSE(handler.repopPendingRef());
}

TEST_CASE("a character switch clears the stats the switch already cleared",
          "[character][switch]") {
    // The ones that were already right, so a future tidy-up of that block
    // cannot quietly drop them.
    GameServices services{};
    GameHandler handler(services);

    handler.playerCritPctRef() = 25.0f;
    handler.playerMeleeAPRef() = 900;
    handler.playerXpRef() = 12345;

    handler.resetStateForCharacterSwitch();

    // -1 is "not yet known", which is what the character sheet draws as a
    // blank rather than as a real zero.
    CHECK(handler.getCritPct() == -1.0f);
    CHECK(handler.getMeleeAttackPower() == -1);
    CHECK(handler.getPlayerXp() == 0);
}

TEST_CASE("a character switch clears the previous character's pet", "[character][switch]") {
    // The pet lives in SpellHandler; this checks the switch actually reaches it,
    // which is the wiring the first two faults were on the wrong side of.
    GameServices services{};
    GameHandler handler(services);
    auto& pet = handler.getSpellHandler()->petState();
    pet.spellList = {2649};
    pet.attackPower = 314;

    handler.resetStateForCharacterSwitch();

    CHECK(handler.getPetSpells().empty());
    CHECK(handler.getPetAttackPower() == 0);
}
