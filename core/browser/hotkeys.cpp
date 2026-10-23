#include "core/browser/hotkeys.hpp"
#include <algorithm>
#include <set>
namespace aura {
// Modifiers are platform independent: Ctrl=1, Shift=2, Alt=4.
std::span<const HotkeySpec> hotkey_catalog() {
    static constexpr HotkeySpec entries[] = {{"focus_address", "Address bar", {'L', 1}},
                                             {"commands", "Command palette", {'K', 1}},
                                             {"new_tab", "New tab", {'T', 1}},
                                             {"private_tab", "Private tab", {'N', 5}},
                                             {"new_window", "New window", {'N', 1}},
                                             {"private_window", "New private window", {'N', 3}},
                                             {"reopen_tab", "Reopen closed tab", {'T', 3}},
                                             {"close_tab", "Close tab", {'W', 1}},
                                             {"close_window", "Close window", {'W', 3}},
                                             {"quit", "Quit Aura", {'Q', 1}},
                                             {"reload", "Reload", {'R', 1}},
                                             {"force_reload", "Reload without cache", {'R', 3}},
                                             {"find", "Find in page", {'F', 1}},
                                             {"bookmark", "Save bookmark", {'D', 1}},
                                             {"bookmarks", "Bookmarks", {'B', 1}},
                                             {"history", "History", {'H', 1}},
                                             {"downloads", "Downloads", {'J', 1}},
                                             {"print", "Print", {'P', 1}},
                                             {"passwords", "Passwords", {'Y', 3}},
                                             {"workspaces", "Workspaces", {'E', 3}},
                                             {"groups", "Tab groups", {'G', 5}},
                                             {"containers", "Site containers", {'C', 5}},
                                             {"profiles", "Profiles", {'P', 5}},
                                             {"split", "Two pages side by side", {'V', 5}},
                                             {"sleep", "Sleep tab", {'S', 5}},
                                             {"reader", "Reader mode", {'R', 5}},
                                             {"translate_page", "Translate page", {'T', 5}},
                                             {"permissions", "Site permissions", {'I', 5}},
                                             {"site_data", "Site data", {'D', 5}},
                                             {"sessions", "Session snapshots", {'E', 5}},
                                             {"screenshot", "Screenshot", {'S', 3}},
                                             {"devtools", "Developer tools", {0x7B, 0}},
                                             {"settings", "Settings", {}},
                                             {"hotkeys", "Keyboard shortcuts", {}}};
    return entries;
}
Hotkey effective_hotkey(const std::vector<HotkeyOverride> &overrides, const std::string &id) {
    for (const auto &entry : overrides)
        if (entry.action == id)
            return entry.chord;
    for (const auto &entry : hotkey_catalog())
        if (entry.id == id)
            return entry.standard;
    return {};
}
bool allowed_hotkey(Hotkey chord) {
    const auto [key, mods] = chord;
    if (!key)
        return !mods;
    if (mods < 0 || mods > 7)
        return false;
    if (!((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || (key >= 0x70 && key <= 0x7B)))
        return false;
    if (!(mods & 5) && !(key >= 0x70 && key <= 0x7B))
        return false;
    if (mods == 1 && (key == 'A' || key == 'C' || key == 'V' || key == 'X' || key == 'Z' ||
                      key == 'Y' || (key >= '0' && key <= '9')))
        return false;
    if (mods == 3 && (key == 'Z' || key == '0'))
        return false;
    if ((mods & 5) == 5 && key >= 0x70 && key <= 0x7B)
        return false;
    if ((mods & 4) && key == 0x73)
        return false; // Alt+F4 belongs to the window manager.
    if (!(mods & 5) && (key == 0x72 || key == 0x74 || key == 0x75 || key == 0x76 || key == 0x79))
        return false;
    return true;
}
bool valid_hotkeys(const std::vector<HotkeyOverride> &overrides) {
    if (overrides.size() > hotkey_catalog().size())
        return false;
    std::set<std::string> ids;
    std::set<std::pair<int, int>> chords;
    for (const auto &entry : overrides) {
        if (!ids.insert(entry.action).second || !allowed_hotkey(entry.chord) ||
            std::none_of(hotkey_catalog().begin(), hotkey_catalog().end(),
                         [&](const auto &s) { return entry.action == s.id; }))
            return false;
    }
    for (const auto &s : hotkey_catalog()) {
        const auto h = effective_hotkey(overrides, s.id);
        if (h.key && !chords.insert({h.key, h.modifiers}).second)
            return false;
    }
    return true;
}
std::string assign_hotkey(std::vector<HotkeyOverride> &overrides, const std::string &id,
                          Hotkey chord) {
    const auto specs = hotkey_catalog();
    const auto spec =
        std::find_if(specs.begin(), specs.end(), [&](const auto &s) { return id == s.id; });
    if (spec == specs.end())
        return "unknown";
    if (!allowed_hotkey(chord))
        return "reserved";
    if (chord.key)
        for (const auto &s : specs)
            if (id != s.id && effective_hotkey(overrides, s.id) == chord)
                return s.id;
    std::erase_if(overrides, [&](const auto &entry) { return entry.action == id; });
    if (chord != spec->standard)
        overrides.push_back({id, chord});
    return {};
}
std::string hotkey_label(Hotkey h) {
    if (!h.key)
        return "Not assigned";
    std::string out;
    if (h.modifiers & 1)
        out += "Ctrl+";
    if (h.modifiers & 2)
        out += "Shift+";
    if (h.modifiers & 4)
        out += "Alt+";
    if (h.key >= 0x70 && h.key <= 0x7B)
        out += 'F' + std::to_string(h.key - 0x6F);
    else
        out += char(h.key);
    return out;
}
} // namespace aura
