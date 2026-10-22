#include "common/config.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
#include <algorithm>

namespace aura {
bool BrowserController::can_sleep(const Tab &t) const {
    if (quitting || tab_visible(t.id) || t.ntp || t.private_tab || t.pinned || t.loading ||
        t.closing || t.page.closed || !t.page.loaded || t.page.failed || t.sleeping ||
        t.sleep_pending || t.reader_pending || !t.browser || !t.devtools || !t.site_tools_ready ||
        t.media_capture || tools_open || site_exception(t.page.url, profile.tab_sleep_exceptions))
        return false;
    // Extension/background browser pages are never frozen by this controller.
    if (!t.page.url.starts_with("https://") && !t.page.url.starts_with("http://"))
        return false;
    if (std::any_of(t.audio_contexts.begin(), t.audio_contexts.end(),
                    [](const auto &v) { return v.second; }))
        return false;
    if (std::any_of(dialogs.begin(), dialogs.end(), [&](const auto &d) { return d.tab == t.id; }))
        return false;
    return std::none_of(downloads.begin(), downloads.end(), [](const auto &d) {
        return !d.complete && !d.cancelled && !d.interrupted;
    });
}
void BrowserController::wake_tab(int id) {
    auto t = tab(id);
    if (!t || (!t->sleeping && !t->sleep_pending))
        return;
    t->sleep_pending = false;
    t->sleeping = false;
    if (t->devtools)
        t->devtools->call("Page.setWebLifecycleState", "{\"state\":\"active\"}");
    if (t->browser)
        t->browser->GetHost()->Invalidate(PET_VIEW);
    ++audit["tabs_woken"];
    changed();
}
void BrowserController::sleep_tab(int id, bool manual) {
    auto t = tab(id);
    if (!t)
        return;
    if (manual && t == current() && !t->pinned && !t->private_tab && !t->ntp && !t->loading) {
        if (split_enabled())
            end_split();
        const auto visible = visible_tabs();
        auto other =
            std::find_if(visible.begin(), visible.end(), [&](int i) { return tabs[i]->id != id; });
        if (other != visible.end())
            activate(*other);
        else
            new_tab("aura://newtab", false, true, false, workspace, 0);
    }
    if (!can_sleep(*t)) {
        if (manual)
            ui->toast(tr("This tab is protected from sleep."));
        return;
    }
    t->sleep_pending = true;
    t->sleep_retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto serial = t->navigation_serial;
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    t->devtools->call(
        "Page.getFrameTree", "{}",
        [this, id, serial, manual, weak](bool ok, const std::string &json) {
            if (weak.expired())
                return;
            auto t = tab(id);
            if (!t || !t->sleep_pending || t->navigation_serial != serial || !t->devtools)
                return;
            auto value = CefParseJSON(json, JSON_PARSER_RFC);
            auto tree = value && value->GetType() == VTYPE_DICTIONARY
                            ? value->GetDictionary()->GetDictionary("frameTree")
                            : nullptr;
            auto frame = tree ? tree->GetDictionary("frame") : nullptr;
            if (!ok || !frame) {
                t->sleep_pending = false;
                return;
            }
            t->devtools->call(
                "Page.createIsolatedWorld",
                "{\"frameId\":" + json_quote(frame->GetString("id").ToString()) +
                    ",\"worldName\":\"Aura Tab Activity\"}",
                [this, id, serial, manual, weak](bool ok, const std::string &json) {
                    if (weak.expired())
                        return;
                    auto t = tab(id);
                    if (!t || !t->sleep_pending || !t->devtools || t->navigation_serial != serial)
                        return;
                    auto value = CefParseJSON(json, JSON_PARSER_RFC);
                    const int context = ok && value && value->GetType() == VTYPE_DICTIONARY
                                            ? value->GetDictionary()->GetInt("executionContextId")
                                            : 0;
                    if (!context) {
                        t->sleep_pending = false;
                        return;
                    }
                    // Inspect in an isolated world. Inaccessible frames conservatively keep the tab
                    // awake.
                    const std::string expression =
                        R"JS((()=>{function busy(w){try{if(w.document.pictureInPictureElement)return true;
                for(const m of w.document.querySelectorAll('audio,video'))if(!m.paused&&!m.ended)return true;
                for(let i=0;i<w.frames.length;i++)if(busy(w.frames[i]))return true;
                return false;}catch(e){return true;}}return busy(window);})())JS";
                    t->devtools->call(
                        "Runtime.evaluate",
                        "{\"contextId\":" + std::to_string(context) +
                            ",\"returnByValue\":true,\"expression\":" + json_quote(expression) +
                            "}",
                        [this, id, serial, manual, weak](bool ok, const std::string &json) {
                            if (weak.expired())
                                return;
                            auto t = tab(id);
                            if (!t || !t->devtools || !t->sleep_pending)
                                return;
                            t->sleep_pending = false;
                            auto v = CefParseJSON(json, JSON_PARSER_RFC);
                            auto result = v && v->GetType() == VTYPE_DICTIONARY
                                              ? v->GetDictionary()->GetDictionary("result")
                                              : nullptr;
                            if (!ok || !result || result->GetString("type") != "boolean" ||
                                result->GetBool("value") || t->navigation_serial != serial ||
                                !can_sleep(*t)) {
                                if (manual)
                                    ui->toast(tr("This tab is protected from sleep."));
                                changed();
                                return;
                            }
                            t->sleep_pending = true;
                            t->devtools->call(
                                "Page.setWebLifecycleState", "{\"state\":\"frozen\"}",
                                [this, id, serial, manual, weak](bool ok, const std::string &) {
                                    if (weak.expired())
                                        return;
                                    auto t = tab(id);
                                    if (!t || !t->devtools)
                                        return;
                                    const bool requested = t->sleep_pending;
                                    t->sleep_pending = false;
                                    if (ok && requested && t->navigation_serial == serial &&
                                        (manual || profile.tab_sleep_minutes > 0) &&
                                        can_sleep(*t)) {
                                        t->sleeping = true;
                                        t->page.view = PaintBuffer{};
                                        t->page.popup = PaintBuffer{};
                                        t->page.popup_visible = false;
                                        ++audit["tabs_slept"];
                                    } else if (ok)
                                        t->devtools->call("Page.setWebLifecycleState",
                                                          "{\"state\":\"active\"}");
                                    changed();
                                });
                        });
                });
        });
    changed();
}
} // namespace aura
