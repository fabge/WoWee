// Which way a creature faces while it is moving.
//
// A move arrives with one orientation describing the whole of it - a single
// bearing for a path that may curve through a dozen waypoints and turn back on
// itself. Held for the whole move it draws a wolf running backwards down the
// return leg of its patrol with the run cycle playing forwards, which is what
// was reported. Entity now turns along the way it is actually travelling,
// except where the move names a facing to hold.
#include <catch_amalgamated.hpp>

#include "game/entity.hpp"

#include <cmath>

using wowee::game::Entity;

namespace {

// Canonical yaw, as this codebase measures it: atan2(-dy, dx), north at zero.
float canonicalYaw(float dx, float dy) { return std::atan2(-dy, dx); }

// Run a move to completion in sixtieth-of-a-second frames.
void runMove(Entity& e, float seconds) {
    for (int frame = 0; frame < static_cast<int>(seconds * 60.0f); ++frame)
        e.updateMovement(1.0f / 60.0f);
}

}  // namespace

TEST_CASE("a creature turns to face the way it is travelling", "[entity][facing]") {
    Entity e(1);
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    // Sent due east in canonical terms - +y is west, so east is -y - while the
    // move carries the facing it had, which is north.
    e.startMoveTo(0.0f, -10.0f, 0.0f, /*destO=*/0.0f, /*durationSec=*/1.0f);
    runMove(e, 1.0f);
    REQUIRE(e.getOrientation() == Catch::Approx(canonicalYaw(0.0f, -10.0f)).margin(0.05f));
}

TEST_CASE("a path that doubles back turns the creature round", "[entity][facing]") {
    Entity e(2);
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    // Out twenty north and back again. The bearing the move carries is the one
    // from start to finish, which here is nothing at all; what the creature is
    // doing is going one way and then the other.
    const std::vector<std::array<float, 3>> path = {
        {0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    e.startMoveAlongPath(path, /*destO=*/0.0f, /*totalDuration=*/2.0f);

    runMove(e, 0.8f);
    const float outbound = e.getOrientation();
    // Heading north, which canonical yaw calls zero.
    REQUIRE(std::abs(outbound) < 0.3f);

    runMove(e, 1.0f);
    const float inbound = e.getOrientation();
    // And now the other way: south, which is ±pi.
    REQUIRE(std::abs(inbound) > 2.5f);
}

TEST_CASE("a move that names a facing keeps it", "[entity][facing]") {
    Entity e(3);
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    // This is a real player's own reported orientation, or a creature ordered
    // to watch something. Backing away from a target while facing it is a
    // thing that happens, and the travel direction must not overwrite it.
    e.startMoveTo(0.0f, -10.0f, 0.0f, /*destO=*/0.0f, /*durationSec=*/1.0f,
                  /*holdFacing=*/true);
    runMove(e, 1.0f);
    REQUIRE(e.getOrientation() == Catch::Approx(0.0f));
}

TEST_CASE("standing still does not spin a creature", "[entity][facing]") {
    Entity e(4);
    e.setPosition(0.0f, 0.0f, 0.0f, 1.0f);
    // A heartbeat with nowhere to go: startMoveTo treats it as a stop, and
    // nothing about the facing may move off the orientation it came with.
    e.startMoveTo(0.0f, 0.0f, 0.0f, /*destO=*/1.0f, /*durationSec=*/1.0f);
    runMove(e, 1.0f);
    REQUIRE(e.getOrientation() == Catch::Approx(1.0f));
}
