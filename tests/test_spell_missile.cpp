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
