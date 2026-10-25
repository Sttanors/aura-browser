#include "common/platform.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_task_manager.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
#include <map>
#include <tuple>
namespace aura {
namespace {
const char *kind(cef_task_type_t type) {
    switch (type) {
    case CEF_TASK_TYPE_BROWSER:
        return "Browser process";
    case CEF_TASK_TYPE_RENDERER:
        return "Page process";
    case CEF_TASK_TYPE_GPU:
        return "Graphics task";
    case CEF_TASK_TYPE_EXTENSION:
        return "Extension task";
    case CEF_TASK_TYPE_DEDICATED_WORKER:
    case CEF_TASK_TYPE_SHARED_WORKER:
    case CEF_TASK_TYPE_SERVICE_WORKER:
        return "Worker task";
    default:
        return "System task";
    }
}
auto topology(const BrowserState &state) {
    std::vector<std::tuple<uint64_t, int, int, uint64_t, std::string, bool>> result;
    for (const auto *window : state.windows)
        for (const auto &t : window->tabs)
            if (t->browser)
                result.emplace_back(window->window_uid, t->id, t->browser->GetIdentifier(),
                                    t->navigation_serial,
                                    t->browser->GetMainFrame()
                                        ? t->browser->GetMainFrame()->GetIdentifier().ToString()
                                        : "",
                                    t->closing);
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace
std::vector<ProcessTask> BrowserController::process_tasks(bool *available) const {
    auto manager = state_->task_manager;
    std::vector<int64_t> ids;
    const bool ready = manager && manager->GetTaskIdsList(ids);
    if (available)
        *available = ready;
    if (!ready)
        return {};
    std::vector<ProcessTask> tasks;
    std::map<int64_t, size_t> positions;
    for (auto id : ids) {
        CefTaskInfo info;
        if (!manager->GetTaskInfo(id, info))
            continue;
        ProcessTask row;
        row.task = id;
        row.kind = kind(info.type);
        row.cpu = info.memory > 0 ? info.cpu_usage : -1;
        row.memory = info.memory > 0 ? info.memory : -1;
        row.killable = info.type == CEF_TASK_TYPE_RENDERER && info.is_killable;

        positions[id] = tasks.size();
        tasks.push_back(std::move(row));
    }
    for (auto *window : state_->windows)
        for (const auto &t : window->tabs) {
            if (!t->browser || t->closing)
                continue;
            const auto found =
                positions.find(manager->GetTaskIdForBrowserId(t->browser->GetIdentifier()));
            if (found == positions.end())
                continue;
            auto &row = tasks[found->second];
            const auto frame = t->browser->GetMainFrame();
            const auto process =
                frame ? renderer_process_key(frame->GetIdentifier().ToString()) : 0;
            if (process) {
                if (row.process && row.process != process)
                    row.killable = false;
                row.process = process;
            }
            row.killable = row.killable && process;
            const bool visible = private_window ? window == this && t->private_tab
                                                : !t->private_tab || window == this;
            if (visible)
                row.tabs.push_back({window->window_uid, t->id, t->navigation_serial,
                                    t->page.title.empty() ? tr("Tab") : t->page.title});
            else
                row.hidden_tabs = true;
        }
    for (auto &r : tasks)
        if (!r.tabs.empty() && !r.process) {
            r.cpu = -1;
            r.memory = -1;
        }
    // Unmapped iframe/worker/system tasks can share process metrics without exposing
    // their process identity. Display main page processes and the browser, not duplicate totals.
    std::erase_if(tasks, [](const auto &r) {
        return r.tabs.empty() && !r.hidden_tabs && r.kind != "Browser process";
    });
    return group_process_tasks(std::move(tasks));
}
void BrowserController::end_process(const ProcessTask &selected) {
    if (!selected.killable || quitting)
        return;
    const auto current = process_tasks();
    const auto found = std::find_if(current.begin(), current.end(), [&](const auto &r) {
        return same_process_target(selected, r);
    });
    if (found == current.end()) {
        ui->toast(tr("The process list changed. Review it and try again."));
        return;
    }
    const auto tree = topology(*state_);
    std::string message =
        tr("All tabs in this process will stop. Unsaved changes will be lost.") + "\n";
    message += tr("Affected tabs: ") + std::to_string(selected.tabs.size());
    for (size_t i = 0; i < std::min<size_t>(5, selected.tabs.size()); ++i) {
        auto title = selected.tabs[i].title;
        for (auto &c : title)
            if (static_cast<unsigned char>(c) < 32 || c == 127)
                c = ' ';
        size_t end = std::min<size_t>(120, title.size());
        while (end < title.size() && end && (static_cast<unsigned char>(title[end]) & 0xc0) == 0x80)
            --end;
        title.resize(end);
        message += "\n" + title;
    }
    if (selected.tabs.size() > 5)
        message += "\n" + tr("Other affected tabs: ") + std::to_string(selected.tabs.size() - 5);
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    confirm_action(tr("End page process?"), message, [this, weak, selected, tree] {
        if (weak.expired() || quitting)
            return;
        auto manager = state_->task_manager;
        auto latest = process_tasks();
        if (topology(*state_) != tree ||
            std::none_of(latest.begin(), latest.end(),
                         [&](const auto &r) { return same_process_target(selected, r); })) {
            ui->toast(tr("The process list changed. Review it and try again."));
            return;
        }
        // Only CEF can terminate the task; the renderer key is never passed to an OS kill API.
        auto mark = [&](bool ended) {
            for (const auto &affected : selected.tabs)
                for (auto *window : state_->windows)
                    if (window->window_uid == affected.window)
                        if (auto t = window->tab(affected.tab))
                            t->process_ended = ended;
        };
        mark(true);
        if (!manager || !manager->KillTask(selected.task)) {
            mark(false);
            ui->toast(tr("Could not end this page process."));
        } else {
            ++audit["page_processes_ended"];
            ui->toast(tr("Page process ended. Reload its tabs to restore them."));
        }
        changed();
    });
    dialogs.back().accept_label = tr("End process");
}
} // namespace aura
