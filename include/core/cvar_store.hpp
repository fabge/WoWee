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

}  // namespace wowee::core
