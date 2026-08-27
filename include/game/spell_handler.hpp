#pragma once

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "game/handler_types.hpp"
#include "audio/spell_sound_manager.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

class SpellHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit SpellHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // Talent data structures (aliased from handler_types.hpp)
    using TalentEntry = game::TalentEntry;
    using TalentTabEntry = game::TalentTabEntry;

    // --- Spell book tabs ---
    struct SpellBookTab {
        std::string name;
        std::string texture; // icon path
        std::vector<uint32_t> spellIds; // spells in this tab
    };

    // Unit cast state (aliased from handler_types.hpp)
    using UnitCastState = game::UnitCastState;

    // Equipment set info (aliased from handler_types.hpp)
    using EquipmentSetInfo = game::EquipmentSetInfo;

    // ---- Spell-domain types ----
    //
    // These described state that lived in GameHandler and is now held here, so
    // they moved with it. GameHandler keeps an alias for each, because its own
    // setters still name them.
    // Talent-driven spell modifiers (SMSG_SET_FLAT_SPELL_MODIFIER / SMSG_SET_PCT_SPELL_MODIFIER)
    // SpellModOp matches WotLK SpellModOp enum (server-side).
    enum class SpellModOp : uint8_t {
        Damage            =  0,
        Duration          =  1,
        Threat            =  2,
        Effect1           =  3,
        Charges           =  4,
        Range             =  5,
        Radius            =  6,
        CritChance        =  7,
        AllEffects        =  8,
        NotLoseCastingTime =  9,
        CastingTime       = 10,
        Cooldown          = 11,
        Effect2           = 12,
        IgnoreArmor       = 13,
        Cost              = 14,
        CritDamageBonus   = 15,
        ResistMissChance  = 16,
        JumpTargets       = 17,
        ChanceOfSuccess   = 18,
        ActivationTime    = 19,
        // From here the list was the modern one - Efficiency, MultipleValue,
        // ResistPushback and the rest are Cataclysm names - grafted onto a
        // 3.3.5 head. Nothing read them, because only Cost and CastingTime are
        // consumed, but the numbers are what the server sends: a talent that
        // modifies a periodic effect arrives as op 22, and a table calling
        // that ResistDispelChance would have applied it to dispel resistance.
        DamageMultiplier  = 20,
        GlobalCooldown    = 21,
        Dot               = 22,
        Effect3           = 23,
        BonusMultiplier   = 24,
        // 25 is not used by this client version.
        ProcPerMinute     = 26,
        ValueMultiplier   = 27,
        ResistDispelChance = 28,
        CritDamageBonus2  = 29,
        SpellCostRefundOnFail = 30,
    };
    static constexpr int SPELL_MOD_OP_COUNT = 32;
    // Shaman totems (4 slots: 0=Earth, 1=Fire, 2=Water, 3=Air)
    struct TotemSlot {
        uint32_t spellId     = 0;
        uint32_t durationMs  = 0;
        std::chrono::steady_clock::time_point placedAt{};
        [[nodiscard]] bool active() const { return spellId != 0 && remainingMs() > 0; }
        [[nodiscard]] float remainingMs() const {
            if (spellId == 0 || durationMs == 0) return 0.0f;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - placedAt).count();
            float rem = static_cast<float>(durationMs) - static_cast<float>(elapsed);
            return rem > 0.0f ? rem : 0.0f;
        }
    };
    static constexpr int NUM_TOTEM_SLOTS = 4;
    // Key: (SpellModOp, groupIndex) - value: accumulated flat or pct modifier
    // pct values are stored in integer percent (e.g. -20 means -20% reduction).
    struct SpellModKey {
        SpellModOp op;
        uint8_t    group;
        bool operator==(const SpellModKey& o) const {
            return op == o.op && group == o.group;
        }
    };
    struct SpellModKeyHash {
        std::size_t operator()(const SpellModKey& k) const {
            return std::hash<uint32_t>()(
                (static_cast<uint32_t>(static_cast<uint8_t>(k.op)) << 8) | k.group);
        }
    };
    // Achievement earned callback - fires when SMSG_ACHIEVEMENT_EARNED is received
    using AchievementEarnedCallback = std::function<void(uint32_t achievementId, const std::string& name)>;
    // Charge callback - fires when player casts a charge spell toward target
    // Parameters: targetGuid, targetX, targetY, targetZ (canonical WoW coordinates)
    using ChargeCallback = std::function<void(uint64_t targetGuid, float x, float y, float z)>;
    // Called when the player starts casting Hearthstone so terrain at the bind
    // point can be pre-loaded during the cast time.
    // Parameters: mapId and canonical (x, y, z) of the bind location.
    using HearthstonePreloadCallback = std::function<void(uint32_t mapId, float x, float y, float z)>;
    // Spell cast animation callbacks - true=start cast/channel, false=finish/cancel
    // guid: caster (may be player or another unit), isChannel: channel vs regular cast
    // castType: DIRECTED (unit target), OMNI (self/no target), AREA (ground AoE)
    using SpellCastAnimCallback = std::function<void(uint64_t guid, bool start, bool isChannel,
                                                      SpellCastType castType)>;
    // Fired when the player's own spell cast fails (spellId of the failed spell).
    using SpellCastFailedCallback = std::function<void(uint32_t spellId)>;
    // Sprint aura callback - fired when sprint-type aura active state changes on player
    using SprintAuraCallback = std::function<void(bool active)>;

    // ---- Accessors for the state that moved here from GameHandler ----
    //
    // GameHandler's public API is unchanged: each of its setters and getters
    // for this state now forwards to one of these. What went away is the
    // xRef() accessor that handed this handler a mutable reference to a member
    // of its owner.
    void setAchievementEarnedCallback(AchievementEarnedCallback cb) { achievementEarnedCallback_ = std::move(cb); }
    void setChargeCallback(ChargeCallback cb) { chargeCallback_ = std::move(cb); }
    void setHearthstonePreloadCallback(HearthstonePreloadCallback cb) { hearthstonePreloadCallback_ = std::move(cb); }
    void setSpellCastAnimCallback(SpellCastAnimCallback cb) { spellCastAnimCallback_ = std::move(cb); }
    void setSpellCastFailedCallback(SpellCastFailedCallback cb) { spellCastFailedCallback_ = std::move(cb); }
    void setSprintAuraCallback(SprintAuraCallback cb) { sprintAuraCallback_ = std::move(cb); }

    [[nodiscard]] const TotemSlot& getActiveTotem(int slot) const {
        static const TotemSlot empty{};
        return (slot >= 0 && slot < NUM_TOTEM_SLOTS) ? activeTotemSlots_[slot] : empty;
    }
    [[nodiscard]] const std::string& getSkillDescription(uint32_t skillId) const {
        static const std::string kNone;
        auto it = skillLineDescriptions_.find(skillId);
        return it != skillLineDescriptions_.end() ? it->second : kNone;
    }
    /// Clear the DBC loaded-flags, so the next read reloads.
    void resetDbcLoadFlags() {
        spellNameCacheLoaded_ = false;
        skillLineDbcLoaded_ = false;
        skillLineAbilityLoaded_ = false;
    }
    [[nodiscard]] bool hasPlayerExploredZoneMasks() const { return hasPlayerExploredZones_; }
    [[nodiscard]] const std::unordered_map<uint32_t, PlayerSkill>& getPlayerSkills() const { return playerSkills_; }
    [[nodiscard]] uint8_t getStableSlots() const { return stableNumSlots_; }

    /// Sum of every flat modifier for one op, across all groups.
    [[nodiscard]] int32_t getSpellFlatMod(SpellModOp op) const {
        int32_t total = 0;
        for (const auto& [k, v] : spellFlatMods_)
            if (k.op == op) total += v;
        return total;
    }
    /// The same for percentage modifiers, in percent.
    [[nodiscard]] int32_t getSpellPctMod(SpellModOp op) const {
        int32_t total = 0;
        for (const auto& [k, v] : spellPctMods_)
            if (k.op == op) total += v;
        return total;
    }

    // ---- Pet state ----
    //
    // Held here rather than in GameHandler, which never touched any of it: the
    // five members were reached only through GameHandler::petXRef(), by this
    // handler and by CombatHandler. Grouped into one struct so the surface
    // CombatHandler shares is a single name instead of five loose accessors,
    // and so the collaboration reads as CombatHandler talking to SpellHandler
    // rather than as both of them reaching through a third object.
    struct PetState {
        static constexpr int kActionBarSlots = 10;
        uint32_t actionSlots[kActionBarSlots] = {};  // SMSG_PET_SPELLS action bar
        uint8_t  command = 1;                        // 0=stay,1=follow,2=attack,3=dismiss
        uint8_t  react   = 1;                        // 0=passive,1=defensive,2=aggressive
        std::vector<uint32_t> spellList;             // known pet spells
        std::unordered_set<uint32_t> autocastSpells; // spells with autocast on

        // The pet's own numbers, off the pet unit's update fields. Armor is
        // resistance index 0, as it is for the player.
        //
        // Here rather than in GameHandler for the reason the five above are:
        // nothing but SMSG_PET_* and the pet unit's fields ever writes them,
        // and nothing cleared them on a character switch. GameHandler declared
        // them, exposed an xRef() and read them in a getter, and its own
        // translation units never mentioned them once - so a hunter's pet
        // attack power was still readable after logging into a mage. In here
        // they are zeroed with the rest by resetAllState.
        std::array<int32_t, 5> stats{};
        std::array<int32_t, 7> resistances{};
        int32_t attackPower = 0;
        float   minDamage = 0.0f;
        float   maxDamage = 0.0f;
        uint32_t experience = 0;
        uint32_t nextLevelExp = 0;
    };
    [[nodiscard]] PetState& petState() { return pet_; }
    [[nodiscard]] const PetState& petState() const { return pet_; }

    [[nodiscard]] uint32_t getPetActionSlot(int idx) const {
        if (idx < 0 || idx >= PetState::kActionBarSlots) return 0;
        return pet_.actionSlots[idx];
    }
    [[nodiscard]] uint8_t getPetCommand() const { return pet_.command; }
    [[nodiscard]] const std::array<int32_t, 5>& getPetStats() const { return pet_.stats; }
    [[nodiscard]] const std::array<int32_t, 7>& getPetResistances() const { return pet_.resistances; }
    [[nodiscard]] int32_t getPetAttackPower() const { return pet_.attackPower; }
    [[nodiscard]] float getPetMinDamage() const { return pet_.minDamage; }
    [[nodiscard]] float getPetMaxDamage() const { return pet_.maxDamage; }
    [[nodiscard]] uint32_t getPetExperience() const { return pet_.experience; }
    [[nodiscard]] uint32_t getPetNextLevelExp() const { return pet_.nextLevelExp; }
    [[nodiscard]] uint8_t getPetReact() const { return pet_.react; }
    [[nodiscard]] const std::vector<uint32_t>& getPetSpells() const { return pet_.spellList; }
    [[nodiscard]] bool isPetSpellAutocast(uint32_t spellId) const {
        return pet_.autocastSpells.count(spellId) != 0;
    }

    // Temporary weapon enchant timers (from SMSG_ITEM_ENCHANT_TIME_UPDATE)
    // Slot: 0=main-hand, 1=off-hand, 2=ranged. Value: expire time (steady_clock ms).
    struct TempEnchantTimer {
        uint32_t slot     = 0;
        uint64_t expireMs = 0;   // std::chrono::steady_clock ms timestamp when it expires
    };
    /// Temporary weapon enchants and when they run out.
    ///
    /// Written here - a temporary enchant arrives as a spell effect - and read
    /// by InventoryHandler for the item tooltip, which is why it sat on
    /// GameHandler with an accessor for each of them.
    [[nodiscard]] const std::vector<TempEnchantTimer>& getTempEnchantTimers() const {
        return tempEnchantTimers_;
    }
    [[nodiscard]] std::vector<TempEnchantTimer>& tempEnchantTimers() { return tempEnchantTimers_; }

    // --- Public API (delegated from GameHandler) ---
    void castSpell(uint32_t spellId, uint64_t targetGuid = 0);

    /// Spell.dbc EffectImplicitTargetA, or 0 when the spell is unknown. 21 means
    /// the spell has to be aimed at a friendly unit.
    [[nodiscard]] uint32_t getSpellImplicitTargetA(uint32_t spellId) const;
    /// Whether Spell.dbc has this spell at all.
    ///
    /// A server may cast something the client's own data has never heard of -
    /// a private core's custom item does it routinely - and the answer to
    /// "what does this aim at" is then not zero but unknown. The two are worth
    /// telling apart: zero means the spell says nothing, unknown means we do.
    [[nodiscard]] bool isSpellKnownToClient(uint32_t spellId) const;

    /// The last spell the player cast while on foot. When mounting is detected,
    /// this identifies which of the player's indefinite self-cast auras is the
    /// mount - scanning for one blindly can land on a racial or a tracking buff.
    [[nodiscard]] uint32_t getLastGroundCastSpellId() const { return lastGroundCastSpellId_; }

    /// Record a spell cast by using an item - pre-WotLK mounts are items, and
    /// their on-use spell never passes through castSpell().
    void noteGroundCastSpell(uint32_t spellId) { lastGroundCastSpellId_ = spellId; }
    void cancelCast();
    void cancelAura(uint32_t spellId);

    // Known spells
    [[nodiscard]] const std::unordered_set<uint32_t>& getKnownSpells() const { return knownSpells_; }
    [[nodiscard]] const std::unordered_map<uint32_t, float>& getSpellCooldowns() const { return spellCooldowns_; }
    [[nodiscard]] float getSpellCooldown(uint32_t spellId) const;
    [[nodiscard]] float getSpellCooldownTotal(uint32_t spellId) const;

    // Cast state
    [[nodiscard]] bool isCasting() const { return casting_ || restorationActive_; }
    [[nodiscard]] bool isChanneling() const { return casting_ ? castIsChannel_ : restorationActive_; }
    [[nodiscard]] bool isRestoring() const { return restorationActive_; }
    [[nodiscard]] bool isGameObjectInteractionCasting() const;
    [[nodiscard]] uint32_t getCurrentCastSpellId() const {
        return casting_ ? currentCastSpellId_ : restorationSpellId_;
    }
    [[nodiscard]] float getCastProgress() const {
        const float total = casting_ ? castTimeTotal_ : restorationTimeTotal_;
        const float remaining = casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
        return total > 0.0f ? (total - remaining) / total : 0.0f;
    }
    [[nodiscard]] float getCastTimeRemaining() const {
        return casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
    }
    [[nodiscard]] float getCastTimeTotal() const {
        return casting_ ? castTimeTotal_ : restorationTimeTotal_;
    }

    // Repeat-craft queue
    void startCraftQueue(uint32_t spellId, int count);
    void cancelCraftQueue();
    [[nodiscard]] int getCraftQueueRemaining() const { return craftQueueRemaining_; }
    [[nodiscard]] uint32_t getCraftQueueSpellId() const { return craftQueueSpellId_; }

    // Crafting window (client-side; opened by casting a profession spell
    // like Cooking or First Aid - see tradeskillOpenerSkillLine)
    [[nodiscard]] bool isCraftingWindowOpen() const { return craftingWindowOpen_; }
    [[nodiscard]] uint32_t getCraftingSkillLine() const { return craftingSkillLine_; }
    /// Opening and closing a profession announce themselves, because the
    /// interface's trade skill panel is driven entirely by these two events -
    /// it hides on TRADE_SKILL_CLOSE and fills itself on TRADE_SKILL_SHOW.
    /// Without them the panel could be complete and still never appear.
    void openCraftingWindow(uint32_t skillLine);
    void closeCraftingWindow();
    // Returns the skill line id if spellId is a tradeskill-window opener
    // (e.g. Cooking → 185) with at least one known recipe, else 0.
    uint32_t tradeskillOpenerSkillLine(uint32_t spellId);

    // SpellFocusObject.dbc name ("Anvil", "Cooking Fire", ...) for
    // requires-spell-focus cast failures; empty if unknown.
    const std::string& getSpellFocusName(uint32_t focusId);

    // TotemCategory.dbc name ("Blacksmith Hammer", "Mining Pick", ...) for
    // totem-category cast failures; empty if unknown.
    const std::string& getTotemCategoryName(uint32_t categoryId);

    // Spell queue (400ms window)
    [[nodiscard]] uint32_t getQueuedSpellId() const { return queuedSpellId_; }
    void cancelQueuedSpell() { queuedSpellId_ = 0; queuedSpellTarget_ = 0; }

    // Unit cast state (tracked per GUID for target frame + boss frames)
    [[nodiscard]] const UnitCastState* getUnitCastState(uint64_t guid) const {
        auto it = unitCastStates_.find(guid);
        return (it != unitCastStates_.end() && it->second.casting) ? &it->second : nullptr;
    }
    void clearUnitCastStates() { unitCastStates_.clear(); }
    void removeUnitCastState(uint64_t guid) { unitCastStates_.erase(guid); }

    // Aura cache mutation (formerly accessed via friend)
    void clearUnitAurasCache() { unitAurasCache_.clear(); }
    void removeUnitAuraCache(uint64_t guid) { unitAurasCache_.erase(guid); }

    // Known spells mutation (formerly accessed via friend)
    void addKnownSpell(uint32_t spellId) { knownSpells_.insert(spellId); }
    [[nodiscard]] bool hasKnownSpell(uint32_t spellId) const { return knownSpells_.count(spellId) > 0; }

    // Target aura mutation (formerly accessed via friend)
    void clearTargetAuras() { for (auto& slot : targetAuras_) slot = AuraSlot{}; }

    // Player aura mutation (formerly accessed via friend)
    void resetPlayerAuras(size_t capacity) { playerAuras_.clear(); playerAuras_.resize(capacity); }
    AuraSlot& getPlayerAuraSlotRef(size_t slot) { return playerAuras_[slot]; }
    std::vector<AuraSlot>& getPlayerAurasMut() { return playerAuras_; }

    // Target cast helpers
    [[nodiscard]] bool isTargetCasting() const;
    [[nodiscard]] uint32_t getTargetCastSpellId() const;
    [[nodiscard]] float getTargetCastProgress() const;
    [[nodiscard]] float getTargetCastTimeRemaining() const;
    [[nodiscard]] bool isTargetCastInterruptible() const;

    // Talents
    [[nodiscard]] uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    [[nodiscard]] uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    [[nodiscard]] uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        static std::unordered_map<uint32_t, uint8_t> empty;
        return spec < 2 ? learnedTalents_[spec] : empty;
    }

    static constexpr uint8_t MAX_GLYPH_SLOTS = 6;
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs() const { return learnedGlyphs_[activeTalentSpec_]; }
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs(uint8_t spec) const {
        static std::array<uint16_t, MAX_GLYPH_SLOTS> empty{};
        return spec < 2 ? learnedGlyphs_[spec] : empty;
    }
    [[nodiscard]] uint8_t getTalentRank(uint32_t talentId) const {
        auto it = learnedTalents_[activeTalentSpec_].find(talentId);
        return (it != learnedTalents_[activeTalentSpec_].end()) ? it->second : 0;
    }
    void learnTalent(uint32_t talentId, uint32_t requestedRank);
    void switchTalentSpec(uint8_t newSpec);

    // Talent DBC access
    [[nodiscard]] const TalentEntry* getTalentEntry(uint32_t talentId) const {
        auto it = talentCache_.find(talentId);
        return (it != talentCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const TalentTabEntry* getTalentTabEntry(uint32_t tabId) const {
        auto it = talentTabCache_.find(tabId);
        return (it != talentTabCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentEntry>& getAllTalents() const { return talentCache_; }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentTabEntry>& getAllTalentTabs() const { return talentTabCache_; }

    /// Drops what was read out of Talent.dbc and TalentTab.dbc.
    ///
    /// Called when the active expansion changes, for the same reason as
    /// MovementHandler::resetTaxiDbcCache: GameHandler cleared copies of its
    /// own and the readers come here.
    void resetTalentDbcCache() {
        talentCache_.clear();
        talentTabCache_.clear();
        talentDbcLoaded_ = false;
    }
    void loadTalentDbc();
    void syncPreWotlkTalentsFromKnownSpells();

    // Auras
    [[nodiscard]] const std::vector<AuraSlot>& getPlayerAuras() const { return playerAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>& getTargetAuras() const { return targetAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>* getUnitAuras(uint64_t guid) const {
        auto it = unitAurasCache_.find(guid);
        return (it != unitAurasCache_.end()) ? &it->second : nullptr;
    }

    // Global Cooldown (GCD)
    [[nodiscard]] float getGCDRemaining() const {
        if (gcdTotal_ <= 0.0f) return 0.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gcdStartedAt_).count() / 1000.0f;
        float rem = gcdTotal_ - elapsed;
        return rem > 0.0f ? rem : 0.0f;
    }
    [[nodiscard]] float getGCDTotal() const { return gcdTotal_; }
    [[nodiscard]] bool isGCDActive() const { return getGCDRemaining() > 0.0f; }

    // Spell book tabs
    const std::vector<SpellBookTab>& getSpellBookTabs();

    // Talent wipe confirm dialog
    [[nodiscard]] bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    [[nodiscard]] uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    /// The trainer offering the wipe. The confirmation closes itself when the
    /// player walks away from them.
    [[nodiscard]] uint64_t getTalentWipeNpcGuid() const { return talentWipeNpcGuid_; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }

    // Pet talent respec confirm
    [[nodiscard]] bool showPetUnlearnDialog() const { return petUnlearnPending_; }
    [[nodiscard]] uint32_t getPetUnlearnCost() const { return petUnlearnCost_; }
    void confirmPetUnlearn();
    void cancelPetUnlearn() { petUnlearnPending_ = false; }

    // Item use

    // Equipment sets - canonical data owned by InventoryHandler;
    // GameHandler::getEquipmentSets() delegates to inventoryHandler_.

    // Pet spells
    void sendPetAction(uint32_t action, uint64_t targetGuid = 0);
    void dismissPet();
    void togglePetSpellAutocast(uint32_t spellId);
    void renamePet(const std::string& newName);

    // Spell DBC accessors
    [[nodiscard]] const int32_t* getSpellEffectBasePoints(uint32_t spellId) const;
    [[nodiscard]] float getSpellDuration(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellName(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellRank(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellDescription(uint32_t spellId) const;
    [[nodiscard]] std::string getEnchantName(uint32_t enchantId) const;
    /// The gem item an enchantment came out of (SpellItemEnchantment.Src_ItemID).
    /// Zero when the enchantment is not a gem, or on a file with no such column.
    /// This is the only route from an enchantment sitting in an item's socket
    /// back to the gem that is in the socket - the item fields carry the
    /// enchantment id and nothing else.
    [[nodiscard]] uint32_t getEnchantGemItem(uint32_t enchantId) const;
    [[nodiscard]] uint8_t getSpellDispelType(uint32_t spellId) const;
    [[nodiscard]] bool isSpellInterruptible(uint32_t spellId) const;
    [[nodiscard]] bool isSpellPassive(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellSchoolMask(uint32_t spellId) const;
    /// Spell.dbc Targets mask (SpellCastTargetFlags): 0x10 = TARGET_FLAG_ITEM.
    [[nodiscard]] uint32_t getSpellTargetFlags(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellTargetAuraState(uint32_t spellId) const;
    /// Spell.dbc RangeIndex resolved via SpellRange.dbc, in yards. Melee ("Combat
    /// Range") is 5; self-only is 0; negative means SpellRange.dbc was unavailable.
    [[nodiscard]] float getSpellMaxRange(uint32_t spellId) const;
    /// Spell.dbc Speed, in yards per second: how fast the spell's missile art
    /// travels. Zero for everything that lands the instant it is cast.
    [[nodiscard]] float getSpellMissileSpeed(uint32_t spellId) const;
    /// True for "Self Only" range spells (shouts, self-buffs): they always land on
    /// the caster, so they take no explicit target and skip melee range checks.
    [[nodiscard]] bool isSelfCastSpell(uint32_t spellId) const;
    /// Maps a superseded spell rank to the highest rank we actually know. Returns
    /// spellId unchanged when it is already known, or has no known same-name rank.
    [[nodiscard]] uint32_t resolveHighestKnownRank(uint32_t spellId) const;
    /// The skill's own name, by SkillLine.dbc id.
    [[nodiscard]] const std::string& getSkillLineName(uint32_t skillLineId) const;

    // Cast state
    void stopCasting();
    void resetCastState();
    void resetTalentState();
    // Full per-character reset (spells, cooldowns, auras, cast state, talents).
    // Called from GameHandler::selectCharacter so spell state doesn't bleed between characters.
    void resetAllState();
    void clearUnitCaches();

    // Aura duration
    void handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs);

    // Skill DBC
    void loadSkillLineDbc();
    void extractSkillFields(const FlatFieldMap& fields);
    void extractExploredZoneFields(const FlatFieldMap& fields);

    // Update per-frame timers (call from GameHandler::update)
    void updateTimers(float dt);
    void refreshRestorationState() { refreshRestorationFromPlayerAuras(); }

    // Packet handlers dispatched from GameHandler's opcode table
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);

    // Pet stable commands (called via GameHandler delegation)
    void requestStabledPetList();
    void stablePet(uint8_t slot);
    void unstablePet(uint32_t petNumber);

    // DBC cache loading (called from GameHandler during login)
    void loadSpellNameCache() const;
    void loadSkillLineAbilityDbc();

    /// Ask the server what the pet is called.
    ///
    /// The name a player gave a pet arrives only in answer to
    /// CMSG_PET_NAME_QUERY. Without it a pet wears its creature template's
    /// name - "Voidwalker" where the player wrote something else - which is
    /// what the pet frame, its nameplate and the pet bar's tooltip all show.
    ///
    /// The pet number in that request is a key the server echoes back and does
    /// not look anything up with: SendPetNameQuery finds the pet by guid. So
    /// this sends a number of its own making and uses it to match the reply to
    /// the pet it asked about, which is what the field would have been for.
    void requestPetName(uint64_t petGuid);

    /// Give a hunter's pet up for good. The interface asks first - this is the
    /// other side of the ABANDON_PET dialog, whose accept called an unbound
    /// name and raised.
    /// The five values every UNIT_SPELLCAST_* event carries.
    ///
    /// unit, spell name, rank, cast id, spell id - in that order, which is what
    /// FrameXML unpacks. These were being fired as just the unit and the spell
    /// id, so the id sat where the name belongs and the cast id was absent.
    [[nodiscard]] std::vector<std::string> spellcastArgs(const std::string& unitId,
                                           uint32_t spellId) const;

    void abandonPet();

    /// Buy the next stable slot from the stable master currently open.
    void buyStableSlot();

private:
    std::unordered_map<uint32_t, uint64_t> pendingPetNameQueries_;
    uint32_t nextPetNameQueryKey_ = 1;

    // --- Packet handlers ---
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    void handleSpellCooldown(network::Packet& packet);
    void handleCooldownEvent(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleLearnedSpell(network::Packet& packet);

    void handleCastResult(network::Packet& packet);
    void handleSpellFailedOther(network::Packet& packet);
    void handleClearCooldown(network::Packet& packet);
    void handleModifyCooldown(network::Packet& packet);
    void handlePlaySpellVisual(network::Packet& packet);
    void handleSpellModifier(network::Packet& packet, bool isFlat);
    void handleSpellDelayed(network::Packet& packet);
    void handleSpellLogMiss(network::Packet& packet);
    void handleSpellFailure(network::Packet& packet);
    void handleItemCooldown(network::Packet& packet);
    void handleDispelFailed(network::Packet& packet);
    void handleTotemCreated(network::Packet& packet);
    void handlePeriodicAuraLog(network::Packet& packet);
    void handleSpellEnergizeLog(network::Packet& packet);
    void handleExtraAuraInfo(network::Packet& packet, bool isInit);
    void handleSpellDispelLog(network::Packet& packet);
    void handleSpellStealLog(network::Packet& packet);
    void handleSpellChanceProcLog(network::Packet& packet);
    void handleSpellInstaKillLog(network::Packet& packet);
    void handleSpellLogExecute(network::Packet& packet);
    void handleClearExtraAuraInfo(network::Packet& packet);
    void handleItemEnchantTimeUpdate(network::Packet& packet);
    void handleResumeCastBar(network::Packet& packet);
    void handleChannelStart(network::Packet& packet);
    void handleChannelUpdate(network::Packet& packet);

    // --- Internal helpers ---

    // Resolve the magic school for a spell (for audio playback).
    // Returns MagicSchool from the spell name cache, defaulting to ARCANE.
    audio::SpellSoundManager::MagicSchool resolveSpellSchool(uint32_t spellId);

    // Play a spell cast or impact sound via audioCoordinator, if available.
    void playSpellCastSound(uint32_t spellId);
    void playSpellImpactSound(uint32_t spellId);

    // Resolve SpellVisualID from Spell.dbc cache for a given spellId.
    uint32_t resolveSpellVisualId(uint32_t spellId);
    // Resolve render-space position for a unit GUID (player or entity).
    bool resolveUnitPosition(uint64_t guid, glm::vec3& outPos);
    // Play the cast/precast visual effect at the caster's position.
    void triggerCastVisual(uint32_t spellId, uint64_t casterGuid, uint32_t castTimeMs = 0);
    // Play the impact visual effect at the target's position.
    void triggerImpactVisual(uint32_t spellId, uint64_t targetGuid);
    void launchRangedWeaponProjectile(uint32_t spellId, uint64_t targetGuid);

    /// Throw the spell's missile art from caster to target.
    ///
    /// True when a missile is in flight, in which case the impact visual is
    /// raised where it lands and the caller must not raise it itself. False
    /// for every spell that arrives the instant it is cast - Spell.dbc gives
    /// those a missile speed of zero.
    bool launchSpellMissile(uint32_t spellId, uint64_t casterGuid, uint64_t targetGuid);
    void refreshRestorationFromPlayerAuras();
    void stopRestorationPresentation();

    // --- handleSpellLogExecute per-effect parsers (extracted to reduce nesting) ---
    void parseEffectPowerDrain(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                               bool usesFullGuid);
    void parseEffectHealthLeech(network::Packet& packet, uint32_t effectLogCount,
                                uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                                bool usesFullGuid);
    void parseEffectCreateItem(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectInterruptCast(network::Packet& packet, uint32_t effectLogCount,
                                  uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                                  bool usesFullGuid);
    void parseEffectFeedPet(network::Packet& packet, uint32_t effectLogCount,
                            uint64_t caster, uint32_t spellId, bool isPlayerCaster);

    // Find the on-use spell for an item (trigger=0 Use or trigger=5 NoDelay).
    // CMSG_USE_ITEM requires a valid spellId or the server silently ignores it.
    void seedCooldownFromSpellInfo(uint32_t spellId);
    void handleSupercededSpell(network::Packet& packet);
    void handleRemovedSpell(network::Packet& packet);
    void handleUnlearnSpells(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);
    void handleAchievementEarned(network::Packet& packet);

    GameHandler& owner_;

    // --- Spell state ---
    std::unordered_set<uint32_t> knownSpells_;
    std::unordered_map<uint32_t, float> spellCooldowns_;    // spellId -> remaining seconds
    // spellId -> the length the cooldown had when it began. Kept beside the
    // remaining time rather than derived from it because GetSpellCooldown is
    // asked for (start, duration), and answering (now, remaining) redraws the
    // swirl as a fresh full sweep every time the interface asks - which it does
    // on every ACTIONBAR_UPDATE_COOLDOWN, so a long cooldown appears to restart
    // whenever anything else is cast.
    std::unordered_map<uint32_t, float> spellCooldownTotals_;
    uint8_t castCount_ = 0;
    bool casting_ = false;
    bool castIsChannel_ = false;
    uint32_t currentCastSpellId_ = 0;
    float castTimeRemaining_ = 0.0f;
    float castTimeTotal_ = 0.0f;
    bool restorationActive_ = false;
    uint32_t restorationSpellId_ = 0;
    bool restorationIsFood_ = false;
    float restorationTimeRemaining_ = 0.0f;
    float restorationTimeTotal_ = 0.0f;
    float restorationSoundTimer_ = 0.0f; // repeats the consume sound while active

    // Repeat-craft queue
    uint32_t craftQueueSpellId_ = 0;
    int craftQueueRemaining_ = 0;

    // Crafting window
    bool craftingWindowOpen_ = false;
    uint32_t craftingSkillLine_ = 0;

    // SpellFocusObject.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> spellFocusNames_;
    bool spellFocusDbcLoaded_ = false;

    // TotemCategory.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> totemCategoryNames_;
    bool totemCategoryDbcLoaded_ = false;

    // Spell queue (400ms window)
    uint32_t lastGroundCastSpellId_ = 0;
    /// When auto-attack was last toggled, for the Ability Toggle guard: a
    /// second press inside a short window is an accident rather than a
    /// decision. See the SPELL_ID_ATTACK branch in castSpell.
    std::chrono::steady_clock::time_point autoAttackToggledAt_{};
    uint32_t queuedSpellId_ = 0;
    uint64_t queuedSpellTarget_ = 0;

    // Per-unit cast state
    std::unordered_map<uint64_t, UnitCastState> unitCastStates_;

    // Talents (dual-spec support)
    uint8_t activeTalentSpec_ = 0;
    uint8_t unspentTalentPoints_[2] = {0, 0};
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];
    std::array<std::array<uint16_t, MAX_GLYPH_SLOTS>, 2> learnedGlyphs_{};
    std::unordered_map<uint32_t, TalentEntry> talentCache_;
    std::unordered_map<uint32_t, TalentTabEntry> talentTabCache_;
    bool talentDbcLoaded_ = false;
    bool talentsInitialized_ = false;

    // Auras
    std::vector<AuraSlot> playerAuras_;
    std::vector<AuraSlot> targetAuras_;
    std::unordered_map<uint64_t, std::vector<AuraSlot>> unitAurasCache_;

    // Global Cooldown
    float gcdTotal_ = 0.0f;
    std::chrono::steady_clock::time_point gcdStartedAt_{};

    // Spell book tabs
    std::vector<SpellBookTab> spellBookTabs_;
    size_t lastSpellCount_ = 0;
    bool spellBookTabsDirty_ = true;

    // Talent wipe confirm dialog
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;

    // Pet talent respec confirm dialog
    PetState pet_;
    std::vector<TempEnchantTimer> tempEnchantTimers_;
    bool petUnlearnPending_ = false;
    uint32_t petUnlearnCost_ = 0;

    // ---- State that used to live in GameHandler ----
    //
    // Seventeen members reached through a GameHandler::xRef() accessor that
    // handed out a mutable reference to its private state. Nothing but this
    // handler read any of them: GameHandler's only other mention of each was
    // the accessor itself and a line clearing it on character switch, which is
    // now in resetAllState below.
    //
    // The decomposition was nominal while this was true - a handler that
    // reaches back through 114 members of its owner is a namespace, not a
    // component. These seventeen are the ones that moved with no forwarding
    // left behind at all.
    AchievementEarnedCallback achievementEarnedCallback_;
    // Shaman totem state
    TotemSlot activeTotemSlots_[NUM_TOTEM_SLOTS];
    ChargeCallback chargeCallback_;
    bool hasPlayerExploredZones_ = false;
    HearthstonePreloadCallback hearthstonePreloadCallback_;
    // ---- Player skills ----
    std::unordered_map<uint32_t, PlayerSkill> playerSkills_;
    bool skillLineAbilityLoaded_ = false;
    bool skillLineDbcLoaded_ = false;
    /// The sentence the skills window prints under a selected skill. Read from
    /// the same row as the name and the icon.
    std::unordered_map<uint32_t, std::string> skillLineDescriptions_;
    /// SkillLine.dbc's own icon, which is what gives each spellbook tab down
    /// the side of the book its distinct picture. Read alongside the name
    /// because they come out of the same row of the same file.
    std::unordered_map<uint32_t, uint32_t> skillLineIcons_;
    SpellCastAnimCallback spellCastAnimCallback_;
    SpellCastFailedCallback spellCastFailedCallback_;
    // ---- Spell modifiers (SMSG_SET_FLAT_SPELL_MODIFIER / SMSG_SET_PCT_SPELL_MODIFIER) ----
    // Keyed by (SpellModOp, groupIndex); cleared on logout/character change.
    std::unordered_map<SpellModKey, int32_t, SpellModKeyHash> spellFlatMods_;
    mutable bool spellNameCacheLoaded_ = false;
    std::unordered_map<SpellModKey, int32_t, SpellModKeyHash> spellPctMods_;
    SprintAuraCallback sprintAuraCallback_;
    uint8_t  stableNumSlots_   = 0;
};

} // namespace game
} // namespace wowee
