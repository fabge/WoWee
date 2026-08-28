#include "core/macos_platform.hpp"

#include "core/logger.hpp"

#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>

#include <SDL2/SDL_scancode.h>

namespace wowee {
namespace core {

void disablePressAndHoldAccents() {
    @autoreleasepool {
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        [defaults registerDefaults:@{@"ApplePressAndHoldEnabled": @NO}];

        // The registration domain is searched last, so an explicit global
        // "defaults write -g ApplePressAndHoldEnabled -bool true" would still
        // win.  Fall back to this process' own application domain, which
        // outranks NSGlobalDomain.
        if ([defaults boolForKey:@"ApplePressAndHoldEnabled"]) {
            [defaults setBool:NO forKey:@"ApplePressAndHoldEnabled"];
            LOG_INFO("Press-and-hold accents were enabled globally; "
                     "overriding for this application");
        }
    }
}

namespace {

UInt16 macVirtualKeyForSdlScancode(int scancode) {
    switch (static_cast<SDL_Scancode>(scancode)) {
        case SDL_SCANCODE_A: return kVK_ANSI_A;
        case SDL_SCANCODE_B: return kVK_ANSI_B;
        case SDL_SCANCODE_C: return kVK_ANSI_C;
        case SDL_SCANCODE_D: return kVK_ANSI_D;
        case SDL_SCANCODE_E: return kVK_ANSI_E;
        case SDL_SCANCODE_F: return kVK_ANSI_F;
        case SDL_SCANCODE_G: return kVK_ANSI_G;
        case SDL_SCANCODE_H: return kVK_ANSI_H;
        case SDL_SCANCODE_I: return kVK_ANSI_I;
        case SDL_SCANCODE_J: return kVK_ANSI_J;
        case SDL_SCANCODE_K: return kVK_ANSI_K;
        case SDL_SCANCODE_L: return kVK_ANSI_L;
        case SDL_SCANCODE_M: return kVK_ANSI_M;
        case SDL_SCANCODE_N: return kVK_ANSI_N;
        case SDL_SCANCODE_O: return kVK_ANSI_O;
        case SDL_SCANCODE_P: return kVK_ANSI_P;
        case SDL_SCANCODE_Q: return kVK_ANSI_Q;
        case SDL_SCANCODE_R: return kVK_ANSI_R;
        case SDL_SCANCODE_S: return kVK_ANSI_S;
        case SDL_SCANCODE_T: return kVK_ANSI_T;
        case SDL_SCANCODE_U: return kVK_ANSI_U;
        case SDL_SCANCODE_V: return kVK_ANSI_V;
        case SDL_SCANCODE_W: return kVK_ANSI_W;
        case SDL_SCANCODE_X: return kVK_ANSI_X;
        case SDL_SCANCODE_Y: return kVK_ANSI_Y;
        case SDL_SCANCODE_Z: return kVK_ANSI_Z;
        case SDL_SCANCODE_0: return kVK_ANSI_0;
        case SDL_SCANCODE_1: return kVK_ANSI_1;
        case SDL_SCANCODE_2: return kVK_ANSI_2;
        case SDL_SCANCODE_3: return kVK_ANSI_3;
        case SDL_SCANCODE_4: return kVK_ANSI_4;
        case SDL_SCANCODE_5: return kVK_ANSI_5;
        case SDL_SCANCODE_6: return kVK_ANSI_6;
        case SDL_SCANCODE_7: return kVK_ANSI_7;
        case SDL_SCANCODE_8: return kVK_ANSI_8;
        case SDL_SCANCODE_9: return kVK_ANSI_9;
        case SDL_SCANCODE_MINUS: return kVK_ANSI_Minus;
        case SDL_SCANCODE_EQUALS: return kVK_ANSI_Equal;
        case SDL_SCANCODE_LEFTBRACKET: return kVK_ANSI_LeftBracket;
        case SDL_SCANCODE_RIGHTBRACKET: return kVK_ANSI_RightBracket;
        case SDL_SCANCODE_BACKSLASH: return kVK_ANSI_Backslash;
        case SDL_SCANCODE_SEMICOLON: return kVK_ANSI_Semicolon;
        case SDL_SCANCODE_APOSTROPHE: return kVK_ANSI_Quote;
        case SDL_SCANCODE_GRAVE: return kVK_ANSI_Grave;
        case SDL_SCANCODE_COMMA: return kVK_ANSI_Comma;
        case SDL_SCANCODE_PERIOD: return kVK_ANSI_Period;
        case SDL_SCANCODE_SLASH: return kVK_ANSI_Slash;
        default: return UINT16_MAX;
    }
}

}  // namespace

std::string localizedKeyName(int sdlScancode) {
    const UInt16 virtualKey = macVirtualKeyForSdlScancode(sdlScancode);
    if (virtualKey == UINT16_MAX) return {};

    @autoreleasepool {
        TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
        if (!source) return {};
        CFDataRef data = static_cast<CFDataRef>(
            TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
        if (!data) {
            CFRelease(source);
            return {};
        }
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        UInt32 deadKeyState = 0;
        UniChar chars[4] = {};
        UniCharCount count = 0;
        const OSStatus status = UCKeyTranslate(
            layout, virtualKey, kUCKeyActionDisplay, 0, LMGetKbdType(),
            kUCKeyTranslateNoDeadKeysBit, &deadKeyState, 4, &count, chars);
        CFRelease(source);
        if (status != noErr || count == 0) return {};
        NSString* value = [NSString stringWithCharacters:chars length:count];
        const char* utf8 = [value UTF8String];
        return utf8 ? std::string(utf8) : std::string();
    }
}

std::string localizedKeyLabel(int sdlScancode) {
    const std::string name = localizedKeyName(sdlScancode);
    if (name.empty()) return name;
    @autoreleasepool {
        NSString* value = [NSString stringWithUTF8String:name.c_str()];
        if (!value) return name;
        // The layout's own casing rules, not ASCII's: this is the difference
        // between A and Ä having an uppercase form at all.
        NSString* upper = [value uppercaseString];
        // Unless uppercasing turns one key into two letters. German ß
        // uppercases to SS, and a key labelled SS is not a key anyone can find
        // on their keyboard - WoW's own deDE files label it ß.
        if ([upper length] != [value length]) upper = value;
        const char* utf8 = [upper UTF8String];
        return utf8 ? std::string(utf8) : name;
    }
}

std::string keyboardLayoutIdentifier() {
    @autoreleasepool {
        TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
        if (!source) return {};
        auto* identifier = static_cast<NSString*>(
            TISGetInputSourceProperty(source, kTISPropertyInputSourceID));
        std::string value;
        if (const char* utf8 = [identifier UTF8String]) value = utf8;
        CFRelease(source);
        return value;
    }
}

} // namespace core
} // namespace wowee
