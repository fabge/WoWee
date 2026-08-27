// EntitySpawner's side of ui::RenderLocator. See include/ui/render_locator.hpp.

#include "ui/render_locator.hpp"

#include "core/entity_spawner.hpp"

#include <memory>

namespace wowee::core {

namespace {

class RenderLocatorImpl final : public ui::RenderLocator {
public:
    explicit RenderLocatorImpl(EntitySpawner& spawner) : spawner_(spawner) {}

    bool positionForGuid(uint64_t guid, glm::vec3& outPos) const override {
        return spawner_.getRenderPositionForGuid(guid, outPos);
    }
    bool boundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const override {
        return spawner_.getRenderBoundsForGuid(guid, outCenter, outRadius);
    }
    bool footZForGuid(uint64_t guid, float& outFootZ) const override {
        return spawner_.getRenderFootZForGuid(guid, outFootZ);
    }

private:
    EntitySpawner& spawner_;
};

}  // namespace

std::unique_ptr<ui::RenderLocator> makeRenderLocator(EntitySpawner& spawner) {
    return std::make_unique<RenderLocatorImpl>(spawner);
}

}  // namespace wowee::core
