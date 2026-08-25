#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace wowee::ui {

/// Keeps the command selected on a key press attached to that physical key.
/// Modifiers and bindings can change before release, so resolving twice is not
/// a press/release pair.
class BindingPressTracker {
public:
    void press(int physicalKey, std::string command) {
        active_[physicalKey] = std::move(command);
    }

    [[nodiscard]] bool contains(int physicalKey) const {
        return active_.find(physicalKey) != active_.end();
    }

    std::optional<std::string> release(int physicalKey) {
        const auto it = active_.find(physicalKey);
        if (it == active_.end()) return std::nullopt;
        std::string command = std::move(it->second);
        active_.erase(it);
        return command;
    }

    void clear() { active_.clear(); }

private:
    std::unordered_map<int, std::string> active_;
};

}  // namespace wowee::ui
