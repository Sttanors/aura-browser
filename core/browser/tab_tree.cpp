#include "core/browser/tab_tree.hpp"
#include "core/browser/profile.hpp"
#include <algorithm>
#include <map>
namespace aura {
std::vector<int> tab_tree_parents(std::span<const TreeTab> tabs) {
    std::map<int, int> ids;
    for (int i = 0; i < int(tabs.size()); ++i)
        if (tabs[i].id > 0 && !ids.emplace(tabs[i].id, i).second)
            ids[tabs[i].id] = -1;
    std::vector<int> parents(tabs.size(), -1), color(tabs.size());
    for (int i = 0; i < int(tabs.size()); ++i) {
        auto found = ids.find(tabs[i].parent);
        if (found == ids.end() || found->second < 0 || found->second == i)
            continue;
        const auto &a = tabs[i], &b = tabs[found->second];
        if (a.workspace == b.workspace && a.group == b.group && a.container == b.container &&
            a.private_mode == b.private_mode)
            parents[i] = found->second;
    }
    for (int i = 0; i < int(tabs.size()); ++i) {
        std::vector<int> path;
        int cursor = i;
        while (cursor >= 0 && color[cursor] == 0) {
            color[cursor] = 1;
            path.push_back(cursor);
            cursor = parents[cursor];
        }
        if (cursor >= 0 && color[cursor] == 1) {
            const auto start = std::find(path.begin(), path.end(), cursor);
            for (auto it = start; it != path.end(); ++it)
                parents[*it] = -1;
        }
        for (int index : path)
            color[index] = 2;
    }
    return parents;
}
std::vector<TreeRow> tab_tree_rows(std::span<const TreeTab> tabs, bool hide_collapsed) {
    const auto parents = tab_tree_parents(tabs);
    std::vector<std::vector<int>> children(tabs.size());
    std::vector<TreeRow> stack, all;
    for (int i = int(tabs.size()) - 1; i >= 0; --i)
        if (parents[i] < 0)
            stack.push_back({i, 0, 0});
    for (int i = 0; i < int(tabs.size()); ++i)
        if (parents[i] >= 0)
            children[parents[i]].push_back(i);
    while (!stack.empty()) {
        const auto row = stack.back();
        stack.pop_back();
        all.push_back(row);
        for (auto it = children[row.index].rbegin(); it != children[row.index].rend(); ++it)
            stack.push_back({*it, row.depth + 1, 0});
    }
    std::vector<int> counts(tabs.size());
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        it->descendants = counts[it->index];
        if (parents[it->index] >= 0)
            counts[parents[it->index]] += it->descendants + 1;
    }
    std::vector<TreeRow> result;
    int hidden_depth = -1;
    for (const auto &row : all) {
        if (hidden_depth >= 0 && row.depth > hidden_depth)
            continue;
        hidden_depth = hide_collapsed && tabs[row.index].collapsed ? row.depth : -1;
        result.push_back(row);
    }
    return result;
}
void normalize_tab_tree(SavedWindow &window) {
    std::vector<TreeTab> nodes;
    for (int i = 0; i < int(window.tabs.size()); ++i) {
        const auto &t = window.tabs[i];
        nodes.push_back({i + 1,
                         t.parent >= 0 && t.parent < int(window.tabs.size()) ? t.parent + 1 : 0,
                         t.workspace, t.group, t.container});
    }
    const auto parents = tab_tree_parents(nodes);
    for (int i = 0; i < int(window.tabs.size()); ++i)
        window.tabs[i].parent = parents[i];
}
void remap_saved_tabs(SavedWindow &window, std::span<const int> source_indices) {
    std::map<int, int> remap;
    for (int i = 0; i < int(source_indices.size()); ++i)
        remap[source_indices[i]] = i;
    auto index = [&](int old) {
        auto it = remap.find(old);
        return it == remap.end() ? -1 : it->second;
    };
    for (auto &t : window.tabs)
        t.parent = index(t.parent);
    const int active = index(window.active);
    window.active = active >= 0 ? active : 0;
    window.split_left = index(window.split_left);
    window.split_right = index(window.split_right);
    normalize_tab_tree(window);
}
} // namespace aura
