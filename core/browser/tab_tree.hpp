#pragma once
#include <span>
#include <vector>
namespace aura {
struct TreeTab {
    int id = 0, parent = 0, workspace = 0, group = 0, container = 0;
    bool private_mode = false, collapsed = false;
};
struct TreeRow {
    int index = 0, depth = 0, descendants = 0;
};
// Returns parent indices, with missing, cross-context and cyclic edges removed.
std::vector<int> tab_tree_parents(std::span<const TreeTab> tabs);
std::vector<TreeRow> tab_tree_rows(std::span<const TreeTab> tabs, bool hide_collapsed = true);
} // namespace aura
