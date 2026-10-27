#include "core/browser/process_tasks.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <tuple>
namespace aura {
uint64_t renderer_process_key(std::string_view identifier) {
    // Pinned CEF frame_util::MakeFrameIdentifier: hexadecimal child_id + '-' + frame token.
    const auto separator = identifier.find('-');
    if (separator == 0 || separator == std::string_view::npos || separator > 8 ||
        separator + 1 == identifier.size())
        return 0;
    uint64_t key = 0;
    auto parsed = std::from_chars(identifier.data(), identifier.data() + separator, key, 16);
    return parsed.ec == std::errc{} && parsed.ptr == identifier.data() + separator &&
                   key <= 0x7fffffffULL
               ? key
               : 0;
}

std::vector<ProcessTask> group_process_tasks(std::vector<ProcessTask> input) {
    std::vector<ProcessTask> result;
    for (auto &task : input) {
        if (!std::isfinite(task.cpu) || task.cpu < 0)
            task.cpu = -1;
        auto found = task.process
                         ? std::find_if(result.begin(), result.end(),
                                        [&](const auto &r) { return r.process == task.process; })
                         : result.end();
        if (found == result.end())
            result.push_back(std::move(task));
        else {
            found->killable = found->killable && task.killable;
            found->hidden_tabs = found->hidden_tabs || task.hidden_tabs;
            found->cpu = std::max(found->cpu, task.cpu);
            found->memory = std::max(found->memory, task.memory);
            found->tabs.insert(found->tabs.end(), task.tabs.begin(), task.tabs.end());
        }
    }
    for (auto &r : result) {
        std::sort(r.tabs.begin(), r.tabs.end(), [](const auto &a, const auto &b) {
            return std::tie(a.window, a.tab) < std::tie(b.window, b.tab);
        });
        r.tabs.erase(std::unique(r.tabs.begin(), r.tabs.end()), r.tabs.end());
        r.killable = r.killable && r.process && !r.hidden_tabs && !r.tabs.empty() &&
                     r.kind == "Page process";
    }
    std::erase_if(result, [](const auto &r) { return r.hidden_tabs && r.tabs.empty(); });
    return result;
}
bool same_process_target(const ProcessTask &a, const ProcessTask &b) {
    if (!a.killable || !b.killable || a.process != b.process || a.task != b.task ||
        a.tabs.size() != b.tabs.size())
        return false;
    for (size_t i = 0; i < a.tabs.size(); ++i)
        if (a.tabs[i].window != b.tabs[i].window || a.tabs[i].tab != b.tabs[i].tab ||
            a.tabs[i].navigation != b.tabs[i].navigation)
            return false;
    return true;
}
} // namespace aura
