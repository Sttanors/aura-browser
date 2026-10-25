#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace aura {
struct ProcessTab {
    uint64_t window = 0;
    int tab = 0;
    uint64_t navigation = 0;
    std::string title;
    bool operator==(const ProcessTab &) const = default;
};
struct ProcessTask {
    int64_t task = -1;
    uint64_t process = 0;
    std::string kind;
    double cpu = -1;
    int64_t memory = -1;
    bool killable = false, hidden_tabs = false;
    std::vector<ProcessTab> tabs;
};
// Metrics are already per process in CEF. Grouping never sums duplicate values.
uint64_t renderer_process_key(std::string_view frame_identifier);
std::vector<ProcessTask> group_process_tasks(std::vector<ProcessTask>);
bool same_process_target(const ProcessTask &, const ProcessTask &);
} // namespace aura
