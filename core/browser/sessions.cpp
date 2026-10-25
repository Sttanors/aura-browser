#include "common/text.hpp"
#include "core/browser/profile.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <set>
namespace aura {
namespace {
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lower(std::string s) { return fold_text(s); }
bool name(const std::string &s) {
    return !s.empty() && s.size() <= 120 &&
           std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string prefix(std::string s) {
    if (s.size() <= 96)
        return s;
    size_t end = 96;
    while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80)
        --end;
    s.resize(end);
    return s;
}
bool address(const std::string &url) {
    if (url.empty() || url.size() > 16384)
        return false;
    if (!(url.starts_with("http://") || url.starts_with("https://") || url.starts_with("file://") ||
          url == "aura://newtab" || url == "about:blank"))
        return false;
    try {
        return !navigation_url(url).empty();
    } catch (...) {
        return false;
    }
}
} // namespace
bool valid_named_session(const NamedSession &s) {
    if (s.id <= 0 || s.id > 1000000 || !name(s.name) || s.windows.empty() ||
        s.windows.size() > 32 || s.workspaces.size() > 32 || s.groups.size() > 128 || !s.revision ||
        s.revision > 1000000000000ULL)
        return false;
    std::set<int> spaces, groups;
    std::map<int, int> group_spaces;
    for (const auto &w : s.workspaces)
        if (w.id < 0 || w.id > 1000000 || !name(w.name) || !spaces.insert(w.id).second)
            return false;
    if (!spaces.contains(0))
        return false;
    for (const auto &g : s.groups) {
        if (g.id <= 0 || g.id > 1000000 || !name(g.name) || !spaces.contains(g.workspace) ||
            g.color < 0 || g.color > 5 || !groups.insert(g.id).second)
            return false;
        group_spaces[g.id] = g.workspace;
    }
    size_t count = 0;
    for (const auto &w : s.windows) {
        count += w.tabs.size();
        if (w.tabs.empty() || count > 200 || w.active < 0 || w.active >= int(w.tabs.size()) ||
            !spaces.contains(w.workspace))
            return false;
        for (const auto &t : w.tabs)
            if (!address(t.url) || !spaces.contains(t.workspace) || t.container < 0 ||
                t.container > 1000000 ||
                (t.group && (!groups.contains(t.group) || group_spaces[t.group] != t.workspace)))
                return false;
        auto tree = w;
        normalize_tab_tree(tree);
        for (size_t i = 0; i < w.tabs.size(); ++i)
            if (tree.tabs[i].parent != w.tabs[i].parent)
                return false;
        if (w.tabs[w.active].workspace != w.workspace)
            return false;
        const int a = w.split_left, b = w.split_right;
        if (a != -1 || b != -1)
            if (a < 0 || b < 0 || a == b || a >= int(w.tabs.size()) || b >= int(w.tabs.size()) ||
                (w.active != a && w.active != b) || w.tabs[a].workspace != w.workspace ||
                w.tabs[b].workspace != w.workspace || w.tabs[a].url == "aura://newtab" ||
                w.tabs[b].url == "aura://newtab")
                return false;
    }
    return true;
}
const NamedSession *Profile::named_session(int id) const {
    for (const auto &s : named_sessions)
        if (s.id == id)
            return &s;
    return nullptr;
}
int Profile::save_named_session(std::string title, std::vector<SavedWindow> saved, int replace) {
    title = trim(std::move(title));
    if (!name(title) || (!replace && named_sessions.size() >= 32))
        return -1;
    for (const auto &s : named_sessions)
        if (s.id != replace && lower(s.name) == lower(title))
            return -1;
    const auto old = named_session(replace);
    if (replace && !old)
        return -1;
    NamedSession s;
    s.id = replace ? replace : next_named_session;
    s.name = std::move(title);
    s.created = old ? old->created : std::time(nullptr);
    s.updated = std::time(nullptr);
    s.revision = old ? old->revision + 1 : 1;
    s.windows = std::move(saved);
    std::set<int> used_spaces{0}, used_groups;
    for (const auto &w : s.windows) {
        used_spaces.insert(w.workspace);
        for (const auto &t : w.tabs) {
            used_spaces.insert(t.workspace);
            if (t.group)
                used_groups.insert(t.group);
        }
    }
    for (const auto &w : workspaces)
        if (used_spaces.contains(w.id))
            s.workspaces.push_back(w);
    for (const auto &g : groups)
        if (used_groups.contains(g.id))
            s.groups.push_back(g);
    if (!valid_named_session(s))
        return -1;
    for (const auto &w : s.windows)
        for (const auto &t : w.tabs)
            if (!container(t.container))
                return -1;
    if (replace) {
        for (auto &entry : named_sessions)
            if (entry.id == replace) {
                entry = std::move(s);
                return replace;
            }
    }
    next_named_session = s.id + 1;
    named_sessions.push_back(std::move(s));
    return named_sessions.back().id;
}
bool Profile::rename_named_session(int id, std::string title) {
    title = trim(std::move(title));
    if (!name(title))
        return false;
    for (const auto &s : named_sessions)
        if (s.id != id && lower(s.name) == lower(title))
            return false;
    for (auto &s : named_sessions)
        if (s.id == id && s.revision < 1000000000000ULL) {
            s.name = std::move(title);
            s.updated = std::time(nullptr);
            ++s.revision;
            return true;
        }
    return false;
}
bool Profile::remove_named_session(int id) {
    const auto size = named_sessions.size();
    std::erase_if(named_sessions, [&](const auto &s) { return s.id == id; });
    return named_sessions.size() != size;
}
bool Profile::named_session_available(int id) const {
    const auto s = named_session(id);
    if (!s || !valid_named_session(*s))
        return false;
    for (const auto &w : s->windows)
        for (const auto &t : w.tabs)
            if (!container(t.container))
                return false;
    return true;
}
std::optional<std::vector<SavedWindow>> Profile::prepare_named_session(int id) {
    if (!named_session_available(id))
        return {};
    const auto source = *named_session(id);
    auto result = source.windows;
    struct Rollback {
        Profile &profile;
        std::vector<Workspace> spaces;
        std::vector<TabGroup> groups;
        bool committed = false;
        ~Rollback() {
            if (!committed) {
                profile.workspaces = std::move(spaces);
                profile.groups = std::move(groups);
            }
        }
    };
    Rollback rollback{*this, workspaces, this->groups};
    auto &updated = *this;
    std::map<int, int> spaces{{0, 0}}, groups{{0, 0}};
    for (const auto &w : source.workspaces)
        if (w.id) {
            auto found =
                std::find_if(updated.workspaces.begin(), updated.workspaces.end(),
                             [&](const auto &item) { return lower(item.name) == lower(w.name); });
            const int target =
                found == updated.workspaces.end() ? updated.add_workspace(w.name) : found->id;
            if (target < 0)
                return {};
            spaces[w.id] = target;
        }
    for (const auto &g : source.groups) {
        auto found =
            std::find_if(updated.groups.begin(), updated.groups.end(), [&](const auto &item) {
                return item.workspace == spaces[g.workspace] && item.name == g.name &&
                       item.color == g.color && item.collapsed == g.collapsed;
            });
        int target = found == updated.groups.end() ? -1 : found->id;
        if (target < 0) {
            target = updated.add_group(spaces[g.workspace], g.name, g.color);
            for (int n = 1; target < 0 && n <= 128 && updated.groups.size() < 128; ++n)
                target = updated.add_group(
                    spaces[g.workspace], prefix(g.name) + " (" + std::to_string(n) + ")", g.color);
            if (target < 0)
                return {};
            updated.group(target)->collapsed = g.collapsed;
        }
        groups[g.id] = target;
    }
    for (auto &w : result) {
        w.workspace = spaces[w.workspace];
        for (auto &t : w.tabs) {
            t.workspace = spaces[t.workspace];
            t.group = groups[t.group];
        }
    }
    rollback.committed = true;
    return result;
}
} // namespace aura
