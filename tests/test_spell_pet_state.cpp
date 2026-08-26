// Pet state across a character switch.
//
// SMSG_PET_SPELLS is the only thing that has ever populated or cleared the
// pet's spell list, autocast set and action bar, and it arrives when a pet is
// summoned or dismissed. A character that has never had a pet is never sent
// it - so logging out of a hunter and into a mage left the hunter's pet spells
// in place. GameHandler zeroed petGuid_ on the switch, which is what kept the
// stale pet off the screen, but every other reader still answered from it.
//
// Reachable as a test at all because the pet state moved from GameHandler into
// SpellHandler on 2026-08-26; before that, covering it meant naming 59
// translation units.
#include <catch_amalgamated.hpp>

#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/spell_handler.hpp"

using wowee::game::GameHandler;
using wowee::game::GameServices;
using wowee::game::SpellHandler;

TEST_CASE("a character switch clears the previous character's pet", "[spell][pet]") {
    GameServices services{};
    GameHandler handler(services);
    SpellHandler& spells = *handler.getSpellHandler();

    // What SMSG_PET_SPELLS leaves behind for a hunter with a pet out.
    auto& pet = spells.petState();
    pet.spellList = {2649, 17253};
    pet.autocastSpells.insert(2649);
    pet.actionSlots[0] = 2649;
    pet.command = 2;   // attack
    pet.react = 2;     // aggressive

    REQUIRE(handler.getPetSpells().size() == 2);
    REQUIRE(handler.isPetSpellAutocast(2649));

    spells.resetAllState();

    // Nothing of the previous character's pet survives.
    CHECK(handler.getPetSpells().empty());
    CHECK_FALSE(handler.isPetSpellAutocast(2649));
    CHECK(handler.getPetActionSlot(0) == 0);
    // Back to the defaults SMSG_PET_SPELLS would otherwise overwrite, not to
    // the previous pet's stance.
    CHECK(handler.getPetCommand() == 1);   // follow
    CHECK(handler.getPetReact() == 1);     // defensive
}

TEST_CASE("an out-of-range pet action slot answers zero rather than reading past the bar",
          "[spell][pet]") {
    GameServices services{};
    GameHandler handler(services);
    SpellHandler& spells = *handler.getSpellHandler();
    spells.petState().actionSlots[0] = 42;

    CHECK(handler.getPetActionSlot(0) == 42);
    CHECK(handler.getPetActionSlot(-1) == 0);
    CHECK(handler.getPetActionSlot(SpellHandler::PetState::kActionBarSlots) == 0);
    CHECK(handler.getPetActionSlot(9999) == 0);
}
