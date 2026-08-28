#pragma once

#include <string>

#ifdef __APPLE__

namespace wowee {
namespace core {

/**
 * Suppress the macOS press-and-hold accent popup for this process.
 *
 * SDL2 leaves text input enabled for the whole session (SDL_VideoInit calls
 * SDL_StartTextInput when there's no screen keyboard), so AppKit routes key
 * events through NSTextInputContext even during normal gameplay.  Holding a
 * key that takes diacritics - A, S, E, and friends - then opens the accent
 * chooser instead of repeating the key.
 *
 * Registering ApplePressAndHoldEnabled=NO restores plain key repeat.  The
 * value lands in NSUserDefaults' registration domain, so it applies only to
 * this process and never writes to the user's saved preferences.
 *
 * Must be called before SDL_Init brings up NSApplication.
 */
void disablePressAndHoldAccents();

/// The printable character produced by a physical SDL scancode under the
/// active macOS keyboard layout. Empty for keys without a printable layout
/// mapping. This is what makes a German QWERTZ key identify itself as Z rather
/// than as the ANSI position's Y.
std::string localizedKeyName(int sdlScancode);

/// The same character as it should be shown on a binding: uppercased the way
/// the layout's own language uppercases it, so the German key beside L reads Ä
/// rather than ä. Empty for keys with no printable layout mapping.
std::string localizedKeyLabel(int sdlScancode);

/// Which keyboard layout the two calls above are currently answering for.
///
/// Opaque, and only ever compared with itself: a caller that resolves the whole
/// keyboard once needs to know when the player has switched input source, and
/// nothing else about it. Empty when the layout cannot be identified, which
/// asks the caller to resolve again rather than to trust what it holds.
std::string keyboardLayoutIdentifier();

} // namespace core
} // namespace wowee

#endif // __APPLE__
