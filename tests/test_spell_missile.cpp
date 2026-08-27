// Spell missile flight arithmetic.
//
// SpellVisual.dbc's MissileModel was read only as a stand-in for a missing
// cast or impact kit, so a Shaman's Lightning Bolt was a glowing ball at the
// target with nothing between it and the caster. The model now flies, and
// these two functions decide how long it takes and which way it points.
//
// Both are pure, so they are testable without a Vulkan device; what they are
// used by is not.
#include <catch_amalgamated.hpp>

#include "rendering/spell_visual_system.hpp"

#include <glm/gtc/constants.hpp>

using wowee::rendering::kSpellMissileMaxDuration;
using wowee::rendering::kSpellMissileMinDuration;
using wowee::rendering::advanceSpellMissile;
using wowee::rendering::spellMissileDuration;
using wowee::rendering::spellMissileRotation;

TEST_CASE("a spell with no missile speed launches nothing", "[spell][missile]") {
    // Spell.dbc gives every instant spell a Speed of zero - Heroic Strike,
    // War Stomp, every self-buff. A zero duration is how the launch declines.
    REQUIRE(spellMissileDuration(30.0f, 0.0f) == 0.0f);
    REQUIRE(spellMissileDuration(30.0f, -1.0f) == 0.0f);
}

TEST_CASE("missile flight time is distance over speed", "[spell][missile]") {
    // Lightning Bolt's Speed is 20 yards per second, and a yard is a world
    // unit: 30 units of air is a second and a half of flight.
    REQUIRE(spellMissileDuration(30.0f, 20.0f) == Catch::Approx(1.5f));
    // Frostbolt is faster at 28.
    REQUIRE(spellMissileDuration(28.0f, 28.0f) == Catch::Approx(1.0f));
}

TEST_CASE("missile flight time is clamped at both ends", "[spell][missile]") {
    // A shot across a whole zone must not leave art hanging in the air.
    REQUIRE(spellMissileDuration(5000.0f, 20.0f) == Catch::Approx(kSpellMissileMaxDuration));
    // And one at arm's length still gets a frame or two rather than a flicker.
    REQUIRE(spellMissileDuration(0.5f, 100.0f) == Catch::Approx(kSpellMissileMinDuration));
}

TEST_CASE("a target in the caster's own position is not shot at", "[spell][missile]") {
    // There is no path to fly and no direction to face along, and the yaw
    // would come out of atan2(0, 0).
    REQUIRE(spellMissileDuration(0.0f, 20.0f) == 0.0f);
}

TEST_CASE("a missile points down its own flight path", "[spell][missile]") {
    const glm::vec3 origin(0.0f);

    // Due +x is yaw zero and level.
    const glm::vec3 east = spellMissileRotation(origin, glm::vec3(10.0f, 0.0f, 0.0f));
    REQUIRE(east.z == Catch::Approx(0.0f));
    REQUIRE(east.y == Catch::Approx(0.0f));

    // Due +y is a quarter turn.
    const glm::vec3 north = spellMissileRotation(origin, glm::vec3(0.0f, 10.0f, 0.0f));
    REQUIRE(north.z == Catch::Approx(glm::half_pi<float>()));

    // A target above the caster pitches the model up, which in this convention
    // is a negative pitch.
    const glm::vec3 up = spellMissileRotation(origin, glm::vec3(10.0f, 0.0f, 10.0f));
    REQUIRE(up.y == Catch::Approx(-glm::quarter_pi<float>()));
    // ...and one below pitches it down by the same amount.
    const glm::vec3 down = spellMissileRotation(origin, glm::vec3(10.0f, 0.0f, -10.0f));
    REQUIRE(down.y == Catch::Approx(glm::quarter_pi<float>()));
}

// A missile that homes is the whole point of carrying the target's instance
// through the launch, and the step is where that happens: the aim point is
// re-read every frame, so this function is asked about a target that moves.
TEST_CASE("a missile closes on its target at its own speed", "[spell][missile]") {
    glm::vec3 pos(0.0f, 0.0f, 0.0f);
    // 20 units per second, a sixtieth of a second: a third of a unit.
    REQUIRE_FALSE(advanceSpellMissile(pos, glm::vec3(30.0f, 0.0f, 0.0f), 20.0f, 1.0f / 60.0f));
    REQUIRE(pos.x == Catch::Approx(20.0f / 60.0f));
    REQUIRE(pos.y == Catch::Approx(0.0f));
}

TEST_CASE("a missile follows a target that moves", "[spell][missile]") {
    // Launched down +x at something thirty out, which then walks ten to the
    // side. Every step aims at where it is now, so the missile turns rather
    // than carrying on to the empty ground it was first aimed at - which is
    // the bug this was written for: the bolt landed beside the wolf.
    glm::vec3 pos(0.0f, 0.0f, 0.0f);
    glm::vec3 target(30.0f, 0.0f, 0.0f);
    for (int frame = 0; frame < 10; ++frame)
        (void)advanceSpellMissile(pos, target, 20.0f, 1.0f / 60.0f);
    REQUIRE(pos.y == Catch::Approx(0.0f));

    target = glm::vec3(30.0f, 10.0f, 0.0f);
    for (int frame = 0; frame < 10; ++frame)
        (void)advanceSpellMissile(pos, target, 20.0f, 1.0f / 60.0f);
    // It has started to turn, and it is turning the way the target went.
    REQUIRE(pos.y > 0.0f);
}

TEST_CASE("a missile arrives rather than overshooting", "[spell][missile]") {
    // The step is longer than what is left. Landing on the target and saying
    // so is right; stepping past it and turning round to come back is not.
    glm::vec3 pos(0.0f, 0.0f, 0.0f);
    const glm::vec3 target(1.0f, 0.0f, 0.0f);
    REQUIRE(advanceSpellMissile(pos, target, 100.0f, 1.0f / 60.0f));
    REQUIRE(pos.x == Catch::Approx(target.x));

    // And a target it is already standing on is an arrival, not a division by
    // a length of zero.
    glm::vec3 same(5.0f, 5.0f, 5.0f);
    REQUIRE(advanceSpellMissile(same, same, 20.0f, 1.0f / 60.0f));
}
