#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace wowee {
namespace game {

/**
 * Identifies a WoW expansion for protocol/asset selection.
 */
struct ExpansionProfile {
    std::string id;            // "classic", "tbc", "wotlk", "cata"
    std::string name;          // "Wrath of the Lich King"
    std::string shortName;     // "WotLK"
    uint8_t majorVersion = 0;
    uint8_t minorVersion = 0;
    uint8_t patchVersion = 0;
    uint16_t build = 0;           // Realm build (sent in LOGON_CHALLENGE)
    uint16_t worldBuild = 0;      // World build (sent in CMSG_AUTH_SESSION, defaults to build)
    uint8_t protocolVersion = 0;  // SRP auth protocol version byte
    // Client header fields used in LOGON_CHALLENGE.
    // Defaults match a typical Windows x86 client.
    std::string game = "WoW";
    std::string platform = "x86";
    std::string os = "Win";
    std::string locale = "enUS";
    uint32_t timezone = 0;
    std::string dataPath;       // Extracted assets and server-local profile data
    std::string definitionPath; // Shipped opcode/update-field/DBC definitions
    uint32_t maxLevel = 60;
    std::vector<uint32_t> races;
    std::vector<uint32_t> classes;

    /// Which header cipher the world connection uses.
    ///
    /// A property of the expansion, not of the network layer: world_socket.cpp
    /// used to pick this from hard-coded build numbers, which was the one place
    /// the otherwise data-driven expansion model leaked into src/network/ and
    /// meant adding an expansion meant editing the socket. Written in the
    /// profile as "vanilla-xor", "tbc-hmac-xor" or "wotlk-rc4".
    ///
    /// Left empty by a profile that does not say, in which case the build
    /// number decides exactly as it did before - every shipped profile predates
    /// this field.
    enum class HeaderCrypt { FromBuild, VanillaXor, TbcHmacXor, WotlkRc4 };
    HeaderCrypt headerCrypt = HeaderCrypt::FromBuild;

    /// The header cipher this profile actually uses, resolving FromBuild
    /// against the world build the way the socket always has.
    [[nodiscard]] HeaderCrypt resolvedHeaderCrypt() const;

    /// The RSA public key this realm signs its Warden module with, 256 bytes.
    ///
    /// Empty means Blizzard's own, which is what a server running a genuine
    /// module uses. A server that builds its own signs it with a key of its
    /// own, and checking that signature against Blizzard's says only that the
    /// two differ. Written in the profile as 512 hex characters.
    std::vector<uint8_t> wardenRsaModulus;

    [[nodiscard]] std::string versionString() const;  // e.g. "3.3.5a"
};

/**
 * Scans Data/expansions/ for available expansion profiles and manages the active selection.
 */
class ExpansionRegistry {
public:
    /**
     * Scan dataRoot/expansions/ for expansion.json files.
     * @param dataRoot Path to extracted Data/ (e.g. "./Data")
     * @param definitionRoot Optional read-only shipped Data/ tree. When it has
     * an expansion matching an extracted profile, protocol tables are loaded
     * from this current copy rather than the stale copy made during extraction.
     * @return Number of profiles discovered
     */
    size_t initialize(const std::string& dataRoot,
                      const std::string& definitionRoot = {});

    /** All discovered profiles. */
    [[nodiscard]] const std::vector<ExpansionProfile>& getAllProfiles() const { return profiles_; }

    /** Lookup by id (e.g. "wotlk"). Returns nullptr if not found. */
    [[nodiscard]] const ExpansionProfile* getProfile(const std::string& id) const;

    /** Set the active expansion. Returns false if id not found. */
    bool setActive(const std::string& id);

    /** Get the active expansion profile. Never null after successful initialize(). */
    [[nodiscard]] const ExpansionProfile* getActive() const;

    /** Convenience: active expansion id. Empty if none. */
    [[nodiscard]] const std::string& getActiveId() const { return activeId_; }

private:
    std::vector<ExpansionProfile> profiles_;
    std::string activeId_;

    bool loadProfile(const std::string& jsonPath, const std::string& dirPath);
};

/// The registry the client is running against, for the handful of places that
/// need the active expansion and have no path to the one Application owns.
///
/// The same shape as pipeline::setActiveDBCLayout, and for the same reason:
/// which expansion is active is one answer for the whole process, and the
/// alternative was an inline in a header reaching Application::getInstance() -
/// which is what made every translation unit that asked "is this classic?"
/// into a src/game -> src/core dependency. Set once, by Application, right
/// after the registry is built.
void setActiveExpansionRegistry(const ExpansionRegistry* registry);
[[nodiscard]] const ExpansionRegistry* getActiveExpansionRegistry();

} // namespace game
} // namespace wowee
