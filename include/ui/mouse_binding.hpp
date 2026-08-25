#pragma once

#include <SDL2/SDL_mouse.h>

#include <string>

namespace wowee::ui {

/// WoW's mouse button numbering, which is not SDL's: the interface calls the
/// right button BUTTON2 and the middle one BUTTON3, while SDL orders them the
/// other way round. Returns 0 for a button the interface cannot bind, so a
/// gaming mouse's extra buttons resolve to nothing rather than to a binding
/// meant for another button.
[[nodiscard]] inline int wowMouseButton(int sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_LEFT:   return 1;
        case SDL_BUTTON_RIGHT:  return 2;
        case SDL_BUTTON_MIDDLE: return 3;
        case SDL_BUTTON_X1:     return 4;
        case SDL_BUTTON_X2:     return 5;
        default:                return 0;
    }
}

/// The binding table's own spelling for that button, or empty when unbindable.
[[nodiscard]] inline std::string wowMouseButtonName(int sdlButton) {
    const int button = wowMouseButton(sdlButton);
    if (button == 0) return {};
    return "BUTTON" + std::to_string(button);
}

/// The identifier a press is tracked under. Negative, because presses are kept
/// in one table keyed by physical input and SDL keycodes are all positive - a
/// mouse button sharing an identifier with a key would release its binding.
[[nodiscard]] inline int mouseBindingPressKey(int wowButton) {
    return -wowButton;
}

}  // namespace wowee::ui
