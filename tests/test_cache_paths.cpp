#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>

#include "core/config_paths.hpp"
#include "core/env.hpp"

namespace {

class CacheRootOverride {
public:
    explicit CacheRootOverride(const std::filesystem::path& path) {
        wowee::core::setEnvVar("WOWEE_CACHE_ROOT", path.string().c_str());
    }

    ~CacheRootOverride() {
        wowee::core::unsetEnvVar("WOWEE_CACHE_ROOT");
    }
};

}  // namespace

TEST_CASE("an explicit cache root is returned and created", "[config-paths]") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "wowee_cache_root_test";
    std::filesystem::remove_all(root);
    const CacheRootOverride override(root);

    CHECK(std::filesystem::path(wowee::core::getCacheRoot()) == root);
    CHECK(std::filesystem::is_directory(root));

    std::filesystem::remove_all(root);
}

TEST_CASE("an untrusted filename cannot leave its directory", "[config-paths]") {
    const std::filesystem::path root = "/trusted/config";

    CHECK(wowee::core::safeChildPath(root.string(), "Alice.cfg") ==
          (root / "Alice.cfg").string());
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), ""));
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), "."));
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), ".."));
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), "../settings.cfg"));
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), "nested/Alice.cfg"));
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), "/tmp/Alice.cfg"));
#ifdef _WIN32
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), "C:Alice.cfg"));
    CHECK_FALSE(wowee::core::safeChildPath(root.string(), "nested\\Alice.cfg"));
#endif
}

// A character name is not a whole filename: it is pasted into the middle of one
// ("Blizzard_TimeManager.<name>.lua.saved"), so the traversal it can express is
// not "../x" on its own but a name that breaks the composed leaf into a path.
// The composed form is what has to be checked, which is what AddonManager does.
TEST_CASE("a server character name cannot escape its addon directory", "[config-paths]") {
    const std::string dir = "/trusted/addons/Blizzard_TimeManager";
    const auto leafFor = [](const std::string& character) {
        return "Blizzard_TimeManager." + character + ".lua.saved";
    };

    CHECK(wowee::core::safeChildPath(dir, leafFor("Alice")) ==
          (std::filesystem::path(dir) / "Blizzard_TimeManager.Alice.lua.saved").string());

    // Each of these is a name a server is free to send in SMSG_CHAR_ENUM.
    CHECK_FALSE(wowee::core::safeChildPath(dir, leafFor("../../../../etc/cron.d/wowee")));
    CHECK_FALSE(wowee::core::safeChildPath(dir, leafFor("a/b")));
    CHECK_FALSE(wowee::core::safeChildPath(dir, leafFor("/absolute")));
}

#ifndef _WIN32
TEST_CASE("a credential file is restricted to its owner", "[config-paths]") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "wowee_private_file_test";
    {
        std::ofstream out(path);
        out << "credential";
    }

    REQUIRE(wowee::core::restrictFileToOwner(path.string()));
    const auto permissions = std::filesystem::status(path).permissions();
    CHECK((permissions & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
    CHECK((permissions & std::filesystem::perms::owner_write) != std::filesystem::perms::none);
    CHECK((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    CHECK((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);

    std::filesystem::remove(path);
}
#endif
