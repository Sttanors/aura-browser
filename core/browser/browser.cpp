#include "common/platform.hpp"
#include "core/browser/browser.hpp"
#include "aura/design_tokens.hpp"
#include "common/config.hpp"
#include "core/browser/client.hpp"
#include "include/cef_cookie.h"
#include "include/cef_frame.h"
#include "include/cef_image.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#else
#include <windows.h>
#include <shellapi.h>
#endif
#include <spdlog/spdlog.h>
namespace aura {
namespace {
std::string tabs_json(const std::vector<std::unique_ptr<Tab>> &tabs) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (auto &t : tabs) {
        if (t->closing)
            continue;
        if (!first)
            out << ',';
        first = false;
        out << "{\"url\":\"" << json_escape(t->page.url) << "\",\"title\":\""
            << json_escape(t->page.title)
            << "\",\"private\":" << (t->private_tab ? "true" : "false")
            << ",\"loaded\":" << (t->page.loaded ? "true" : "false") << ",\"frame_rate\":"
            << (t->browser ? t->browser->GetHost()->GetWindowlessFrameRate() : 0)
            << ",\"frames\":" << t->page.view.generation() << ",\"zoom_percent\":"
            << std::round(
                   std::pow(1.2, t->browser ? t->browser->GetHost()->GetZoomLevel() : t->zoom) *
                   100)
            << '}';
    }
    out << ']';
    return out.str();
}
class Completion final : public CefCompletionCallback {
  public:
    explicit Completion(std::function<void()> action) : action_(std::move(action)) {}
    void OnComplete() override {
        action_();
    }

  private:
    std::function<void()> action_;
    IMPLEMENT_REFCOUNTING(Completion);
};
class CookiesDeleted final : public CefDeleteCookiesCallback {
  public:
    explicit CookiesDeleted(std::function<void()> action) : action_(std::move(action)) {}
    void OnComplete(int) override {
        action_();
    }

  private:
    std::function<void()> action_;
    IMPLEMENT_REFCOUNTING(CookiesDeleted);
};
class ImageCallback final : public CefDownloadImageCallback {
  public:
    ImageCallback(BrowserController &owner, int id) : owner_(owner), id_(id) {}
    void OnDownloadImageFinished(const CefString &, int, CefRefPtr<CefImage> image) override {
        if (!image)
            return;
        auto t = owner_.tab(id_);
        if (!t || t->closing)
            return;
        int w = 0, h = 0;
        auto bitmap =
            image->GetAsBitmap(1, CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED, w, h);
        if (!bitmap || w <= 0 || h <= 0 || w > 256 || h > 256)
            return;
        std::vector<std::uint8_t> bytes(bitmap->GetSize());
        bitmap->GetData(bytes.data(), bytes.size(), 0);
        try {
            t->favicon.update(w, h, bytes, {});
            owner_.changed();
        } catch (const std::exception &) {
        }
    }

  private:
    BrowserController &owner_;
    int id_;
    IMPLEMENT_REFCOUNTING(ImageCallback);
};
class ToolsClient final : public CefClient, public CefLifeSpanHandler {
  public:
    explicit ToolsClient(BrowserController &owner) : owner_(owner) {}
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }
    void OnAfterCreated(CefRefPtr<CefBrowser>) override {
        ++owner_.tools_open;
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        --owner_.tools_open;
    }

  private:
    BrowserController &owner_;
    IMPLEMENT_REFCOUNTING(ToolsClient);
};
} // namespace
BrowserController::BrowserController(std::filesystem::path path, std::filesystem::path font,
                                     double factor, bool accelerated)
    : profile(std::move(path)), scale(factor), extensions(profile.directory) {
    resources = font.parent_path().parent_path();
    request_policy->protection = std::make_shared<Protection>(resources);
    ui = std::make_unique<BrowserUi>(*this, font);
    WindowCallbacks hooks;
    hooks.draw = [this](SkCanvas &canvas) { ui->draw(canvas); };
    hooks.draw_content = [this](SkCanvas &canvas) { return ui->draw_web_frame(canvas); };
    hooks.resize = [this](int w, int h, double factor, double dpi) {
        scale = dpi;
        ui_scale = factor;
        width = w;
        height = h;
        ui->resize(w, h);
        resize_extension_popup();
        auto r = ui->web_bounds();
        for (auto &t : tabs) {
            t->page.scale = scale;
            t->page.view_width = std::max(1, int(std::lround(r.width * ui_scale)));
            t->page.view_height = std::max(1, int(std::lround(r.height * ui_scale)));
            if (t->browser) {
                t->browser->GetHost()->WasResized();
                t->browser->GetHost()->NotifyScreenInfoChanged();
            }
        }
    };
    hooks.pointer = [this](const PointerInput &p) { ui->pointer(p); };
    hooks.key = [this](const CefKeyEvent &key) { return ui->key(key); };
    hooks.text = [this](const std::string &text) { ui->text(text); };
    hooks.focus = [this](bool focus) {
        if (!focus)
            close_extension_popup(false);
        if (auto p = extension_popup; p && p->browser)
            p->browser->GetHost()->SetFocus(focus);
        else if (auto t = current(); t && t->browser)
            t->browser->GetHost()->SetFocus(focus && !ui->editing);
    };
    hooks.close = [this] { request_quit(); };
    window = std::make_unique<BrowserWindow>(width, height, scale, std::move(hooks), accelerated,
                                             profile.ui_zoom / 100.0);
}
BrowserController::~BrowserController() = default;
std::string BrowserController::tr(std::string_view english) const {
    return translate(profile.language, english);
}
void BrowserController::configure_permissions(CefRefPtr<CefRequestContext> context,
                                              const std::string &url, const std::string &top) {
    // Values and supported scopes follow Chromium's ContentSettingsRegistry.
    // Private contexts are configured per origin; CEF ignores their empty default scope.
    const struct {
        cef_content_setting_types_t type;
        const std::string *value;
    } rules[] = {{CEF_CONTENT_SETTING_TYPE_NOTIFICATIONS, &profile.notifications},
                 {CEF_CONTENT_SETTING_TYPE_GEOLOCATION, &profile.geolocation},
                 {CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA, &profile.camera},
                 {CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC, &profile.microphone},
                 {CEF_CONTENT_SETTING_TYPE_CLIPBOARD_READ_WRITE, &profile.clipboard},
                 {CEF_CONTENT_SETTING_TYPE_MIDI_SYSEX, &profile.midi},
                 {CEF_CONTENT_SETTING_TYPE_SENSORS, &profile.sensors}};
    for (const auto &rule : rules)
        context->SetContentSetting(url, top, rule.type,
                                   *rule.value == "block"   ? CEF_CONTENT_SETTING_VALUE_BLOCK
                                   : *rule.value == "allow" ? CEF_CONTENT_SETTING_VALUE_ALLOW
                                                            : CEF_CONTENT_SETTING_VALUE_ASK);
    context->SetContentSetting(url, top, CEF_CONTENT_SETTING_TYPE_POPUPS,
                               profile.block_popups ? CEF_CONTENT_SETTING_VALUE_BLOCK
                                                    : CEF_CONTENT_SETTING_VALUE_ALLOW);
}
void BrowserController::apply_settings(bool update_zoom) {
    window->interface_zoom(profile.ui_zoom / 100.0);
    close_menu();
    request_policy->protection->configure(profile.adblock, profile.cosmetic_filtering,
                                          profile.adblock_exceptions);
    request_policy->do_not_track = profile.do_not_track;
    request_policy->send_referrer = profile.send_referrer;
    profile.prune_history();
    configure_permissions(CefRequestContext::GetGlobalContext());
    configure_font_preferences(CefRequestContext::GetGlobalContext());
    for (auto &t : tabs) {
        update_site_tools(*t);
        if (t->private_tab && t->browser)
            configure_font_preferences(t->browser->GetHost()->GetRequestContext());
        if (t->browser &&
            (t->page.url.starts_with("http://") || t->page.url.starts_with("https://")))
            configure_permissions(t->browser->GetHost()->GetRequestContext(), t->page.url,
                                  t->page.url);
        if (t->ntp)
            t->page.title = tr(t->private_tab ? "Private tab" : "New tab");
        if (update_zoom) {
            t->zoom = std::log(profile.default_zoom / 100.0) / std::log(1.2);
            if (t->browser)
                t->browser->GetHost()->SetZoomLevel(t->zoom);
        }
    }
    if (current())
        window->title(current()->page.title + " — Aura");
    changed(true);
}
Tab *BrowserController::current() {
    return active >= 0 && active < static_cast<int>(tabs.size()) ? tabs[active].get() : nullptr;
}
Tab *BrowserController::tab(int id) {
    for (auto &t : tabs)
        if (t->id == id)
            return t.get();
    return nullptr;
}
void BrowserController::open_new_tab(bool private_tab) {
    new_tab(private_tab || profile.new_tab_page == "newtab" ? "aura://newtab"
            : profile.new_tab_page == "home"                ? profile.home_page
                                                            : "about:blank",
            private_tab);
}
void BrowserController::start(const std::string &initial, bool private_mode) {
    apply_settings();
    if (private_mode)
        new_tab(initial.empty() ? "aura://newtab" : initial, true);
    else if (!initial.empty())
        new_tab(initial);
    else if (profile.restore && !profile.session.empty()) {
        auto saved = profile.session;
        int selected = profile.active;
        restoring_ = true;
        for (auto &t : saved)
            new_tab(t.url, false, false, t.pinned);
        restoring_ = false;
        activate(std::min(selected, static_cast<int>(tabs.size()) - 1));
    } else
        new_tab(profile.startup_page == "home" ? profile.home_page : "aura://newtab");
}
void BrowserController::changed(bool persistent) {
    if (window)
        window->invalidate();
    if (persistent) {
        needs_save_ = true;
        save_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    }
}
void BrowserController::defer(std::function<void()> action) {
    pending_.push_back(std::move(action));
}
void BrowserController::tick() {
    const int rate = window->refresh_rate();
    if (rate != browser_frame_rate_) {
        browser_frame_rate_ = rate;
        for (auto &t : tabs)
            if (t->browser)
                t->browser->GetHost()->SetWindowlessFrameRate(rate);
        if (extension_popup && extension_popup->browser)
            extension_popup->browser->GetHost()->SetWindowlessFrameRate(rate);
    }
    std::erase_if(closing_popups_, [](const auto &p) { return p->page.closed; });
    const bool extension_busy = extensions.busy;
    extensions.tick();
    if (extension_busy != extensions.busy)
        changed();
    auto work = std::move(pending_);
    pending_.clear();
    for (auto &action : work)
        action();
    const int old_active_id = current() ? current()->id : 0;
    const bool active_closed = current() && current()->page.closed;
    for (int i = static_cast<int>(tabs.size()) - 1; i >= 0; --i)
        if (tabs[i]->page.closed) {
            tabs.erase(tabs.begin() + i);
            if (i < active)
                --active;
            else if (i == active)
                active = std::min(active, static_cast<int>(tabs.size()) - 1);
        }
    if (active_closed && !tabs.empty()) {
        if (profile.close_selection == "first")
            active = 0;
        else if (profile.close_selection == "last")
            active = tabs.size() - 1;
    }
    if (!quitting && tabs.empty())
        open_new_tab();
    else if (!quitting && current() && current()->id != old_active_id)
        activate(active);
    if (ui->tick())
        changed();
    if (needs_save_ && !quitting && std::chrono::steady_clock::now() >= save_at_) {
        try {
            save_session();
        } catch (const std::exception &error) {
            ui->toast(tr(error.what()));
        }
        needs_save_ = false;
    }
}
void BrowserController::ensure_browser(Tab &t, const std::string &url) {
    auto r = ui->web_bounds();
    t.page.view_width = std::max(1, int(std::lround(r.width * ui_scale)));
    t.page.view_height = std::max(1, int(std::lround(r.height * ui_scale)));
    t.page.scale = scale;
    t.page.url = url;
    t.page.title = url_host(url);
    t.ntp = false;
    t.page.failed = false;
    t.page.loaded = false;
    t.loading = true;
    t.progress = .04;
    if (t.browser) {
        t.browser->GetHost()->WasHidden(&t != current());
        t.browser->GetMainFrame()->LoadURL(url);
        return;
    }
    t.page.invalidate = [this, id = t.id] {
        if (current() && current()->id == id)
            changed();
    };
    t.page.repaint = [this, id = t.id] {
        if (auto t = current(); t && t->id == id && !t->ntp)
            window->invalidate(ui->web_bounds());
    };
    t.page.ime_caret = [this, id = t.id](const CefRect &caret) {
        if (current() && current()->id == id) {
            auto r = ui->web_bounds();
            window->caret(r.x + int(caret.x / ui_scale),
                          r.y + int((caret.y + caret.height) / ui_scale));
        }
    };
    t.client = new BrowserClient(*this, t);
    CefWindowInfo info;
    info.SetAsWindowless(window->handle());
    CefBrowserSettings settings;
    settings.windowless_frame_rate = window->refresh_rate();
    settings.background_color = 0xFFFFFFFF;
    settings.default_font_size = profile.default_font_size;
    settings.default_fixed_font_size = profile.default_font_size;
    settings.minimum_font_size = profile.minimum_font_size;
    settings.minimum_logical_font_size = profile.minimum_font_size;
    CefString(&settings.standard_font_family) = profile.font_family == "serif"  ? "serif"
                                                : profile.font_family == "mono" ? "monospace"
                                                                                : "sans-serif";
    settings.javascript = profile.javascript ? STATE_ENABLED : STATE_DISABLED;
    settings.image_loading = profile.images ? STATE_ENABLED : STATE_DISABLED;
    settings.remote_fonts = profile.remote_fonts ? STATE_ENABLED : STATE_DISABLED;
    settings.local_storage = profile.local_storage ? STATE_ENABLED : STATE_DISABLED;
    settings.text_area_resize = profile.resize_textareas ? STATE_ENABLED : STATE_DISABLED;
    settings.tab_to_links = profile.tab_to_links ? STATE_ENABLED : STATE_DISABLED;
    settings.image_shrink_standalone_to_fit = profile.image_fit ? STATE_ENABLED : STATE_DISABLED;
    CefRefPtr<CefRequestContext> context;
    if (t.private_tab) {
        if (!private_context_) {
            CefRequestContextSettings settings;
            CefString(&settings.accept_language_list) = profile.site_language;
            private_context_ = CefRequestContext::CreateContext(settings, nullptr);
        }
        context = private_context_;
    }
    t.browser = CefBrowserHost::CreateBrowserSync(info, t.client, "about:blank", settings, nullptr,
                                                  context);
    if (!t.browser) {
        t.client = nullptr;
        t.page.failed = true;
        t.page.error = tr("Chromium could not create this tab");
        ui->toast(t.page.error);
    } else {
        t.browser->GetHost()->SetZoomLevel(t.zoom);
        t.browser->GetHost()->SetAudioMuted(t.muted);
        t.browser->GetHost()->WasHidden(&t != current());
        t.browser->GetHost()->SetFocus(&t == current() && !ui->editing);
    }
}
void BrowserController::new_tab(std::string input, bool private_tab, bool foreground, bool pinned) {
    if (quitting)
        return;
    std::string url;
    try {
        url = navigation_url(std::move(input), profile.search_engine);
    } catch (const std::exception &e) {
        ui->toast(tr(e.what()));
        return;
    }
    auto t = std::make_unique<Tab>();
    t->id = next_id_++;
    t->page.scale = scale;
    t->private_tab = private_tab;
    t->pinned = pinned;
    t->muted = profile.mute_new_tabs;
    t->page.url = url;
    t->page.title = tr(private_tab ? "Private tab" : "New tab");
    t->zoom = std::log(profile.default_zoom / 100.0) / std::log(1.2);
    int index = !restoring_ && !profile.tabs_at_end && current() ? active + 1
                                                                 : static_cast<int>(tabs.size());
    tabs.insert(tabs.begin() + index, std::move(t));
    if (index <= active)
        ++active;
    if (foreground || active < 0)
        activate(index);
    if (url != "aura://newtab")
        ensure_browser(*tabs[index], url);
    ++audit[private_tab ? "private_tabs" : "new_tabs"];
    changed(true);
}
void BrowserController::activate(int index) {
    close_extension_popup(false);
    if (index < 0 || index >= static_cast<int>(tabs.size()) || tabs[index]->closing)
        return;
    active = index;
    ui->editing = false;
    ui->find_open = false;
    ui->status.clear();
    close_menu();
    for (auto &t : tabs)
        if (t->browser) {
            t->browser->GetHost()->WasHidden(t.get() != current() || t->ntp);
            t->browser->GetHost()->SetFocus(t.get() == current());
        }
    window->title(current()->page.title + " — Aura");
    ++audit["tab_switches"];
    changed(true);
}
void BrowserController::close_tab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs.size()))
        return;
    auto &t = *tabs[index];
    if (profile.quit_last_tab && !quitting &&
        std::count_if(tabs.begin(), tabs.end(), [](auto &t) { return !t->closing; }) == 1) {
        request_quit();
        return;
    }
    cancel_permissions(t.id);
    if (t.closing)
        return;
    if (!t.private_tab) {
        recently_closed_.push_back({t.page.url, t.pinned});
        if (recently_closed_.size() > 30)
            recently_closed_.erase(recently_closed_.begin());
    }
    close_menu();
    t.closing = true;
    if (t.browser) {
        t.browser->GetHost()->CloseDevTools();
        t.browser->GetHost()->CloseBrowser(false);
    } else
        t.page.closed = true;
    ++audit["tabs_closed_by_user"];
    changed(true);
}
void BrowserController::reopen() {
    if (recently_closed_.empty())
        return;
    auto t = recently_closed_.back();
    recently_closed_.pop_back();
    new_tab(t.url, false, true, t.pinned);
    ++audit["tabs_reopened"];
}
void BrowserController::move_tab(int from, int to) {
    if (from < 0 || to < 0 || from >= static_cast<int>(tabs.size()) ||
        to >= static_cast<int>(tabs.size()) || from == to)
        return;
    const auto id = current() ? current()->id : 0;
    auto item = std::move(tabs[from]);
    tabs.erase(tabs.begin() + from);
    tabs.insert(tabs.begin() + to, std::move(item));
    for (std::size_t i = 0; i < tabs.size(); ++i)
        if (tabs[i]->id == id)
            active = i;
    changed(true);
}
void BrowserController::navigate(std::string input) {
    close_extension_popup();
    auto t = current();
    if (!t)
        return;
    try {
        const auto url = navigation_url(std::move(input), profile.search_engine);
        ui->editing = false;
        ui->panel(BrowserUi::None);
        if (url == "aura://newtab") {
            t->ntp = true;
            t->page.url = url;
            t->page.title = tr(t->private_tab ? "Private tab" : "New tab");
            window->title(t->page.title + " — Aura");
            if (t->browser) {
                t->browser->StopLoad();
                t->browser->GetHost()->SetAudioMuted(true);
                t->browser->GetHost()->WasHidden(true);
            }
        } else {
            if (t->browser)
                t->browser->GetHost()->SetAudioMuted(t->muted);
            ensure_browser(*t, url);
        }
        ++audit["navigations"];
        changed(true);
    } catch (const std::exception &e) {
        ui->toast(tr(e.what()));
    }
}
void BrowserController::back() {
    close_extension_popup();
    if (auto t = current(); t && t->browser && t->back) {
        t->ntp = false;
        t->browser->GetHost()->WasHidden(false);
        t->browser->GoBack();
        ++audit["back"];
        changed();
    }
}
void BrowserController::forward() {
    close_extension_popup();
    if (auto t = current(); t && t->browser && t->forward) {
        t->ntp = false;
        t->browser->GoForward();
        ++audit["forward"];
        changed();
    }
}
void BrowserController::reload(bool ignore) {
    if (auto t = current(); t && t->browser && !t->ntp) {
        if (t->loading)
            t->browser->StopLoad();
        else if (ignore)
            t->browser->ReloadIgnoreCache();
        else
            t->browser->Reload();
        ++audit["reload"];
    }
}
void BrowserController::zoom(double delta, bool reset) {
    if (auto t = current(); t && t->browser) {
        t->zoom = reset ? std::log(profile.default_zoom / 100.0) / std::log(1.2)
                        : std::clamp(t->zoom + delta, -4.0, 5.0);
        t->browser->GetHost()->SetZoomLevel(t->zoom);
        ui->toast(tr("Page zoom  ") +
                  std::to_string(static_cast<int>(std::round(std::pow(1.2, t->zoom) * 100))) + "%");
    }
}
void BrowserController::bookmark() {
    if (auto t = current(); t && !t->ntp) {
        profile.toggle_bookmark(t->page.url, t->page.title);
        ui->toast(tr(profile.bookmarked(t->page.url) ? "Bookmark saved" : "Bookmark removed"));
        ++audit["bookmark"];
        changed(true);
    }
}
void BrowserController::pin() {
    if (auto t = current()) {
        t->pinned = !t->pinned;
        ui->toast(tr(t->pinned ? "Tab pinned" : "Tab unpinned"));
        changed(true);
    }
}
void BrowserController::mute() {
    if (auto t = current()) {
        t->muted = !t->muted;
        if (t->browser)
            t->browser->GetHost()->SetAudioMuted(t->muted);
        ui->toast(tr(t->muted ? "Tab muted" : "Tab unmuted"));
        changed();
    }
}
void BrowserController::devtools() {
    if (auto t = current(); t && t->browser) {
        CefWindowInfo info;
#ifdef _WIN32
        info.SetAsPopup(window->handle(), tr("Aura Developer Tools"));
#else
        CefString(&info.window_name) = tr("Aura Developer Tools");
#endif
        info.bounds = {0, 0, 1100, 800};
        CefBrowserSettings settings;
        t->browser->GetHost()->ShowDevTools(info, new ToolsClient(*this), settings, {});
    }
}
void BrowserController::find_text(const std::string &text, bool next, bool backwards) {
    if (auto t = current(); t && t->browser) {
        if (text.empty()) {
            t->browser->GetHost()->StopFinding(true);
            find_matches = find_active = 0;
        } else
            t->browser->GetHost()->Find(text, !backwards, false, next);
        changed();
    }
}
void BrowserController::close_menu() {
    auto callback = std::move(menu_callback);
    menu_action = {};
    menu.clear();
    if (callback)
        callback->Cancel();
    changed();
}
void BrowserController::answer_dialog(bool accept) {
    if (dialogs.empty())
        return;
    auto dialog = std::move(dialogs.front());
    dialogs.pop_front();
    if (!accept && dialog.before_unload) {
        quitting = false;
        for (auto &t : tabs)
            if (!t->page.closed)
                t->closing = false;
    }
    if (!accept && dialog.before_unload)
        quit_confirmed_ = false;
    if (dialog.callback)
        dialog.callback->Continue(accept, dialog.input.value);
    if (dialog.action)
        dialog.action(accept, dialog.input.value);
    changed();
}
void BrowserController::request_quit(bool force) {
    if (quitting && !force)
        return;
    if (!force && !quit_confirmed_ && profile.confirm_quit &&
        std::count_if(tabs.begin(), tabs.end(), [](auto &t) { return !t->closing; }) > 1) {
        if (std::any_of(dialogs.begin(), dialogs.end(),
                        [](auto &d) { return d.origin == "Close Aura?"; }))
            return;
        confirm_action("Close Aura?", "All open tabs will close. Continue?", [this] {
            quit_confirmed_ = true;
            request_quit();
        });
        return;
    }
    close_menu();
    quit_snapshot_ = tabs_json(tabs);
    try {
        save_session();
    } catch (const std::exception &e) {
        spdlog::error("{}", e.what());
    }
    quitting = true;
    close_extension_popup(false);
    while (!dialogs.empty())
        answer_dialog(force);
    quitting = true;
    for (auto &t : tabs) {
        t->closing = true;
        if (t->browser) {
            t->browser->GetHost()->CloseDevTools();
            t->browser->GetHost()->CloseBrowser(force);
        } else
            t->page.closed = true;
    }
}
bool BrowserController::finished() {
    const bool browsers_closed =
        quitting && tools_open == 0 && !extension_popup &&
        std::all_of(closing_popups_.begin(), closing_popups_.end(),
                    [](const auto &p) { return p->page.closed; }) &&
        std::all_of(tabs.begin(), tabs.end(), [](auto &t) { return t->page.closed; });
    if (browsers_closed && !cleanup_started_) {
        cleanup_started_ = true;
        if (profile.clear_history_exit) {
            profile.history.clear();
            try {
                profile.save();
            } catch (const std::exception &e) {
                spdlog::error("{}", e.what());
            }
        }
        if (profile.clear_cookies_exit)
            clear_data("cookies");
    }
    return browsers_closed && data_tasks_ == 0;
}
void BrowserController::prepare_shutdown() {
    extensions.shutdown();
    closing_popups_.clear();
    extension_popup.reset();
    close_menu();
    dialogs.clear();
    for (auto &d : downloads)
        d.callback = nullptr;
    for (auto &t : tabs) {
        t->browser = nullptr;
        t->client = nullptr;
    }
    tabs.clear();
    private_context_ = nullptr;
}
void BrowserController::save_session() {
    profile.session.clear();
    int index = 0;
    profile.active = 0;
    for (auto &t : tabs)
        if (!t->private_tab && !t->closing) {
            profile.session.push_back({t->page.url, t->pinned});
            if (t.get() == current())
                profile.active = index;
            ++index;
        }
    profile.save();
}
std::string BrowserController::report() const {
    std::ostringstream out;
    out << "{\"product\":\"Aura\",\"window\":" << window->native_id()
        << ",\"cef_created\":" << created_ << ",\"cef_closed\":" << closed_ << ",\"compositor\":\""
        << json_escape(window->compositor()) << "\",\"renderer\":\""
        << json_escape(window->renderer()) << "\",\"presentation_frames\":" << window->frames()
        << ",\"presentation\":" << window->presentation_stats()
        << ",\"interface\":{\"requested\":" << profile.ui_zoom << ",\"applied\":" << ui_scale * 100
        << ",\"device_scale\":" << scale << ",\"width\":" << width << ",\"height\":" << height
        << "}"
        << ",\"panel\":" << static_cast<int>(ui->visible_panel())
        << ",\"blocked_requests\":" << request_policy->protection->blocked.load()
        << ",\"adblock_ready\":" << (request_policy->protection->ready() ? "true" : "false")
        << ",\"extensions\":" << extensions.report()
        << ",\"extensions_ui\":" << ui->extensions_report()
        << ",\"extension_popup\":" << ui->extension_popup_report()
        << ",\"extension_toolbar\":" << ui->extension_toolbar_report()
        << ",\"shortcuts_ui\":" << ui->shortcuts_report()
        << ",\"start_page\":" << ui->start_page_report()
        << ",\"settings_ui\":" << ui->settings_report() << ",\"dialog_count\":" << dialogs.size()
        << ",\"tabs\":" << (quit_snapshot_.empty() ? tabs_json(tabs) : quit_snapshot_)
        << ",\"settings\":{";
    bool first_setting = true;
    for (const auto &setting : settings_catalog()) {
        if (!first_setting)
            out << ',';
        first_setting = false;
        out << '"' << setting.id << "\":";
        const bool text = std::holds_alternative<std::string BrowserPreferences::*>(setting.member);
        if (text)
            out << '"';
        out << json_escape(setting_value(profile, setting));
        if (text)
            out << '"';
    }
    out << '}' << ",\"history_count\":" << profile.history.size()
        << ",\"bookmark_count\":" << profile.bookmarks.size() << ",\"downloads\":[";
    bool first = true;
    for (auto &d : downloads) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":\"" << json_escape(d.name)
            << "\",\"complete\":" << (d.complete ? "true" : "false")
            << ",\"interrupted\":" << (d.interrupted ? "true" : "false")
            << ",\"received\":" << d.received << '}';
    }
    out << "],\"audit\":{";
    first = true;
    for (auto &[key, value] : audit) {
        if (!first)
            out << ',';
        first = false;
        out << '"' << key << "\":" << value;
    }
    out << "}}";
    return out.str();
}
void BrowserController::browser_created(int id, CefRefPtr<CefBrowser> browser) {
    if (auto t = tab(id)) {
        t->browser = browser;
        if (t->private_tab)
            configure_font_preferences(browser->GetHost()->GetRequestContext());
        const auto initial = t->page.url;
        t->devtools = new DevToolsSession(browser);
        t->devtools->attach();
        setup_site_tools(id, [this, id, initial] {
            if (auto t = tab(id); t && t->browser && !t->closing)
                t->browser->GetMainFrame()->LoadURL(initial);
        });
        ++created_;
    }
}
void BrowserController::browser_closed(int id) {
    if (auto t = tab(id)) {
        t->page.closed = true;
        if (t->devtools)
            t->devtools->close();
        t->devtools = nullptr;
        t->browser = nullptr;
        ++closed_;
        changed();
    }
}
void BrowserController::page_changed(int id, bool history) {
    if (auto t = tab(id)) {
        if (t == current())
            window->title(t->page.title + " — Aura");
        if (!t->ntp && t->page.loaded && !t->page.failed)
            profile.visit(t->page.url, t->page.title, t->private_tab);
        if (history)
            ++audit["pages_loaded"];
        changed(!t->private_tab);
    }
}
void BrowserController::open_from_page(int source, std::string url, bool foreground) {
    const auto t = tab(source);
    if (!t)
        return;
    const bool private_tab = t->private_tab;
    defer([this, url = std::move(url), private_tab, foreground] {
        new_tab(url, private_tab, foreground && profile.foreground_links);
    });
    ++audit["page_opened_tabs"];
}
void BrowserController::update_favicon(int id, const std::string &url) {
    auto t = tab(id);
    if (!profile.show_favicons || !t || !t->browser || url == t->icon_url)
        return;
    t->icon_url = url;
    t->browser->GetHost()->DownloadImage(url, true, 32, false, new ImageCallback(*this, id));
}
void BrowserController::start_download(CefRefPtr<CefDownloadItem> item,
                                       CefRefPtr<CefBeforeDownloadCallback> callback,
                                       const std::string &suggested) {
    spdlog::info("Download {} accepted", item->GetId());
    (void)item;
    if (profile.ask_download_name) {
        BrowserDialog dialog;
        dialog.origin = "Download filename";
        dialog.message = "Choose a filename. Existing files are kept.";
        dialog.owned = true;
        dialog.prompt = true;
        dialog.input.set(suggested, true);
        dialog.action = [this, callback, suggested](bool accept, const std::string &name) {
            if (accept)
                finish_download(callback, name.empty() ? suggested : name);
        };
        dialogs.push_back(std::move(dialog));
        changed();
    } else
        finish_download(callback, suggested);
}
std::filesystem::path BrowserController::downloads_directory() const {
    return profile.download_directory.empty() ? profile.directory / "downloads"
                                              : path_from_utf8(profile.download_directory);
}
void BrowserController::finish_download(CefRefPtr<CefBeforeDownloadCallback> callback,
                                        std::string input) {
    auto folder = downloads_directory();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        ui->toast(tr("Cannot create the download folder"));
        return;
    }
    auto name = path_from_utf8(input).filename();
    if (name.empty() || name == "." || name == "..")
        name = "download";
    if (profile.date_downloads) {
        const auto now = std::time(nullptr);
        const auto local = local_time(now);
        std::ostringstream prefix;
        prefix << std::put_time(&local, "%Y-%m-%d_");
        name = path_from_utf8(prefix.str() + utf8_path(name));
    }
    auto path = folder / name;
    int suffix = 1;
    while (std::filesystem::exists(path) ||
           std::find(download_paths_.begin(), download_paths_.end(), path) != download_paths_.end())
        path = folder / path_from_utf8(utf8_path(name.stem()) + " (" + std::to_string(suffix++) +
                                       ")" + utf8_path(name.extension()));
    download_paths_.push_back(path);
    callback->Continue(utf8_path(path), false);
    ++audit["download_started"];
    if (profile.open_downloads)
        ui->panel(BrowserUi::Downloads);
    ui->toast(tr("Download started"));
}
void BrowserController::download_updated(CefRefPtr<CefDownloadItem> item,
                                         CefRefPtr<CefDownloadItemCallback> callback) {
    if (!item->IsValid())
        return;
    if (item->IsComplete() || item->IsCanceled() || item->IsInterrupted())
        spdlog::info("Download {} complete={} canceled={} interrupted={} reason={}", item->GetId(),
                     item->IsComplete(), item->IsCanceled(), item->IsInterrupted(),
                     static_cast<int>(item->GetInterruptReason()));
    auto it = std::find_if(downloads.begin(), downloads.end(),
                           [&](auto &d) { return d.id == item->GetId(); });
    if (it == downloads.end()) {
        downloads.insert(downloads.begin(), Download{});
        it = downloads.begin();
        it->id = item->GetId();
    }
    auto &d = *it;
    d.name = item->GetSuggestedFileName();
    d.path = item->GetFullPath();
    if (!d.path.empty())
        d.name = utf8_path(path_from_utf8(d.path).filename());
    d.percent = item->GetPercentComplete();
    d.received = item->GetReceivedBytes();
    d.total = item->GetTotalBytes();
    const bool just_completed = !d.complete && item->IsComplete();
    d.complete = item->IsComplete();
    d.cancelled = item->IsCanceled();
    d.paused = item->IsPaused();
    d.interrupted = item->IsInterrupted();
    d.interrupt_reason = item->GetInterruptReason();
    d.callback = d.complete || d.cancelled ? nullptr : callback;
    if (just_completed && profile.notify_downloads)
        ui->toast(tr("Download complete") + " · " + d.name);
    if (just_completed && profile.close_downloads && ui->visible_panel() == BrowserUi::Downloads &&
        std::all_of(downloads.begin(), downloads.end(),
                    [](auto &v) { return v.complete || v.cancelled; }))
        ui->panel(BrowserUi::None);
    changed();
}
void BrowserController::download_action(std::size_t i, bool cancel) {
    if (i >= downloads.size() || !downloads[i].callback)
        return;
    auto &d = downloads[i];
    if (cancel)
        d.callback->Cancel();
    else if (d.paused || d.interrupted)
        d.callback->Resume();
    else
        d.callback->Pause();
}
void BrowserController::confirm_action(std::string title, std::string message,
                                       std::function<void()> action) {
    BrowserDialog dialog;
    dialog.origin = std::move(title);
    dialog.message = std::move(message);
    dialog.owned = true;
    dialog.action = [action = std::move(action)](bool accept, const std::string &) {
        if (accept)
            action();
    };
    dialogs.push_back(std::move(dialog));
    changed();
}
void BrowserController::cancel_permissions(int tab_id) {
    std::vector<std::function<void(bool, const std::string &)>> actions;
    for (auto it = dialogs.begin(); it != dialogs.end();) {
        if (it->permission && it->tab == tab_id) {
            actions.push_back(std::move(it->action));
            it = dialogs.erase(it);
        } else
            ++it;
    }
    for (auto &action : actions)
        if (action)
            action(false, "");
    changed();
}
void BrowserController::clear_data(const std::string &kind) {
    if (kind == "history") {
        profile.history.clear();
        changed(true);
        ui->toast(tr("Aura history cleared"));
        return;
    }
    if (kind == "reset") {
        static_cast<BrowserPreferences &>(profile) = BrowserPreferences{};
        apply_settings(true);
        ui->toast(tr("Settings reset"));
        return;
    }
    if (kind == "downloads") {
        std::erase_if(downloads, [](auto &d) { return d.complete || d.cancelled; });
        changed();
        return;
    }
    std::vector<CefRefPtr<CefRequestContext>> contexts{CefRequestContext::GetGlobalContext()};
    if (private_context_)
        contexts.push_back(private_context_);
    for (auto &context : contexts) {
        ++data_tasks_;
        auto done = [this, kind] {
            --data_tasks_;
            ++audit["cleared_" + kind];
            if (!quitting && data_tasks_ == 0)
                ui->toast(tr("Browser data cleared"));
            changed();
        };
        if (kind == "cookies") {
            if (!context->GetCookieManager(nullptr)->DeleteCookies("", "",
                                                                   new CookiesDeleted(done)))
                done();
        } else if (kind == "cache")
            context->ClearHttpCache(new Completion(done));
        else
            done();
    }
}
void BrowserController::open_folder() {
    const auto path = utf8_path(downloads_directory());
    std::filesystem::create_directories(path_from_utf8(path));
#ifdef _WIN32
    const auto folder = path_from_utf8(path).wstring();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window->handle(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ui->toast(tr("Could not open the downloads folder"));
#else
    pid_t child{};
    char command[] = "xdg-open";
    char *arguments[] = {command, const_cast<char *>(path.c_str()), nullptr};
    if (posix_spawnp(&child, command, nullptr, nullptr, arguments, environ) == 0)
        std::thread([child] {
            int status;
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        }).detach();
    else
        ui->toast(tr("Could not open the downloads folder"));
#endif
}
} // namespace aura
