#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

// ============================================================================
// EmoteRegistry - extracted from AnimationController
//
// Owns all static emote data, DBC loading, emote text lookup, and
// animation ID resolution.  Singleton - loaded once on first use.
// ============================================================================

struct EmoteInfo {
    uint32_t animId = 0;
    uint32_t dbcId = 0;
    bool loop = false;
    std::string textNoTarget;
    std::string textTarget;
    std::string othersNoTarget;
    std::string othersTarget;
    std::string command;
};

class EmoteRegistry {
public:
    static EmoteRegistry& instance();

    /// Where the emote tables are read from. Set once, by Application.
    ///
    /// This registry is a process-wide singleton and most of what reads it are
    /// static lookups with no instance to carry anything - so it holds the
    /// asset manager rather than taking one per call. It used to ask
    /// core::Application::getInstance() for it, which is what made a table of
    /// emote text part of the rendering -> core edge in the library graph.
    /// Same shape as pipeline::setActiveDBCLayout.
    void setAssetManager(pipeline::AssetManager* assetManager);

    /// Load emotes from DBC files (called once on first use).
    ///
    /// With no asset manager set it loads the fallback set, which is what it
    /// did when Application had not built one yet.
    void loadFromDbc();

    struct EmoteResult { uint32_t animId; bool loop; };

    /// Look up an emote by chat command (e.g. "dance", "wave").
    [[nodiscard]] std::optional<EmoteResult> findEmote(const std::string& command) const;


    /// Get the animation ID for an Emotes.dbc emote ID, as used by SMSG_EMOTE
    /// and UNIT_NPC_EMOTESTATE.
    [[nodiscard]] uint32_t animByEmotesId(uint32_t emoteId) const;

    /// True if the Emotes.dbc entry is a persistent state emote (EmoteSpecProc
    /// != 0, e.g. STATE_WORK_NOSHEATHE) rather than a one-shot (ONESHOT_WAVE).
    [[nodiscard]] bool isStateEmote(uint32_t emoteId) const;

    /// Get the emote state variant (looping) for a one-shot emote animation.
    [[nodiscard]] uint32_t getStateVariant(uint32_t oneShotAnimId) const;

    /// Get first-person emote text for a command.
    std::string textFor(const std::string& emoteName,
                        const std::string* targetName = nullptr) const;

    /// Get DBC ID for an emote command.
    [[nodiscard]] uint32_t dbcIdFor(const std::string& emoteName) const;

    /// Get third-person emote text by DBC ID.
    std::string textByDbcId(uint32_t dbcId,
                            const std::string& senderName,
                            const std::string* targetName = nullptr) const;


private:
    EmoteRegistry() = default;
    EmoteRegistry(const EmoteRegistry&) = delete;
    EmoteRegistry& operator=(const EmoteRegistry&) = delete;

    void loadFallbackEmotes();
    void buildDbcIdIndex();

    pipeline::AssetManager* assetManager_ = nullptr;
    bool loaded_ = false;
    std::unordered_map<std::string, EmoteInfo> emoteTable_;
    std::unordered_map<uint32_t, const EmoteInfo*> emoteByDbcId_;
    std::unordered_map<uint32_t, uint32_t> animByEmotesId_;
    std::unordered_set<uint32_t> stateEmoteIds_;
};

} // namespace rendering
} // namespace wowee
