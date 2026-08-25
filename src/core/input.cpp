#include "core/input.hpp"

namespace wowee {
namespace core {

Input& Input::getInstance() {
    static Input instance;
    return instance;
}

void Input::beginFrame() {
    pressedBindingCommands_.clear();
}

void Input::setBindingCommandHeld(const std::string& command, bool held) {
    if (command.empty()) return;
    if (held) {
        unsigned& count = heldBindingCommands_[command];
        if (count++ == 0) pressedBindingCommands_.insert(command);
    } else {
        const auto it = heldBindingCommands_.find(command);
        if (it == heldBindingCommands_.end()) return;
        if (it->second <= 1) heldBindingCommands_.erase(it);
        else --it->second;
    }
}

bool Input::isBindingCommandHeld(const std::string& command) const {
    return heldBindingCommands_.contains(command);
}

bool Input::isBindingCommandJustPressed(const std::string& command) const {
    return pressedBindingCommands_.contains(command);
}

void Input::clearBindingCommands() {
    heldBindingCommands_.clear();
    pressedBindingCommands_.clear();
}

void Input::update() {
    // Copy current state to previous
    previousKeyState = currentKeyState;
    previousMouseState = currentMouseState;
    previousMousePosition = mousePosition;

    // Get current keyboard state
    const Uint8* keyState = SDL_GetKeyboardState(nullptr);
    for (int i = 0; i < NUM_KEYS; ++i) {
        currentKeyState[i] = keyState[i] || virtualKeyState[i];
    }

    // Get current mouse state
    int mouseX, mouseY;
    Uint32 mouseState = SDL_GetMouseState(&mouseX, &mouseY);
    mousePosition = glm::vec2(static_cast<float>(mouseX), static_cast<float>(mouseY));

    // SDL_BUTTON(x) is defined as (1 << (x-1)), so button indices are 1-based.
    // SDL_BUTTON(0) is undefined behavior (negative shift). Start at 1.
    currentMouseState[0] = false;
    for (int i = 1; i < NUM_MOUSE_BUTTONS; ++i) {
        currentMouseState[i] = (mouseState & SDL_BUTTON(i)) != 0;
    }

    // Calculate mouse delta
    mouseDelta = mousePosition - previousMousePosition;
}

void Input::setVirtualKey(SDL_Scancode key, bool held) {
    if (key < 0 || key >= NUM_KEYS) return;
    if (virtualKeyState[key] == held) return;
    virtualKeyState[key] = held;
    // Touch controls express the same default movement actions as a keyboard,
    // without producing SDL key events for the binding dispatcher.
    const char* command = nullptr;
    switch (key) {
        case SDL_SCANCODE_W: command = "MOVEFORWARD"; break;
        case SDL_SCANCODE_S: command = "MOVEBACKWARD"; break;
        case SDL_SCANCODE_A: command = "TURNLEFT"; break;
        case SDL_SCANCODE_D: command = "TURNRIGHT"; break;
        case SDL_SCANCODE_Q: command = "STRAFELEFT"; break;
        case SDL_SCANCODE_E: command = "STRAFERIGHT"; break;
        case SDL_SCANCODE_SPACE: command = "JUMP"; break;
        default: break;
    }
    if (command) setBindingCommandHeld(command, held);
}

void Input::clearVirtualKeys() {
    for (int key = 0; key < NUM_KEYS; ++key) {
        if (virtualKeyState[key]) setVirtualKey(static_cast<SDL_Scancode>(key), false);
    }
}

bool Input::isKeyPressed(SDL_Scancode key) const {
    if (key < 0 || key >= NUM_KEYS) return false;
    return currentKeyState[key];
}

bool Input::isKeyJustPressed(SDL_Scancode key) const {
    if (key < 0 || key >= NUM_KEYS) return false;
    return currentKeyState[key] && !previousKeyState[key];
}
bool Input::isMouseButtonPressed(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return currentMouseState[button];
}

bool Input::isMouseButtonJustPressed(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return currentMouseState[button] && !previousMouseState[button];
}

bool Input::isMouseButtonJustReleased(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return !currentMouseState[button] && previousMouseState[button];
}
} // namespace core
} // namespace wowee
