#include "core/browser/browser.hpp"
#include <algorithm>
#include <iterator>
#include <set>
namespace aura {
std::vector<TreeTab> BrowserController::tree_nodes() const {
    std::vector<TreeTab> nodes;
    for (const auto &t : tabs)
        nodes.push_back({t->id, t->parent, t->workspace, t->group, t->container, t->private_tab,
                         t->tree_collapsed});
    return nodes;
}
std::vector<TreeRow> BrowserController::tree_rows(bool hide_collapsed) const {
    const auto nodes = tree_nodes();
    std::vector<TreeTab> ordered;
    std::vector<int> indices;
    for (const auto &g : profile.groups)
        if (!private_window)
            for (int i = 0; i < int(tabs.size()); ++i)
                if (tabs[i]->group == g.id && tabs[i]->workspace == g.workspace) {
                    ordered.push_back(nodes[i]);
                    indices.push_back(i);
                }
    for (int i = 0; i < int(tabs.size()); ++i) {
        const auto g = profile.group(tabs[i]->group);
        if (private_window || !g || g->workspace != tabs[i]->workspace) {
            ordered.push_back(nodes[i]);
            indices.push_back(i);
        }
    }
    auto rows = tab_tree_rows(ordered, hide_collapsed && profile.tree_tabs);
    for (auto &r : rows)
        r.index = indices[r.index];
    std::erase_if(rows, [&](const auto &r) { return tabs[r.index]->page.closed; });
    return rows;
}
void BrowserController::normalize_tree() {
    const auto parents = tab_tree_parents(tree_nodes());
    for (int i = 0; i < int(tabs.size()); ++i)
        tabs[i]->parent = parents[i] < 0 ? 0 : tabs[parents[i]]->id;
}
void BrowserController::detach_tree_tab(int id) {
    if (auto t = tab(id)) {
        for (auto &child : tabs)
            if (child->parent == id)
                child->parent = t->parent;
        t->parent = 0;
        normalize_tree();
        changed(true);
    }
}
void BrowserController::reveal_tree(int id) {
    if (!profile.tree_tabs)
        return;
    normalize_tree();
    auto t = tab(id);
    while (t && t->parent) {
        t = tab(t->parent);
        if (t)
            t->tree_collapsed = false;
    }
}
void BrowserController::toggle_tree(int id) {
    if (!profile.tree_tabs)
        return;
    for (const auto &r : tree_rows(false))
        if (tabs[r.index]->id == id && r.descendants) {
            tabs[r.index]->tree_collapsed = !tabs[r.index]->tree_collapsed;
            changed(true);
            return;
        }
}
void BrowserController::move_tab(int from, int to) {
    if (from < 0 || to < 0 || from >= int(tabs.size()) || to >= int(tabs.size()) || from == to)
        return;
    if (tabs[from]->workspace != tabs[to]->workspace)
        return;
    const int selected = current() ? current()->id : 0, source = tabs[from]->id,
              target = tabs[to]->id;
    std::set<int> branch{source};
    if (profile.tree_tabs) {
        bool inside = false;
        int depth = 0;
        for (const auto &r : tree_rows(false)) {
            if (r.index == from) {
                inside = true;
                depth = r.depth;
            } else if (inside && r.depth <= depth)
                break;
            if (inside)
                branch.insert(tabs[r.index]->id);
        }
        if (branch.contains(target))
            return;
        const auto group = tabs[to]->group;
        for (auto &t : tabs)
            if (branch.contains(t->id))
                t->group = group;
        tabs[from]->parent = tabs[to]->parent;
    } else
        detach_tree_tab(source);
    std::vector<std::unique_ptr<Tab>> moving, rest;
    for (auto &t : tabs)
        (branch.contains(t->id) ? moving : rest).push_back(std::move(t));
    auto position =
        std::find_if(rest.begin(), rest.end(), [&](const auto &t) { return t->id == target; });
    rest.insert(position, std::make_move_iterator(moving.begin()),
                std::make_move_iterator(moving.end()));
    tabs = std::move(rest);
    for (int i = 0; i < int(tabs.size()); ++i)
        if (tabs[i]->id == selected)
            active = i;
    normalize_tree();
    changed(true);
}
} // namespace aura
