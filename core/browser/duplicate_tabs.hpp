#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace aura {
struct DuplicateRef {
    uint64_t window = 0;
    int tab = 0;
    bool operator==(const DuplicateRef &) const = default;
};
enum DuplicateProtection : unsigned {
    DuplicatePinned = 1,
    DuplicateBusy = 2,
    DuplicateMedia = 4,
    DuplicateDownload = 8,
    DuplicateDialog = 16
};
struct DuplicateTab {
    DuplicateRef ref;
    uint64_t navigation = 0;
    std::string url, title;
    int container = 0, workspace = 0, group = 0;
    bool private_mode = false, current = false;
    unsigned protection = 0;
};
struct DuplicateGroup {
    std::string url;
    int container = 0;
    uint64_t private_window = 0;
    std::vector<DuplicateTab> tabs;
};
std::vector<DuplicateGroup> group_duplicate_tabs(std::span<const DuplicateTab>);
bool same_duplicate_tab(const DuplicateTab &, const DuplicateTab &);
bool same_duplicate_group(const DuplicateGroup &, const DuplicateGroup &);
DuplicateRef preferred_duplicate(const DuplicateGroup &);
std::vector<DuplicateTab> duplicate_close_plan(const DuplicateGroup &, DuplicateRef keep);
} // namespace aura
