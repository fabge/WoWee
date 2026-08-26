// pipeline_registry.hpp - the subsystems whose pipelines embed the render pass.
//
// A Vulkan graphics pipeline bakes in its render pass and sample count, so
// every subsystem that owns one has to rebuild when MSAA changes or the
// swapchain is recreated. Thirty-five headers declare recreatePipelines() and
// Renderer called them from a hand-written enumeration sixty lines long.
//
// The failure that enumeration produces is silent and specific: a subsystem
// added to the renderer and left out of the list keeps its old pipeline, which
// is bound to a destroyed render pass. Nothing warns. It draws with a stale
// pass until the driver loses the device, and the symptom - a crash a fraction
// of a second after an anti-aliasing change - looks nothing like a missing
// line in a list.
//
// So the list is registered rather than written out, next to where each
// subsystem is created, and tools/render_pipeline_registry_check.py fails the
// build if a type declaring recreatePipelines() is never registered. The
// registry preserves registration order, because some entries depend on
// earlier ones having rebuilt first.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wowee::rendering {

class PipelineRegistry {
public:
    /// Register one subsystem's rebuild. `name` is for diagnostics and for the
    /// sweep; `rebuild` is called in registration order.
    ///
    /// Most entries are a bare recreatePipelines(). A few do more - the water
    /// renderer rebuilds its single-sampled pass, the swim effects resync their
    /// target pass first - and those register the whole of what they need,
    /// which keeps the ordering visible at the point it matters.
    void add(std::string name, std::function<void()> rebuild) {
        entries_.emplace_back(std::move(name), std::move(rebuild));
    }

    /// Rebuild everything, in registration order.
    void rebuildAll() const {
        for (const auto& e : entries_) {
            if (e.second) e.second();
        }
    }

    [[nodiscard]] size_t size() const { return entries_.size(); }
    [[nodiscard]] const std::vector<std::pair<std::string, std::function<void()>>>&
    entries() const { return entries_; }

    void clear() { entries_.clear(); }

private:
    std::vector<std::pair<std::string, std::function<void()>>> entries_;
};

}  // namespace wowee::rendering
