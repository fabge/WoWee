#pragma once

#include <array>
#include <cstddef>

namespace wowee::game {

/// Which totem slots have just stopped holding a totem.
///
/// A totem running out is not announced by anything: SMSG_TOTEM_CREATED is the
/// only totem message this client receives, so the interface is told when one
/// is placed and never when one ends. FrameXML's TotemFrame shows a button on
/// PLAYER_TOTEM_UPDATE and hides it on the same event, and its OnUpdate only
/// rewrites the duration text - so with no event the icon stayed under the
/// player portrait with "0 s" beneath it for the rest of the session.
///
/// The same shape as a spell cooldown ending, which is handled a few lines
/// away in SpellHandler::updateTimers for the same reason.
///
/// Edge-triggered on purpose: the event fires on the tick a slot goes from
/// holding a totem to not, and never again, so a slot that has been empty for
/// ten minutes is silent.
template <std::size_t N>
class TotemExpiryWatch {
public:
    /// The slots that were active last call and are not now, as a bitmask
    /// where bit i is slot i.
    unsigned expired(const std::array<bool, N>& active) {
        unsigned mask = 0;
        for (std::size_t i = 0; i < N; ++i) {
            if (wasActive_[i] && !active[i]) mask |= (1u << i);
            wasActive_[i] = active[i];
        }
        return mask;
    }

private:
    std::array<bool, N> wasActive_{};
};

}  // namespace wowee::game
