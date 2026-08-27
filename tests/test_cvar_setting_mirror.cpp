// A setting reaching the CVar the game's own control drives.
//
// Six of this client's settings are also driven by a Blizzard control, through
// the kClientCVars rows: view distance, mouse speed, friendly nameplates,
// ground clutter, invert mouse and autoloot. The CVar store is applied over
// settings.cfg at start-up, so a change to one of these that stops at the
// settings file is undone at the next start by a CVar nobody touched.
//
// framexml_settings_control_check drives the real panel and would catch this -
// it did catch it, for a day, when the call went behind an optional service
// pointer the harness does not set. But that sweep needs the extracted
// interface, which CI has none of, so it skips there and the fault would have
// reached five platforms unseen. This is the same promise asked of the one
// function that keeps it, and it needs nothing but a config root.
#include <catch_amalgamated.hpp>

#include "core/cvar_store.hpp"
#include "core/env.hpp"

#include <filesystem>
#include <string>

using wowee::core::ApplyingCVarToSetting;
using wowee::core::cvarStore;
using wowee::core::findClientCVar;
using wowee::core::noteClientSettingChanged;

namespace {

/// Its own corner of the filesystem. noteClientSettingChanged saves on every
/// change, and a test must not write the player's own cvars.cfg.
void useScratchConfigRoot() {
    static bool done = false;
    if (done) return;
    done = true;
    const auto dir = std::filesystem::temp_directory_path() / "wowee-cvar-mirror-test";
    std::filesystem::create_directories(dir);
    wowee::core::setEnvVar("WOWEE_CONFIG_ROOT", dir.string().c_str());
}

}  // namespace

TEST_CASE("a setting a Blizzard control drives reaches its CVar", "[cvar][settings]") {
    useScratchConfigRoot();
    cvarStore().clear();

    noteClientSettingChanged("viewdistance", "1200");
    REQUIRE(cvarStore()["farclip"] == "1200");

    // Not every setting has a CVar, and one that has none must not invent a row.
    const auto before = cvarStore().size();
    noteClientSettingChanged("show_dps_meter", "1");
    REQUIRE(cvarStore().size() == before);
}

TEST_CASE("a scaled setting is converted into the CVar's own units", "[cvar][settings]") {
    useScratchConfigRoot();
    cvarStore().clear();

    // Blizzard's Ground Density counts doodads and runs to 64; this client's
    // setting is a proportion running to 1.5. One of theirs is 1.5/64 of ours,
    // so the whole clutter setting is the whole slider.
    noteClientSettingChanged("groundclutter", "1.5");
    REQUIRE(cvarStore()["groundeffectdensity"] == "64");

    // And half as much clutter is half the doodads.
    noteClientSettingChanged("groundclutter", "0.75");
    REQUIRE(cvarStore()["groundeffectdensity"] == "32");
}

TEST_CASE("a CVar applied to its setting is not echoed back", "[cvar][settings]") {
    useScratchConfigRoot();
    cvarStore().clear();
    cvarStore()["groundeffectdensity"] = "24";

    // The store is the source in this direction. Letting the setting answer
    // back rewrites the store with whatever survived the trip - ground clutter
    // is kept as a whole percent, so 24 came home as 23.893333, which is not a
    // number that slider can be put on.
    {
        const ApplyingCVarToSetting applying;
        noteClientSettingChanged("groundclutter", "0.5597");
    }
    REQUIRE(cvarStore()["groundeffectdensity"] == "24");

    // And the guard lifts with the scope.
    noteClientSettingChanged("groundclutter", "0.75");
    REQUIRE(cvarStore()["groundeffectdensity"] == "32");
}

TEST_CASE("the six bindings a Blizzard control needs are all there", "[cvar][settings]") {
    // Named rather than counted: the point of the row is that a specific
    // control works, so a row silently renamed is the failure to catch.
    for (const char* cvar : {"farclip", "mousespeed", "nameplateshowfriends",
                             "groundeffectdensity", "mouseinvertpitch", "autolootdefault"}) {
        INFO(cvar);
        REQUIRE(findClientCVar(cvar) != nullptr);
    }
    // Folded to lower case by both sides, so the mixed-case spelling FrameXML
    // actually writes is not what is looked up.
    REQUIRE(findClientCVar("farClip") == nullptr);
}
