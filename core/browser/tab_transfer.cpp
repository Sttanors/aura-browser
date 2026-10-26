#include "core/browser/browser.hpp"
#include "core/browser/client.hpp"
#include "ui/browser_ui.hpp"
#include <algorithm>
namespace aura {
std::string BrowserController::tab_transfer_problem(int id, const BrowserController &target) const {
    const auto it =
        std::find_if(tabs.begin(), tabs.end(), [id](const auto &t) { return t->id == id; });
    if (it == tabs.end() || quitting || target.quitting || state_->exiting || &target == this ||
        state_ != target.state_)
        return "This window is unavailable.";
    const auto &t = **it;
    if (t.private_tab != target.private_window)
        return "Choose a window with the same browsing mode.";
    if (t.closing || t.page.closed || t.creation_pending || t.popup_pending || t.sleep_pending || t.discard_pending ||
        t.reader_pending || t.reading_pending || (t.browser && !t.site_tools_ready) ||
        (t.devtools && t.devtools->busy()) || !pending_popups_.empty() || !pending_.empty() ||
        tools_open || target.tools_open || !dialogs.empty() || !target.dialogs.empty() ||
        menu_callback || target.menu_callback || screenshot.pending || screenshot.selecting ||
        screenshot.native_running || target.screenshot.pending || target.screenshot.selecting ||
        target.screenshot.native_running)
        return "Finish the current dialog or page operation before moving the tab.";
    if (state_->duplicate_merge.active)
        return "Stop duplicate cleanup before moving tabs.";
    if (std::any_of(downloads.begin(), downloads.end(), [&](const auto &d) {
            return d.source_window == window_uid && d.source_tab == id &&
                   d.checksum_status == "hashing";
        }))
        return "Finish the current dialog or page operation before moving the tab.";
    // A download whose source is not yet known cannot be rebound safely.
    if (std::any_of(downloads.begin(), downloads.end(), [&](const auto &d) {
            return d.source_window == window_uid && !d.source_tab && !d.complete && !d.cancelled &&
                   !d.interrupted;
        }))
        return "Wait for the download to start before moving the tab.";
    return {};
}
bool BrowserController::move_tab_to_window(int id, BrowserController &target, int before) {
    const auto problem = tab_transfer_problem(id, target);
    if (!problem.empty()) {
        ui->toast(tr(problem));
        return false;
    }
    auto source =
        std::find_if(tabs.begin(), tabs.end(), [id](const auto &t) { return t->id == id; });
    const int old_active = current() ? current()->id : 0;
    const int source_index = int(source - tabs.begin());
    close_extension_popup(false);
    target.close_extension_popup(false);
    close_menu();
    target.close_menu();
    // Moving one tab promotes its children in the original window. No cross-window tree links.
    detach_tree_tab(id);
    if (split_tabs[0] == id || split_tabs[1] == id)
        end_split();
    for (auto it = workspace_selection_.begin(); it != workspace_selection_.end();)
        if (it->second == id)
            it = workspace_selection_.erase(it);
        else
            ++it;
    if (site_data.tab == id) {
        const auto serial = site_data.serial + 1;
        site_data = {};
        site_data.serial = serial;
    }
    auto moved = std::move(*source);
    tabs.erase(source);
    moved->id = target.next_id_++;
    moved->parent = 0;
    moved->tree_collapsed = false;
    moved->workspace = target.private_window ? 0 : target.workspace;
    if (const auto at = target.tab(before); at && at->workspace == moved->workspace)
        moved->group = at->group;
    if (const auto group = profile.group(moved->group);
        !group || group->workspace != moved->workspace)
        moved->group = 0;
    const int new_id = moved->id;
    if (moved->client)
        static_cast<BrowserClient *>(moved->client.get())->rebind(target, new_id);
    if (moved->devtools)
        moved->devtools
            ->discard_callbacks(); // Do not retain one-shot callbacks into the old window.
    for (auto &d : downloads)
        if (d.source_window == window_uid && d.source_tab == id) {
            d.source_window = target.window_uid;
            d.source_tab = new_id;
        }
    if (moved->browser) {
        --created_;
        ++target.created_;
        moved->browser->GetHost()->SetFocus(false);
        moved->browser->GetHost()->NotifyMoveOrResizeStarted();
        moved->browser->GetHost()->SetWindowlessFrameRate(target.window->refresh_rate());
    }
    auto destination = std::find_if(target.tabs.begin(), target.tabs.end(),
                                    [before](const auto &v) { return v->id == before; });
    const int index = int(destination - target.tabs.begin());
    target.tabs.insert(destination, std::move(moved));
    target.bind_tab_callbacks(*target.tabs[index]);
    target.bind_site_tool_events(new_id);
    normalize_tree();
    target.normalize_tree();
    active = 0;
    if (tabs.empty())
        new_tab("aura://newtab", private_window);
    else {
        const auto previous = std::find_if(
            tabs.begin(), tabs.end(), [old_active](const auto &v) { return v->id == old_active; });
        activate(previous == tabs.end() ? std::min(source_index, int(tabs.size()) - 1)
                                        : int(previous - tabs.begin()));
    }
    target.activate(index);
    resize_pages();
    target.resize_pages();
    target.window->activate();
    ++audit["tabs_transferred_out"];
    ++target.audit["tabs_transferred_in"];
    cancel_tab_drag();
    changed(true);
    target.changed(true);
    return true;
}
void BrowserController::cancel_tab_drag() {
    if (state_->tab_drop.source == window_uid || state_->tab_drop.target == window_uid) {
        state_->tab_drop = {};
        changed();
    }
}
void BrowserController::tab_drag(int id, int x, int y, bool release) {
    if (window->backend() == "wayland") {
        if (!release && !state_->tab_drop.source) {
            int before = 0;
            Rect marker;
            if (ui->tab_drop_at(x, y, before, marker))
                return; // Ordinary in-window reorder keeps using the existing gesture.
            state_->tab_drop = {window_uid, 0, id, 0, {}};
            if (!window->begin_tab_drag())
                state_->tab_drop = {};
        }
        return;
    }
    TabDrop next;
    for (auto *target : state_->windows) {
        if (target == this || target->quitting)
            continue;
        const auto point = target->window->point_from(*window, x, y);
        if (!point)
            continue;
        int before = 0;
        Rect marker;
        if (target->ui->tab_drop_at(point->first, point->second, before, marker) &&
            tab_transfer_problem(id, *target).empty()) {
            next = {window_uid, target->window_uid, id, before, marker};
            if (release)
                move_tab_to_window(id, *target, before);
        }
        break; // Never fall through an incompatible/covered front window.
    }
    if (release)
        next = {};
    state_->tab_drop = next;
    changed();
}
bool BrowserController::native_tab_drag(int stage, int x, int y) {
    ++audit[stage == 0   ? "native_drag_motion"
            : stage == 1 ? "native_drag_drop"
            : stage == 2 ? "native_drag_leave"
                         : "native_drag_end"];
    auto &drop = state_->tab_drop;
    auto source = std::find_if(state_->windows.begin(), state_->windows.end(),
                               [&](auto *w) { return w->window_uid == drop.source; });
    if (stage == 3) {
        ui->cancel_tab_gesture();
        drop = {};
        changed();
        return false;
    }
    if (stage == 2) {
        if (drop.target == window_uid) {
            drop.target = 0;
            changed();
        }
        return false;
    }
    int before = 0;
    Rect marker;
    const bool found = source != state_->windows.end();
    const bool hit = ui->tab_drop_at(x, y, before, marker);
    const bool valid =
        found && hit &&
        (*source == this || (*source)->tab_transfer_problem(drop.tab, *this).empty());
    if (!found)
        ++audit["native_drag_missing_source"];
    else if (!hit)
        ++audit["native_drag_outside_tabs"];
    else if (!valid)
        ++audit["native_drag_guarded"];
    drop.target = valid ? window_uid : 0;
    drop.before = before;
    drop.marker = marker;
    changed();
    if (stage == 1 && valid) {
        auto *from = *source;
        if (from == this)
            return true; // Dropping back in the source window cancels the external move.
        const int id = drop.tab;
        return from->move_tab_to_window(id, *this, before);
    }
    return valid;
}
} // namespace aura
