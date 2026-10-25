#include "core/browser/duplicate_tabs.hpp"
#include "core/browser/profile.hpp"
#include <algorithm>
#include <map>
#include <tuple>
namespace aura {
std::vector<DuplicateGroup> group_duplicate_tabs(std::span<const DuplicateTab> tabs) {
    std::map<std::tuple<std::string, int, uint64_t>, DuplicateGroup> groups;
    for (const auto &t : tabs) {
        if (!t.ref.window || t.ref.tab <= 0 ||
            !(t.url.starts_with("http://") || t.url.starts_with("https://") ||
              t.url.starts_with("file://")) ||
            !valid_bookmark_address(t.url))
            continue;
        auto scope = t.private_mode ? t.ref.window : 0;
        auto &g = groups[{t.url, t.container, scope}];
        g.url = t.url;
        g.container = t.container;
        g.private_window = scope;
        if (std::none_of(g.tabs.begin(), g.tabs.end(),
                         [&](const auto &old) { return old.ref == t.ref; }))
            g.tabs.push_back(t);
    }
    std::vector<DuplicateGroup> result;
    for (auto &[key, g] : groups)
        if (g.tabs.size() > 1) {
            std::sort(g.tabs.begin(), g.tabs.end(), [](const auto &a, const auto &b) {
                return std::tie(a.ref.window, a.ref.tab) < std::tie(b.ref.window, b.ref.tab);
            });
            result.push_back(std::move(g));
        }
    return result;
}
bool same_duplicate_tab(const DuplicateTab &a, const DuplicateTab &b) {
    return a.ref == b.ref && a.navigation == b.navigation && a.url == b.url &&
           a.container == b.container && a.workspace == b.workspace && a.group == b.group &&
           a.private_mode == b.private_mode;
}
bool same_duplicate_group(const DuplicateGroup &a, const DuplicateGroup &b) {
    if (a.url != b.url || a.container != b.container || a.private_window != b.private_window ||
        a.tabs.size() != b.tabs.size())
        return false;
    for (const auto &t : a.tabs) {
        const auto found = std::find_if(b.tabs.begin(), b.tabs.end(), [&](const auto &r) {
            return same_duplicate_tab(t, r) && t.protection == r.protection;
        });
        if (found == b.tabs.end())
            return false;
    }
    return true;
}
DuplicateRef preferred_duplicate(const DuplicateGroup &g) {
    for (const auto &t : g.tabs)
        if (t.protection & DuplicatePinned)
            return t.ref;
    for (const auto &t : g.tabs)
        if (t.current)
            return t.ref;
    return g.tabs.empty() ? DuplicateRef{} : g.tabs.front().ref;
}
std::vector<DuplicateTab> duplicate_close_plan(const DuplicateGroup &g, DuplicateRef keep) {
    if (std::none_of(g.tabs.begin(), g.tabs.end(), [&](const auto &t) { return t.ref == keep; }))
        return {};
    std::vector<DuplicateTab> result;
    for (const auto &t : g.tabs)
        if (t.ref != keep && !t.protection)
            result.push_back(t);
    return result;
}
} // namespace aura
