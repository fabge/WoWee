// A cooldown frame paints its own sweep, so it has to reach the draw order.
//
// FrameXML declares every action button's cooldown as
// `<Cooldown name="$parentCooldown">` with no backdrop and no texture of its
// own, and the draw order drops a Frame with nothing of its own to paint as a
// container. So drawCooldown was never reached and every cooldown in the game
// was invisible on the button - readable only in the tooltip, which counts the
// seconds down a different path and is why it looked like a display quirk
// rather than a missing draw.
//
// A status bar is the precedent: it paints its fill, so it is exempt, and it
// is exempt only when it has a fill to paint. A cooldown is exempt only while
// one is running.
#include <catch_amalgamated.hpp>

#include "ui/widget_tree.hpp"

#include <algorithm>

using wowee::ui::WidgetTree;
using wowee::ui::WidgetKind;

namespace {

/// A shown, anchored, sized cooldown frame parented to the root.
uint32_t makeCooldown(WidgetTree& tree) {
    const uint32_t id = tree.create(WidgetKind::Frame, tree.uiParentId(), "CDTest");
    auto* w = tree.get(id);
    REQUIRE(w != nullptr);
    w->isCooldown = true;
    w->shown = true;
    w->width = 36.0f;
    w->height = 36.0f;
    w->anchors.push_back({.point = "CENTER",
                          .relativeTo = tree.uiParentId(),
                          .relativePoint = "CENTER",
                          .x = 0.0f,
                          .y = 0.0f});
    return id;
}

bool inDrawOrder(const WidgetTree& tree, uint32_t id) {
    const auto* w = tree.get(id);
    const auto& order = tree.drawOrder();
    return std::find(order.begin(), order.end(), w) != order.end();
}

}  // namespace

TEST_CASE("a running cooldown is drawn", "[ui][cooldown]") {
    WidgetTree tree;
    tree.layout(1920.0f, 1080.0f);
    const uint32_t id = makeCooldown(tree);

    // Idle: nothing to paint, and it stays out of the way like any container.
    tree.layout(1920.0f, 1080.0f);
    REQUIRE_FALSE(inDrawOrder(tree, id));

    // Running: it has a sweep of its own, so it has to be reached.
    tree.get(id)->cooldownStart = 0.0;
    tree.get(id)->cooldownDuration = 30.0;
    tree.layout(1920.0f, 1080.0f);
    REQUIRE(inDrawOrder(tree, id));
}

TEST_CASE("an ordinary frame is still a container", "[ui][cooldown]") {
    // The exemption is for cooldowns, not for every frame - widening it would
    // put every container in the game into the draw order.
    WidgetTree tree;
    tree.layout(1920.0f, 1080.0f);
    const uint32_t id = makeCooldown(tree);
    tree.get(id)->isCooldown = false;
    tree.get(id)->cooldownDuration = 30.0;
    tree.layout(1920.0f, 1080.0f);
    REQUIRE_FALSE(inDrawOrder(tree, id));
}
