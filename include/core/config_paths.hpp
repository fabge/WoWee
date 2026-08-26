#pragma once

#include <optional>
#include <string>

namespace wowee::core {

// Absolute path to the directory holding the running executable.
// Empty if it cannot be determined.
std::string getExecutableDir();

// Root directory for user config (login.cfg, settings.cfg, last_character.cfg,
// characters/). Two modes:
//   - Portable: if a "portable.txt" marker file, or an existing "config" folder,
//     sits next to the executable, config lives in <exe_dir>/config. This keeps
//     the whole client self-contained in one folder (USB sticks, clean uninstall,
//     easy backup of server profiles).
//   - Per-user (default): %APPDATA%\wowee on Windows, ~/.wowee elsewhere.
std::string getConfigRoot();

// Root directory for disposable runtime caches. WOWEE_CACHE_ROOT overrides the
// platform default, which is %LOCALAPPDATA%\Wowee\cache on Windows,
// ~/Library/Caches/Wowee on macOS, and $XDG_CACHE_HOME/wowee (or
// ~/.cache/wowee) elsewhere. The directory is created before it is returned.
std::string getCacheRoot();

// True when the running executable sits inside a macOS .app bundle. Always
// false elsewhere. Everything inside the bundle is under the code-signed seal
// and must be treated as read-only, so this is what the writable-path
// decisions are made against.
bool runningFromMacAppBundle();

// Restrict a credential-bearing file to its owner on POSIX. Windows user data
// inherits the profile directory's ACL, so this is a no-op there. Returns false
// when a POSIX permission update fails.
bool restrictFileToOwner(const std::string& path);

// Join an untrusted filename to a trusted directory only when it is exactly
// one relative path component. Returns no path for absolute names, separators,
// or traversal components.
std::optional<std::string> safeChildPath(const std::string& directory,
                                         const std::string& filename);

// One-time seeding of portable config. On the first launch after the user drops
// a "portable.txt" marker next to the executable (before any config folder
// exists), copies the existing per-user config tree into <exe_dir>/config so
// saved server profiles, settings, and characters carry over. No-op afterwards,
// and a no-op when not in portable mode. Call once at startup before config is
// read.
void migratePortableConfigIfNeeded();

// Enters the directory named by WOWEE_RESOURCE_ROOT, which holds assets/ and
// Data/ in the layout a desktop install has.
//
// Only Android sets that variable, and only Android needs this: a process there
// starts in a directory holding neither, and shaders, interface art and the
// expansion profiles are all opened through relative paths. It is not enough to
// do this once at startup, because SDL and the Vulkan driver leave the working
// directory at /system/bin, so this is called again once they are up.
//
// A no-op wherever WOWEE_RESOURCE_ROOT is unset. Returns false only if the
// variable names a directory that cannot be entered.
bool enterResourceRoot();

}  // namespace wowee::core
