// setting_text.hpp - how a setting's value is spelled as text.
//
// Two helpers, and they are here rather than beside the settings schema
// because three layers need them and only one of those is the interface: the
// schema and its panel in src/ui, the Lua CVar API in src/addons, and the CVar
// store in this library, which converts a setting into its CVar's units and
// has to spell the result the same way the panel would. A copy in the third
// place is a copy that disagrees - which is exactly what "written twice, once
// on each side of the bridge" below is about.
#pragma once

#include <string>

namespace wowee::core {

/// A setting's value as a string, the way a CVar carries one.
///
/// A whole number with no decimal point, a fraction without the trailing zeros.
/// std::to_string gives six decimals for everything, and the options panels
/// compare some of these as strings - a checkbox tests `value == "1"`, which
/// "1.000000" fails, and the box unticks itself every time the panel opens.
///
/// Written twice before this, once on each side of the bridge, ninety minutes
/// apart.
inline std::string settingNumberText(double v) {
    if (v == static_cast<long long>(v)) return std::to_string(static_cast<long long>(v));
    std::string s = std::to_string(v);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

/// Whether a setting string means on. Empty and "0" are the only falses - a
/// CVar arrives as a string, and in Lua every string including "0" is true, so
/// the test cannot be left to the caller.
inline bool settingIsOn(const std::string& v) { return !v.empty() && v != "0"; }

}  // namespace wowee::core
