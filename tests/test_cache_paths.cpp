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
