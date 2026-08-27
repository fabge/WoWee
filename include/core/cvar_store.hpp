// cvar_store.hpp - the CVars the player or the interface has set.
//
// Lives in wowee_base rather than with the Lua CVar API that wraps it. It is
// a key-value file under the config root and nothing about it needs Lua, but
// src/game and src/rendering read from it during play - the threat warning
// level, whether to dismount when flying, the spam filter - and reaching into
// src/addons for that was the whole of the game -> addons and
// rendering -> addons back-edges, one symbol each against five hundred and two
// the other way.
//
// The in-memory map is the point: storedCVarValue is called from hot paths
// like the threat update, so answering from a file read per call would trade
// one problem for a worse one.
#pragma once

#include <string>
#include <unordered_map>

namespace wowee::core {

/// The store itself. Written by SetCVar, read by everything else.
std::unordered_map<std::string, std::string>& cvarStore();

/// Where the CVars live between runs.
///
/// Its own file rather than settings.cfg, which this client's own panel writes
/// whole from its own fields: a CVar written into that file would be dropped
/// the next time that panel saved.
std::string cvarStorePath();

/// Read the stored CVars back. Called once, before the interface loads.
void loadStoredCVars();

/// Write them back. Called on every change rather than at shutdown: the file
/// is a few hundred bytes, and a setting that survives only a clean exit is
/// not one a player can rely on.
void saveStoredCVars();

/// One CVar's value: the store first, then the file, then the fallback.
///
/// The file half is what makes this answerable before the interface is up,
/// which is when the renderer asks about its own settings.
std::string storedCVarValue(const std::string& key, const std::string& fallback);

/// The CVars whose value is a client setting.
///
/// FrameXML's Video and Interface panels are bound to CVar names. The values
/// behind these names are settings this client already had - they simply had
/// never been introduced, so the panels wrote to a store nothing read and the
/// controls did nothing.
///
/// A row here, a key in Application's bridge, and the control works. The
/// alternative is a getter and a setter on LuaServices, a lambda in
/// Application, and a branch in each of GetCVar and SetCVar, four places per
/// option.
struct ClientCVarBinding {
    const char* cvar;      ///< lower case, as both sides fold it
    const char* setting;   ///< the key Application's bridge answers to
    /// What the setting reads when the CVar reads one, where the two are not
    /// counting the same thing.
    ///
    /// Almost always they are: a volume is 0 to 1 on both sides, a checkbox is
    /// 0 or 1. Ground clutter is not. Blizzard's Ground Density slider counts
    /// doodads and runs 16 to 64; this client's setting is a proportion and
    /// runs 0 to 1.5. Handing one straight to the other put 64 into a field
    /// that stops at 1.5, so every position of that slider - all seven of them
    /// - came out as the most clutter the client will draw, and stayed there:
    /// the value written to the config was clamped back down on the next load,
    /// which made it permanent.
    double scale = 1.0;
};

/// The binding for a CVar name, or null. The name must already be lower case,
/// which is the folding both sides do.
const ClientCVarBinding* findClientCVar(const std::string& lowerName);

/// A client setting has changed: mirror it into the CVar the game's own
/// control drives, for the settings that have one.
///
/// The store is applied over settings.cfg at start-up, so a setting that does
/// not reach it is undone at the next start by a CVar nobody touched.
///
/// It lives here, next to the store it writes, rather than behind an injected
/// service. It was reached through one for a day - `AddonBridge` - and the
/// harness that exists to watch exactly this passes an empty service set, so
/// the check went quiet and the settings panel's most important side effect
/// became conditional on a pointer. Nothing about mirroring a value into a
/// key-value map needs Lua or an interface, and the same reasoning put the
/// store itself in this library.
void noteClientSettingChanged(const std::string& settingKey, const std::string& value);

/// Held while a CVar is being applied to the setting it drives, so the echo
/// back does not rewrite the store.
///
/// The store already holds the value in that direction, and echoing it back
/// rewrites it with whatever survives the trip: ground clutter is kept as a
/// whole percent, so a Ground Density of 24 came back as 23.893333 and the
/// store no longer said what the player picked - nor any position that slider
/// can be put in.
class ApplyingCVarToSetting {
public:
    ApplyingCVarToSetting();
    ~ApplyingCVarToSetting();
    ApplyingCVarToSetting(const ApplyingCVarToSetting&) = delete;
    ApplyingCVarToSetting& operator=(const ApplyingCVarToSetting&) = delete;
};

}  // namespace wowee::core
