#pragma once
#include <span>
#include <string>
#include <vector>
namespace aura {
struct Hotkey {
    int key = 0, modifiers = 0;
    bool operator==(const Hotkey &) const = default;
};
struct HotkeySpec {
    const char *id, *title;
    Hotkey standard;
};
struct HotkeyOverride {
    std::string action;
    Hotkey chord;
};
std::span<const HotkeySpec> hotkey_catalog();
Hotkey effective_hotkey(const std::vector<HotkeyOverride> &, const std::string &);
std::string hotkey_label(Hotkey);
bool allowed_hotkey(Hotkey);
bool valid_hotkeys(const std::vector<HotkeyOverride> &);
// Returns empty on success, a conflicting command id, or "reserved" / "unknown".
std::string assign_hotkey(std::vector<HotkeyOverride> &, const std::string &, Hotkey);
} // namespace aura
