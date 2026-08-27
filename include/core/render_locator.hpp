#pragma once

// EntitySpawner's side of ui::RenderLocator. See src/core/render_locator.cpp.

#include <memory>

namespace wowee {
namespace ui { class RenderLocator; }
namespace core {

class EntitySpawner;

/// Build the narrow face src/ui asks "where is this guid" through.
std::unique_ptr<ui::RenderLocator> makeRenderLocator(EntitySpawner& spawner);

}  // namespace core
}  // namespace wowee
