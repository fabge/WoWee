// cvar_store.cpp - see cvar_store.hpp.
//
// Moved out of src/addons/lua_system_api.cpp on 2026-08-26. Nothing here needs
// Lua; what kept it there was that the Lua CVar API was written first.
#include "core/cvar_store.hpp"

#include "core/config_paths.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace wowee::core {
namespace {

void toLowerInPlace(std::string& s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

/// CVars the player or the interface has actually set, which win over the
/// defaults below.
///
/// SetCVar was a no-op, so every option the interface changed reverted the
/// instant it was read back: ticking a box in the interface options did
/// nothing, and any code that writes a CVar and then reads it to confirm - of
/// which FrameXML has a fair amount - saw its own write disappear.
std::unordered_map<std::string, std::string>& cvarStore() {
    static std::unordered_map<std::string, std::string> store;
    return store;
}

/// Where the CVars live between runs.
///
/// Its own file rather than settings.cfg, which this client's own panel writes
/// whole from its own fields: a CVar written into that file would be dropped
/// the next time that panel saved.
std::string cvarStorePath() {
    return getConfigRoot() + "/cvars.cfg";
}

/// Read the stored CVars back. Called once, before the interface loads,
/// because a panel reads its checkbox out of the CVar as it is built and
/// anything arriving later leaves the box disagreeing with the setting.
///
/// Storing them in memory alone made every interface option last exactly one
/// session. That is not a small thing: the equipment manager is off until its
/// box is ticked, the box writes equipmentManager, and the paperdoll reads that
/// on VARIABLES_LOADED - so it came back off on every login, and so did every
/// other option the player had set.
void loadStoredCVars() {
    std::ifstream in(cvarStorePath());
    if (!in.is_open()) return;
    std::string line;
    size_t loaded = 0;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        toLowerInPlace(key);
        cvarStore()[key] = line.substr(eq + 1);
        ++loaded;
    }
    LOG_INFO("CVars: loaded ", loaded, " from ", cvarStorePath());
}

/// Write them back. Called on every change rather than at shutdown: the file is
/// a few hundred bytes, and a setting that survives only a clean exit is not
/// one a player can rely on.
void saveStoredCVars() {
    const std::string path = cvarStorePath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save CVars to ", path);
        return;
    }
    // Sorted, so a diff of the file reads as a change of settings rather than
    // as the hash order moving underneath it.
    std::vector<const std::pair<const std::string, std::string>*> rows;
    rows.reserve(cvarStore().size());
    for (const auto& kv : cvarStore()) rows.push_back(&kv);
    std::sort(rows.begin(), rows.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    for (const auto* kv : rows) {
        // A value with a newline in it would come back as two broken lines.
        if (kv->second.find('\n') != std::string::npos) continue;
        out << kv->first << '=' << kv->second << '\n';
    }
}

/// Apply what was loaded from disk, once the services behind it exist.
///
/// Called after the interface is up rather than at load: the store is filled
/// before any renderer, camera or audio manager is wired, and every branch
/// above checks its service and would quietly do nothing that early - which is
/// indistinguishable from the fault this exists to fix.
std::string storedCVarValue(const std::string& key, const std::string& fallback) {
    // The store first, for a call made after the interface is up; the file
    // otherwise, which is the case this exists for. Reading the file twice
    // costs nothing and keeps the two answers the same.
    std::string wanted = key;
    toLowerInPlace(wanted);
    if (auto it = cvarStore().find(wanted); it != cvarStore().end()) return it->second;

    std::ifstream in(cvarStorePath());
    if (!in.is_open()) return fallback;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = line.substr(0, eq);
        toLowerInPlace(k);
        if (k == wanted) return line.substr(eq + 1);
    }
    return fallback;
}

}  // namespace wowee::core
