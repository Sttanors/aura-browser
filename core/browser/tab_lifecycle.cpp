#include "common/config.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
#include <algorithm>

namespace aura {
namespace {
void unload_listener_guard(CefRefPtr<DevToolsSession> session, std::function<void(bool)> done) {
    // Only obtain a Window reference; no page-supplied function is called.
    session->call(
        "Runtime.evaluate", "{\"expression\":\"window\",\"objectGroup\":\"AuraUnloadGuard\"}",
        [session, done](bool ok, const std::string &json) {
            auto v = CefParseJSON(json, JSON_PARSER_RFC);
            auto result = ok && v && v->GetType() == VTYPE_DICTIONARY
                              ? v->GetDictionary()->GetDictionary("result")
                              : nullptr;
            const auto object = result ? result->GetString("objectId").ToString() : "";
            if (object.empty()) {
                done(false);
                return;
            }
            session->call("DOMDebugger.getEventListeners",
                          "{\"objectId\":" + json_quote(object) + ",\"depth\":1,\"pierce\":true}",
                          [session, done](bool ok, const std::string &json) {
                              auto v = CefParseJSON(json, JSON_PARSER_RFC);
                              auto listeners = ok && v && v->GetType() == VTYPE_DICTIONARY
                                                   ? v->GetDictionary()->GetList("listeners")
                                                   : nullptr;
                              bool safe = listeners && listeners->GetSize() <= 10000;
                              if (safe)
                                  for (size_t i = 0; i < listeners->GetSize(); ++i) {
                                      auto entry = listeners->GetDictionary(i);
                                      if (!entry || entry->GetString("type") == "beforeunload") {
                                          safe = false;
                                          break;
                                      }
                                  }
                              session->call("Runtime.releaseObjectGroup",
                                            "{\"objectGroup\":\"AuraUnloadGuard\"}");
                              done(safe);
                          });
        });
}
} // namespace

bool BrowserController::can_sleep(const Tab &t, bool allow_visible) const {
    if (safe_mode() || quitting || (!allow_visible && tab_visible(t.id)) || t.ntp ||
        t.private_tab || t.pinned || t.loading || t.closing || t.page.closed || !t.page.loaded ||
        t.page.failed || t.discarded || t.sleeping || t.sleep_pending || t.reader_pending ||
        t.reading_pending || !t.browser || !t.devtools || !t.site_tools_ready || t.media_capture ||
        (screenshot.native_running && screenshot.job_tab == t.id) || tools_open ||
        site_exception(t.page.url, profile.tab_sleep_exceptions))
        return false;
    // Extension/background browser pages are never frozen by this controller.
    if (!t.page.url.starts_with("https://") && !t.page.url.starts_with("http://"))
        return false;
    if (std::any_of(t.audio_contexts.begin(), t.audio_contexts.end(),
                    [](const auto &v) { return v.second; }))
        return false;
    if (std::any_of(dialogs.begin(), dialogs.end(), [&](const auto &d) { return d.tab == t.id; }))
        return false;
    return std::none_of(downloads.begin(), downloads.end(), [&](const auto &d) {
        // Unknown sources remain protected until OnBeforeDownload identifies their tab.
        return d.source_window == window_uid && (!d.source_tab || d.source_tab == t.id) &&
               !d.complete && !d.cancelled && !d.interrupted;
    });
}
void BrowserController::wake_tab(int id) {
    auto t = tab(id);
    if (!t)
        return;
    if (t->discard_pending && !t->discard_closing)
        t->discard_pending = false;
    if (t->discarded && !t->closing && !quitting) {
        const auto url = t->page.url;
        t->discarded = false;
        t->last_navigation_get = false;
        ensure_browser(*t, url);
        ++audit["tabs_restored"];
        changed();
    }
    if (!t->sleeping && !t->sleep_pending)
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
    if (t->discard_pending || t->discarded || !can_sleep(*t, manual && t == current())) {
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
                                !can_sleep(*t, manual && t == current())) {
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
                                        can_sleep(*t, manual && t == current())) {
                                        // Keep focus and split layout unchanged until freeze
                                        // succeeds.
                                        if (manual && t == current()) {
                                            if (split_enabled())
                                                end_split();
                                            const auto visible = visible_tabs();
                                            auto other = std::find_if(
                                                visible.begin(), visible.end(),
                                                [&](int i) { return tabs[i]->id != id; });
                                            if (other != visible.end())
                                                activate(*other);
                                            else
                                                new_tab("aura://newtab", false, true, false,
                                                        workspace, 0);
                                        }
                                        t->sleeping = true;
                                        t->page.view = PaintBuffer{};
                                        t->page.popup = PaintBuffer{};
                                        t->page.popup_visible = false;
                                        ++audit["tabs_slept"];
                                    } else {
                                        if (ok)
                                            t->devtools->call("Page.setWebLifecycleState",
                                                              "{\"state\":\"active\"}");
                                        if (manual && requested)
                                            ui->toast(tr("This tab is protected from sleep."));
                                    }
                                    changed();
                                });
                        });
                });
        });
    changed();
}
void BrowserController::discard_inactive_tabs() {
    confirm_action(tr("Unload inactive tabs?"),
                   tr("Pages will reload when selected. Their back and forward history will be "
                      "reset. Forms, media and downloads are protected."),
                   [this] {
                       for (auto &t : tabs)
                           if (can_sleep(*t))
                               discard_tab(t->id, false);
                   });
}
void BrowserController::discard_tab(int id, bool manual) {
    auto t = tab(id);
    if (!t || t->discard_pending || t->reader_active || !t->last_navigation_get || !can_sleep(*t)) {
        if (manual)
            ui->toast(tr("This tab is protected from unloading."));
        return;
    }
    const auto source = read_resource(resources / "protection/tab-discard.js");
    if (source.empty())
        return;
    t->discard_pending = true;
    t->discard_manual = manual;
    t->discard_started = std::chrono::steady_clock::now();
    t->sleep_retry_after = t->discard_started + std::chrono::seconds(30);
    const auto started = t->discard_started;
    const auto serial = t->navigation_serial;
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    const auto pending = [this, id, started, serial, weak]() -> Tab * {
        if (weak.expired())
            return nullptr;
        auto t = tab(id);
        return t && t->discard_pending && t->discard_started == started &&
                       t->navigation_serial == serial && t->devtools
                   ? t
                   : nullptr;
    };
    const auto refuse = [this, pending, manual] {
        if (auto t = pending()) {
            t->discard_pending = false;
            if (manual)
                ui->toast(tr("This tab is protected from unloading."));
            changed();
        }
    };
    t->devtools->call(
        "Page.getFrameTree", "{}",
        [this, pending, refuse, source](bool ok, const std::string &json) {
            auto t = pending();
            if (!t)
                return;
            auto v = CefParseJSON(json, JSON_PARSER_RFC);
            auto tree = ok && v && v->GetType() == VTYPE_DICTIONARY
                            ? v->GetDictionary()->GetDictionary("frameTree")
                            : nullptr;
            auto frame = tree ? tree->GetDictionary("frame") : nullptr;
            // Every child frame stays protected, including inaccessible/OOPIF documents.
            auto children = tree ? tree->GetList("childFrames") : nullptr;
            if (!frame || (children && children->GetSize())) {
                refuse();
                return;
            }
            t->devtools->call(
                "Page.createIsolatedWorld",
                "{\"frameId\":" + json_quote(frame->GetString("id").ToString()) +
                    ",\"worldName\":\"Aura Unload Guard\"}",
                [this, pending, refuse, source](bool ok, const std::string &json) {
                    auto t = pending();
                    if (!t)
                        return;
                    auto v = CefParseJSON(json, JSON_PARSER_RFC);
                    const int context = ok && v && v->GetType() == VTYPE_DICTIONARY
                                            ? v->GetDictionary()->GetInt("executionContextId")
                                            : 0;
                    if (!context) {
                        refuse();
                        return;
                    }
                    t->devtools->call(
                        "Runtime.evaluate",
                        "{\"contextId\":" + std::to_string(context) +
                            ",\"returnByValue\":true,"
                            "\"expression\":" +
                            json_quote(source) + "}",
                        [this, pending, refuse](bool ok, const std::string &json) {
                            auto t = pending();
                            if (!t)
                                return;
                            auto v = CefParseJSON(json, JSON_PARSER_RFC);
                            auto result = ok && v && v->GetType() == VTYPE_DICTIONARY
                                              ? v->GetDictionary()->GetDictionary("result")
                                              : nullptr;
                            if (!result || result->GetString("type") != "boolean" ||
                                result->GetBool("value") || t->reader_active ||
                                !t->last_navigation_get || !can_sleep(*t)) {
                                refuse();
                                return;
                            }
                            unload_listener_guard(t->devtools, [this, pending, refuse](bool safe) {
                                if (!pending())
                                    return;
                                if (!safe) {
                                    refuse();
                                    return;
                                }
                                // Leave the DevTools observer before closing its session.
                                defer([this, pending, refuse] {
                                    auto t = pending();
                                    if (!t)
                                        return;
                                    if (!can_sleep(*t)) {
                                        refuse();
                                        return;
                                    }
                                    t->discard_closing = true;
                                    t->browser->GetHost()->CloseBrowser(false);
                                });
                            });
                        });
                });
        });
    changed();
}
} // namespace aura
