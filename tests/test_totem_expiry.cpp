#include <catch_amalgamated.hpp>

#include "game/totem_expiry.hpp"

using wowee::game::TotemExpiryWatch;

TEST_CASE("a slot that was never active never expires", "[totem]") {
    TotemExpiryWatch<4> watch;
    CHECK(watch.expired({false, false, false, false}) == 0u);
    CHECK(watch.expired({false, false, false, false}) == 0u);
}

TEST_CASE("placing a totem is not an expiry", "[totem]") {
    TotemExpiryWatch<4> watch;
    CHECK(watch.expired({true, false, false, false}) == 0u);
    CHECK(watch.expired({true, false, false, false}) == 0u);
}

TEST_CASE("a totem running out fires once", "[totem]") {
    // The reported fault: nothing on the wire says a totem has ended, so the
    // icon stayed under the player portrait reading "0 s".
    TotemExpiryWatch<4> watch;
    watch.expired({true, false, false, false});
    CHECK(watch.expired({false, false, false, false}) == 0b0001u);
    CHECK(watch.expired({false, false, false, false}) == 0u);
}

TEST_CASE("each slot is watched independently", "[totem]") {
    TotemExpiryWatch<4> watch;
    watch.expired({true, true, true, true});
    CHECK(watch.expired({true, false, true, false}) == 0b1010u);
    CHECK(watch.expired({false, false, true, false}) == 0b0001u);
}

TEST_CASE("a totem replaced in the same tick is not an expiry", "[totem]") {
    // Dropping a new totem into a slot that still holds one leaves it active
    // throughout, and the frame is updated by SMSG_TOTEM_CREATED instead.
    TotemExpiryWatch<4> watch;
    watch.expired({true, false, false, false});
    CHECK(watch.expired({true, false, false, false}) == 0u);
}

TEST_CASE("a slot can expire more than once over a session", "[totem]") {
    TotemExpiryWatch<4> watch;
    watch.expired({true, false, false, false});
    CHECK(watch.expired({false, false, false, false}) == 0b0001u);
    watch.expired({true, false, false, false});
    CHECK(watch.expired({false, false, false, false}) == 0b0001u);
}
