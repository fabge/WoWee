#pragma once

// Where a unit is on screen.
//
// Three questions src/ui asks about a guid - where it is, how big it is, and
// where its feet are - to place a nameplate, a combat-text float, or a
// selection reticle. They were the last three symbols on the ui -> core edge in
// the library graph, and they are not a reach for the composition root: they are
// a narrow question with a wider answerer.
//
// EntitySpawner answers them, because it is what holds the render instance for
// each guid. Declared here because src/ui owns the question, implemented in
// src/core which already depends on src/ui for sixty-five other symbols, and
// wired by Application into UIServices.

#include <cstdint>

#include <glm/glm.hpp>

namespace wowee::ui {

class RenderLocator {
public:
    virtual ~RenderLocator() = default;

    /// The unit's position in render space. False when it has no instance -
    /// out of range, not yet spawned, or a corpse that has gone.
    [[nodiscard]] virtual bool positionForGuid(uint64_t guid, glm::vec3& outPos) const = 0;

    /// Its bounding sphere, for picking and for sizing a reticle.
    [[nodiscard]] virtual bool boundsForGuid(uint64_t guid, glm::vec3& outCenter,
                                             float& outRadius) const = 0;

    /// The z of the ground under it, which is where a selection circle goes.
    [[nodiscard]] virtual bool footZForGuid(uint64_t guid, float& outFootZ) const = 0;
};

} // namespace wowee::ui
