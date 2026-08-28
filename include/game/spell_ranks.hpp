#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::game {

/// The number in a spell's rank string, or 0 when it has none.
///
/// Spell.dbc carries the rank as display text - "Rank 4" - and never as a
/// number. A spell with no rank string is the only one of its name, which
/// sorts as rank zero and is correct: it is its own highest rank.
inline int spellRankNumber(const std::string& rankText) {
    if (rankText.empty()) return 0;
    // Case-folded because the column is display text and the locale decides
    // its capitalisation.
    std::string lowered = rankText;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered.rfind("rank ", 0) != 0) return 0;
    int value = 0;
    for (size_t i = 5; i < lowered.size(); ++i) {
        const char c = lowered[i];
        if (c < '0' || c > '9') break;
        value = value * 10 + (c - '0');
    }
    return value;
}

/// Move the highest known rank of each spell name to the front, keeping the
/// order within each group, and answer how many there are.
///
/// This is what "Show all spell ranks" turns off. GetSpellTabInfo hands
/// FrameXML two ranges into one flat list - the whole tab, and the highest-rank
/// subset - and SpellBook_GetTabInfo keeps the second whenever the box is
/// unticked, which is the default. Both have to be contiguous, so the list is
/// ordered rather than filtered: the highest ranks first, then everything else.
///
/// It answered the whole tab for both, so the box did nothing and every rank of
/// every spell was listed - two Lightning Bolts, two Earth Shocks - which is
/// also why nothing was upgrading on the action bar.
inline size_t partitionHighestRanks(
        std::vector<uint32_t>& spellIds,
        const std::function<std::string(uint32_t)>& nameOf,
        const std::function<int(uint32_t)>& rankOf) {
    // The best rank seen for each name. Names rather than spell ids, because
    // ranks of one spell are separate ids and that is the whole point.
    std::unordered_map<std::string, int> best;
    for (uint32_t id : spellIds) {
        const std::string name = nameOf(id);
        const int rank = rankOf(id);
        auto it = best.find(name);
        if (it == best.end() || rank > it->second) best[name] = rank;
    }
    const auto isHighest = [&](uint32_t id) {
        const auto it = best.find(nameOf(id));
        return it != best.end() && rankOf(id) == it->second;
    };
    // Stable, so the name ordering the caller already applied survives.
    const auto split = std::stable_partition(spellIds.begin(), spellIds.end(), isHighest);
    return static_cast<size_t>(std::distance(spellIds.begin(), split));
}

}  // namespace wowee::game
