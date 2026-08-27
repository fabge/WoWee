#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;
class Renderer;
class CharacterRenderer;

// A missile that would take longer than this is drawn arriving sooner: a shot
// across a whole zone should not leave art hanging in the air for seconds.
inline constexpr float kSpellMissileMaxDuration = 3.0f;
// And one at point-blank range still gets a frame or two of flight rather than
// a flicker.
inline constexpr float kSpellMissileMinDuration = 0.05f;

/// How long a missile crossing `distance` world units at `speed` is drawn
/// travelling, in seconds.
///
/// `speed` is Spell.dbc's Speed, in yards per second, and a yard is a world
/// unit. Returns 0 for a speed at or below zero - the spell has no missile and
/// lands the instant it is cast - and for a distance too small to aim along.
[[nodiscard]] float spellMissileDuration(float distance, float speed);

/// The euler rotation that points a missile down its own flight path.
///
/// Missiles fly straight, so this holds for the whole flight, which is why a
/// launched missile never needs its rotation set again.
[[nodiscard]] glm::vec3 spellMissileRotation(const glm::vec3& start, const glm::vec3& end);

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem() = default;

    // Initialize with references to the M2 renderer and parent renderer
    void initialize(M2Renderer* m2Renderer, Renderer* renderer);
    void shutdown();

    // Spawn a spell visual at a world position.
    // useImpactKit=false → CastKit path; useImpactKit=true → ImpactKit path
    // attachInstanceId: the CASTER's CharacterRenderer instance for hand/chest/
    // head bone tracking (0 = static effect at worldPosition). Effects used to
    // attach to the local player unconditionally, so every nearby unit's cast
    // kit landed on the player's hands.
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false, uint32_t attachInstanceId = 0);

    // Spawn a precast visual effect at a world position.
    // castTimeMs: server cast time in milliseconds (0 = use anim duration).
    // attachInstanceId: see playSpellVisual.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t castTimeMs = 0, uint32_t attachInstanceId = 0);

    // Launch a spell's missile from the caster toward the target.
    //
    // SpellVisual.dbc's MissileModel is the art that travels between the two -
    // a Lightning Bolt's bolt, a Frostbolt's shard. It was read only as a
    // stand-in for a missing cast or impact kit, so it was drawn standing
    // still and every projectile spell arrived with nothing in between.
    //
    // speed is Spell.dbc's missile speed in yards per second; a speed at or
    // below zero means the spell has no missile and nothing is launched.
    // Returns true when a missile is in flight, in which case it raises the
    // impact visual at the destination on arrival and the caller must not play
    // that impact itself.
    bool launchSpellMissile(uint32_t visualId, const glm::vec3& start,
                            const glm::vec3& end, float speed);

    // Launch a physical weapon projectile (arrow, bullet, or thrown item)
    // without invoking the spell visual pipeline.
    void playPhysicalProjectile(const std::string& modelPath,
                                const std::string& texturePath,
                                const glm::vec3& start,
                                const glm::vec3& end,
                                float duration,
                                bool spin);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // Remove all active precast visual instances (cast canceled/interrupted).
    void cancelAllPrecastVisuals();

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

private:
    // Spell visual effects - transient M2 instances spawned by SMSG_PLAY_SPELL_VISUAL/IMPACT
    struct SpellVisualInstance {
        uint32_t instanceId;
        float elapsed;
        float duration;  // per-instance lifetime in seconds (from M2 anim or default)
        bool isPrecast;  // true for precast effects (removed on cancel/interrupt)
        uint32_t attachmentId;  // character attachment point to track (0=none/static)
        uint32_t attachInstanceId;  // CharacterRenderer instance the attachment belongs to
        // A missile travels from travelStart to travelEnd over its whole
        // lifetime and raises impactVisualId when it arrives. Everything else
        // leaves these zeroed and holds still.
        bool isMissile = false;
        glm::vec3 travelStart{0.0f};
        glm::vec3 travelEnd{0.0f};
        uint32_t impactVisualId = 0;
    };

    struct PhysicalProjectile {
        uint32_t instanceId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 rotation{0.0f};
        float elapsed = 0.0f;
        float duration = 0.0f;
        bool spin = false;
    };

    void loadSpellVisualDbc();

    /// Resolve a model path to a model the M2 renderer holds, reading and
    /// uploading it on first use. Returns 0 when the model cannot be used; the
    /// path is remembered as failed so the file is not read again this session.
    uint32_t acquireEffectModel(const std::string& modelPath);

    M2Renderer* m2Renderer_ = nullptr;
    Renderer* renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    std::vector<PhysicalProjectile> physicalProjectiles_;
    std::unordered_map<uint32_t, std::string> spellVisualPrecastPath_; // visualId → precast M2 path
    std::unordered_map<uint32_t, std::string> spellVisualCastPath_;   // visualId → cast M2 path
    std::unordered_map<uint32_t, std::string> spellVisualImpactPath_; // visualId → impact M2 path
    std::unordered_map<uint32_t, std::string> spellVisualMissilePath_; // visualId → travelling M2 path
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    bool spellVisualDbcLoaded_ = false;
    static constexpr float SPELL_VISUAL_MAX_DURATION = 5.0f;
    static constexpr float SPELL_VISUAL_DEFAULT_DURATION = 2.0f;

    // Determine character attachment point from model path keywords
    static uint32_t classifyAttachmentId(const std::string& modelPath);

    // Apply height offset based on model path keywords (Hand → hands, Chest → chest, Base → ground)
    static glm::vec3 applyEffectHeightOffset(const glm::vec3& basePos, const std::string& modelPath);
};

} // namespace rendering
} // namespace wowee
