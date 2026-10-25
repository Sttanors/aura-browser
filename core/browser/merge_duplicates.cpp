#include "core/browser/browser.hpp"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
namespace aura {
namespace {
BrowserController *owner(const BrowserState &state, DuplicateRef ref) {
    for (auto *w : state.windows)
        if (w->window_uid == ref.window)
            return w;
    return nullptr;
}
std::optional<DuplicateTab> snapshot(const BrowserController &app, DuplicateRef ref) {
    for (const auto &t : app.duplicate_tabs())
        if (t.ref == ref)
            return t;
    return {};
}
CefRefPtr<CefDictionaryValue> dictionary(const std::string &json) {
    auto value = CefParseJSON(json, JSON_PARSER_RFC);
    return value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
}
} // namespace
std::vector<DuplicateTab> BrowserController::duplicate_tabs(bool include_closing) const {
    std::vector<DuplicateTab> result;
    for (auto *w : state_->windows) {
        if (w->quitting)
            continue;
        for (const auto &t : w->tabs) {
            if (t->ntp || t->page.closed || (!include_closing && t->closing))
                continue;
            if (private_window ? (w != this || !t->private_tab) : (t->private_tab && w != this))
                continue;
            unsigned protection = t->pinned ? DuplicatePinned : 0;
            if (t->loading || !t->page.loaded || t->page.failed || !t->browser || !t->devtools ||
                !t->site_tools_ready || t->creation_pending || t->popup_pending ||
                t->sleep_pending || t->reader_pending || t->reading_pending ||
                (w->screenshot.native_running && w->screenshot.job_tab == t->id))
                protection |= DuplicateBusy;
            if (t->media_capture || std::any_of(t->audio_contexts.begin(), t->audio_contexts.end(),
                                                [](const auto &a) { return a.second; }))
                protection |= DuplicateMedia;
            if (!w->dialogs.empty())
                protection |= DuplicateDialog;
            if (std::any_of(downloads.begin(), downloads.end(), [&](const auto &d) {
                    return d.source_window == w->window_uid &&
                           (!d.source_tab || d.source_tab == t->id) && !d.complete &&
                           !d.cancelled && !d.interrupted;
                }))
                protection |= DuplicateDownload;
            result.push_back({{w->window_uid, t->id},
                              t->navigation_serial,
                              t->page.url,
                              t->page.title,
                              t->container,
                              t->workspace,
                              t->group,
                              t->private_tab,
                              w == this && t.get() == w->current(),
                              protection});
        }
    }
    return result;
}
void BrowserController::show_duplicate_tab(DuplicateRef ref) {
    const auto found = snapshot(*this, ref);
    if (!found)
        return;
    auto w = owner(*state_, ref);
    if (!w || !w->window)
        return;
    for (size_t i = 0; i < w->tabs.size(); ++i)
        if (w->tabs[i]->id == ref.tab && !w->tabs[i]->closing) {
            w->ui->panel(BrowserUi::None);
            w->activate(int(i));
            w->window->activate();
            return;
        }
}
void BrowserController::stop_duplicate_merge(const std::string &reason) {
    auto &m = state_->duplicate_merge;
    if (!m.active)
        return;
    m.skipped += m.pending.size() + (m.checking ? 1 : 0);
    if (m.waiting && reason == "Closing was cancelled. Remaining duplicates were kept.")
        ++m.skipped;
    m.active = false;
    ++m.generation;
    m.pending.clear();
    m.keep = {};
    m.checking.reset();
    m.waiting.reset();
    m.status = reason;
    changed();
}
void BrowserController::merge_duplicates(const DuplicateGroup &selected, DuplicateRef keep) {
    if (quitting || state_->duplicate_merge.active)
        return;
    const auto groups = group_duplicate_tabs(duplicate_tabs());
    const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto &g) {
        return same_duplicate_group(g, selected);
    });
    const auto keeper = snapshot(*this, keep);
    if (found == groups.end() || !keeper || (keeper->protection & DuplicateBusy)) {
        ui->toast(tr("The duplicate list changed. Review it and try again."));
        return;
    }
    auto plan = duplicate_close_plan(selected, keep);
    if (plan.empty())
        return;
    BrowserDialog dialog;
    dialog.owned = true;
    dialog.origin = tr("Close duplicate tabs");
    dialog.accept_label = tr("Close duplicates");
    auto title = keeper->title;
    for (auto &c : title)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
            c = ' ';
    size_t end = std::min<size_t>(120, title.size());
    while (end < title.size() && end && (static_cast<unsigned char>(title[end]) & 0xc0) == 0x80)
        --end;
    title.resize(end);
    dialog.message = tr("Keep this tab:") + " " + title + "\n" + tr("Tabs to close:") + " " +
                     std::to_string(plan.size()) + "\n" +
                     tr("Protected tabs stay open. Unsaved changes may be lost; page leave prompts "
                        "are preserved.");
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    dialog.action = [this, weak, selected, keep](bool accept, const std::string &) {
        if (weak.expired() || !accept || quitting || state_->duplicate_merge.active)
            return;
        const auto groups = group_duplicate_tabs(duplicate_tabs());
        if (std::none_of(groups.begin(), groups.end(),
                         [&](const auto &g) { return same_duplicate_group(g, selected); })) {
            ui->toast(tr("The duplicate list changed. Review it and try again."));
            return;
        }
        const auto keeper = snapshot(*this, keep);
        if (!keeper)
            return;
        auto plan = duplicate_close_plan(selected, keep);
        auto &m = state_->duplicate_merge;
        ++m.generation;
        m.active = true;
        m.requester = window_uid;
        m.keep = *keeper;
        m.pending = std::deque<DuplicateTab>(plan.begin(), plan.end());
        m.waiting.reset();
        m.checking.reset();
        m.closed = 0;
        m.skipped = selected.tabs.size() - plan.size() - 1;
        m.status = "Closing duplicates…";
        changed();
    };
    dialogs.push_back(std::move(dialog));
    changed();
}
void BrowserController::tick_duplicate_merge() {
    auto &m = state_->duplicate_merge;
    if (!m.active || m.requester != window_uid)
        return;
    if (quitting || state_->exiting) {
        stop_duplicate_merge();
        return;
    }
    const auto keep = snapshot(*this, m.keep.ref);
    if (!keep || !same_duplicate_tab(m.keep, *keep)) {
        stop_duplicate_merge("The kept tab changed. Remaining duplicates were kept.");
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m.waiting) {
        auto w = owner(*state_, m.waiting->ref);
        auto t = w ? w->tab(m.waiting->ref.tab) : nullptr;
        if (!t || t->page.closed) {
            ++m.closed;
            m.waiting.reset();
            changed();
        } else if (!t->closing) {
            stop_duplicate_merge("Closing was cancelled. Remaining duplicates were kept.");
            return;
        } else if (now - m.started > std::chrono::seconds(30)) {
            stop_duplicate_merge("A tab is still closing. Remaining duplicates were kept.");
            return;
        } else
            return;
    }
    if (m.checking) {
        if (now - m.started > std::chrono::seconds(15)) {
            ++m.skipped;
            m.checking.reset();
            changed();
        } else
            return;
    }
    if (m.pending.empty()) {
        stop_duplicate_merge(
            m.skipped ? "Duplicate cleanup finished. Protected or unverified tabs were kept."
                      : "Duplicate cleanup finished.");
        return;
    }
    const auto target = m.pending.front();
    m.pending.pop_front();
    const auto current = snapshot(*this, target.ref);
    if (!current || !same_duplicate_tab(target, *current) || current->protection) {
        ++m.skipped;
        changed();
        return;
    }
    m.checking = target;
    m.started = now;
    probe_duplicate_media(target, m.generation);
    changed();
}
void BrowserController::probe_duplicate_media(DuplicateTab target, uint64_t generation) {
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    auto valid = [this, weak, target, generation]() -> Tab * {
        if (weak.expired())
            return nullptr;
        const auto &m = state_->duplicate_merge;
        if (!m.active || m.generation != generation || !m.checking || m.checking->ref != target.ref)
            return nullptr;
        const auto current = snapshot(*this, target.ref);
        auto w = owner(*state_, target.ref);
        auto t = w ? w->tab(target.ref.tab) : nullptr;
        return current && same_duplicate_tab(target, *current) && !current->protection && t &&
                       t->devtools
                   ? t
                   : nullptr;
    };
    auto complete = [this, weak, target, generation](bool safe) {
        if (weak.expired())
            return;
        auto &m = state_->duplicate_merge;
        if (!m.active || m.generation != generation || !m.checking || m.checking->ref != target.ref)
            return;
        const auto keep = snapshot(*this, m.keep.ref);
        if (!keep || !same_duplicate_tab(m.keep, *keep)) {
            stop_duplicate_merge("The kept tab changed. Remaining duplicates were kept.");
            return;
        }
        const auto current = snapshot(*this, target.ref);
        m.checking.reset();
        if (!safe || !current || !same_duplicate_tab(target, *current) || current->protection) {
            ++m.skipped;
            changed();
            return;
        }
        auto w = owner(*state_, target.ref);
        if (!w) {
            ++m.skipped;
            changed();
            return;
        }
        for (size_t i = 0; i < w->tabs.size(); ++i)
            if (w->tabs[i]->id == target.ref.tab) {
                m.waiting = target;
                m.started = std::chrono::steady_clock::now();
                w->close_tab(int(i), true);
                changed();
                return;
            }
        ++m.skipped;
        changed();
    };
    auto t = valid();
    if (!t) {
        complete(false);
        return;
    }
    t->devtools->call("Page.getFrameTree", "{}", [valid, complete](bool ok, const std::string &json) {
        auto t = valid();
        if (!t) {
            complete(false);
            return;
        }
        auto d = dictionary(json);
        auto tree = ok && d ? d->GetDictionary("frameTree") : nullptr;
        auto frame = tree ? tree->GetDictionary("frame") : nullptr;
        if (!frame) {
            complete(false);
            return;
        }
        t->devtools->call(
            "Page.createIsolatedWorld",
            "{\"frameId\":" + json_quote(frame->GetString("id").ToString()) +
                ",\"worldName\":\"Aura Duplicate Activity\"}",
            [valid, complete](bool ok, const std::string &json) {
                auto t = valid();
                if (!t) {
                    complete(false);
                    return;
                }
                auto d = dictionary(json);
                int context = ok && d ? d->GetInt("executionContextId") : 0;
                if (!context) {
                    complete(false);
                    return;
                }
                const std::string js =
                    R"JS((()=>{function busy(w){try{if(w.document.pictureInPictureElement)return true;
for(const m of w.document.querySelectorAll('audio,video'))if(!m.paused&&!m.ended)return true;
for(let i=0;i<w.frames.length;i++)if(busy(w.frames[i]))return true;
return false;}catch(e){return true;}}return busy(window);})())JS";
                t->devtools->call("Runtime.evaluate",
                                  "{\"contextId\":" + std::to_string(context) +
                                      ",\"returnByValue\":true,\"expression\":" + json_quote(js) +
                                      "}",
                                  [valid, complete](bool ok, const std::string &json) {
                                      if (!valid()) {
                                          complete(false);
                                          return;
                                      }
                                      auto d = dictionary(json);
                                      auto result = ok && d && !d->HasKey("exceptionDetails")
                                                        ? d->GetDictionary("result")
                                                        : nullptr;
                                      complete(result && result->GetType("value") == VTYPE_BOOL &&
                                               !result->GetBool("value"));
                                  });
            });
    });
}
} // namespace aura
