#include <catch_amalgamated.hpp>

#include "game/expansion_profile.hpp"

#include <cstdlib>
#include <filesystem>
#include "core/env.hpp"
#include <fstream>

namespace {

void setBuildOverride(const char* value) {
    if (value) wowee::core::setEnvVar("WOWEE_TEST_AUTH_BUILD", value);
    else wowee::core::unsetEnvVar("WOWEE_TEST_AUTH_BUILD");
}

std::filesystem::path writeProfileTree() {
    const auto root = std::filesystem::temp_directory_path() /
                      "wowee_expansion_profile_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "expansions" / "turtle");
    std::ofstream out(root / "expansions" / "turtle" / "expansion.json");
    out << R"({
        "id": "turtle",
        "name": "Turtle WoW",
        "shortName": "Turtle",
        "version": { "major": 1, "minor": 18, "patch": 1 },
        "build": 7272,
        "buildEnv": "WOWEE_TEST_AUTH_BUILD",
        "worldBuild": 5875,
        "protocolVersion": 3
    })";
    return root;
}

/// A profile tree whose expansion.json carries `extra` alongside the basics.
std::filesystem::path writeProfileWith(const std::string& extra) {
    const auto root = std::filesystem::temp_directory_path() /
                      "wowee_expansion_warden_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "expansions" / "turtle");
    std::ofstream out(root / "expansions" / "turtle" / "expansion.json");
    out << R"({
        "id": "turtle",
        "name": "Turtle WoW",
        "shortName": "Turtle",
        "version": { "major": 1, "minor": 18, "patch": 1 },
        "build": 7272,
        "protocolVersion": 8)" << extra << "\n    }";
    return root;
}

} // namespace

TEST_CASE("Expansion profile keeps custom auth and world builds separate",
          "[expansion_profile]") {
    setBuildOverride(nullptr);
    const auto root = writeProfileTree();

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    CHECK(profile->versionString() == "1.18.1");
    CHECK(profile->build == 7272);
    CHECK(profile->worldBuild == 5875);

    std::filesystem::remove_all(root);
}

TEST_CASE("Expansion profile accepts a validated auth build override",
          "[expansion_profile]") {
    const auto root = writeProfileTree();
    setBuildOverride("7234");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    CHECK(profile->build == 7234);
    CHECK(profile->worldBuild == 5875);

    setBuildOverride(nullptr);
    std::filesystem::remove_all(root);
}

TEST_CASE("Expansion profile rejects an invalid auth build override",
          "[expansion_profile]") {
    const auto root = writeProfileTree();
    setBuildOverride("999999");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    CHECK(profile->build == 7272);

    setBuildOverride(nullptr);
    std::filesystem::remove_all(root);
}

TEST_CASE("Expansion registry prefers a profile with extracted assets",
          "[expansion_profile]") {
    const auto root = std::filesystem::temp_directory_path() /
                      "wowee_expansion_asset_selection_test";
    std::filesystem::remove_all(root);
    for (const auto& [id, build] : {
             std::pair<const char*, int>{"classic", 5875},
             {"turtle", 7272},
             {"wotlk", 12340}}) {
        const auto dir = root / "expansions" / id;
        std::filesystem::create_directories(dir);
        std::ofstream profile(dir / "expansion.json");
        profile << "{\"id\":\"" << id << "\",\"name\":\"" << id
                << "\",\"shortName\":\"" << id << "\",\"build\":"
                << build << ",\"protocolVersion\":3}";
    }
    std::ofstream(root / "expansions" / "turtle" / "manifest.json") << "{}";

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 3);
    REQUIRE(registry.getActive() != nullptr);
    CHECK(registry.getActive()->id == "turtle");

    std::filesystem::remove_all(root);
}

TEST_CASE("Shipped definitions override stale extracted tables without replacing realm settings",
          "[expansion_profile]") {
    const auto extracted = writeProfileTree();
    const auto shipped = std::filesystem::temp_directory_path() /
                         "wowee_expansion_definition_test";
    std::filesystem::remove_all(shipped);
    const auto definitionDir = shipped / "expansions" / "turtle";
    std::filesystem::create_directories(definitionDir);
    std::ofstream(definitionDir / "opcodes.json") << "{}";
    std::ofstream(definitionDir / "update_fields.json") << "{}";
    std::ofstream(definitionDir / "dbc_layouts.json") << "{}";

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(extracted.string(), shipped.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    CHECK(profile->dataPath == (extracted / "expansions" / "turtle").string());
    CHECK(profile->definitionPath == definitionDir.string());
    // Profile values remain realm-local; only client-owned tables move.
    CHECK(profile->worldBuild == 5875);

    std::filesystem::remove_all(extracted);
    std::filesystem::remove_all(shipped);
}

TEST_CASE("A realm's own Warden key is read from its profile", "[expansion_profile]") {
    // 512 hex characters, which is the only length a 2048-bit modulus has.
    const std::string key(512, 'a');
    const auto root = writeProfileWith(",\n        \"wardenRsaModulus\": \"" + key + "\"");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    REQUIRE(profile->wardenRsaModulus.size() == 256);
    CHECK(profile->wardenRsaModulus.front() == 0xAA);
    CHECK(profile->wardenRsaModulus.back() == 0xAA);

    std::filesystem::remove_all(root);
}

TEST_CASE("A Warden key of the wrong length is refused rather than half-read",
          "[expansion_profile]") {
    // One nibble short. Taking 255 bytes and a half would fail verification
    // exactly the way no key at all does, which is the one outcome that would
    // send somebody looking anywhere but at the profile.
    const auto root = writeProfileWith(",\n        \"wardenRsaModulus\": \"" +
                                       std::string(511, 'a') + "\"");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    CHECK(profile->wardenRsaModulus.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("A Warden key with a character that is not hex is refused",
          "[expansion_profile]") {
    const auto root = writeProfileWith(",\n        \"wardenRsaModulus\": \"" +
                                       std::string(511, 'a') + "z\"");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    CHECK(profile->wardenRsaModulus.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("A profile naming no Warden key leaves the retail one in place",
          "[expansion_profile]") {
    const auto root = writeProfileWith("");

    wowee::game::ExpansionRegistry registry;
    REQUIRE(registry.initialize(root.string()) == 1);
    const auto* profile = registry.getProfile("turtle");
    REQUIRE(profile != nullptr);
    CHECK(profile->wardenRsaModulus.empty());

    std::filesystem::remove_all(root);
}

// The world header cipher is a property of the expansion, not of the network
// layer. It used to be worked out inside WorldSocket::initEncryption from
// hard-coded build numbers, which made src/network/ the one place that knew
// which expansions exist.
TEST_CASE("the header cipher comes from the profile", "[expansion][crypt]") {
    using Crypt = wowee::game::ExpansionProfile::HeaderCrypt;

    SECTION("an explicit choice is used as written") {
        wowee::game::ExpansionProfile p;
        p.build = 12340;
        p.worldBuild = 12340;
        p.headerCrypt = Crypt::VanillaXor;
        // Against the build number, which would say RC4. The profile wins:
        // that is the entire point of the field.
        REQUIRE(p.resolvedHeaderCrypt() == Crypt::VanillaXor);
    }

    SECTION("a profile that says nothing falls back to the build boundaries") {
        const auto eraFor = [](uint16_t worldBuild) {
            wowee::game::ExpansionProfile p;
            p.build = worldBuild;
            p.worldBuild = worldBuild;
            return p.resolvedHeaderCrypt();
        };

        // The boundaries the socket used before this was a profile field:
        // 5875 is 1.12.1 and 8606 is 2.4.3, and both are inclusive.
        REQUIRE(eraFor(5875) == Crypt::VanillaXor);
        REQUIRE(eraFor(5876) == Crypt::TbcHmacXor);
        REQUIRE(eraFor(8606) == Crypt::TbcHmacXor);
        REQUIRE(eraFor(8607) == Crypt::WotlkRc4);
        REQUIRE(eraFor(12340) == Crypt::WotlkRc4);
    }

    SECTION("the world build decides, not the realm build") {
        // Turtle connects with realm build 7272 and world build 5875, and it
        // is the world connection being encrypted. Reading the realm build
        // here would key TBC's cipher on a vanilla server and every packet
        // after AUTH_SESSION would decrypt to noise.
        wowee::game::ExpansionProfile p;
        p.build = 7272;
        p.worldBuild = 5875;
        REQUIRE(p.resolvedHeaderCrypt() == Crypt::VanillaXor);
    }
}
