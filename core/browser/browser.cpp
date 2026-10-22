#include "core/browser/browser.hpp"
#include "aura/design_tokens.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include "common/update_key.hpp"
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
#include <shellapi.h>
#include <windows.h>
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
            << ",\"workspace\":" << t->workspace
            << ",\"loaded\":" << (t->page.loaded ? "true" : "false")
            << ",\"failed\":" << (t->page.failed ? "true" : "false")
            << ",\"http_status\":" << t->page.http_status << ",\"frame_rate\":"
            << (t->browser ? t->browser->GetHost()->GetWindowlessFrameRate() : 0)
            << ",\"frames\":" << t->page.view.generation() << ",\"audio_muted\":"
            << (t->browser && t->browser->GetHost()->IsAudioMuted() ? "true" : "false")
            << ",\"zoom_percent\":"
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
    ImageCallback(BrowserController &owner, int id)
        : owner_(owner), id_(id), lifetime_(owner.callback_lifetime) {}
    void OnDownloadImageFinished(const CefString &, int, CefRefPtr<CefImage> image) override {
        if (!image || lifetime_.expired())
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
    std::weak_ptr<void> lifetime_;
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
BrowserState::BrowserState(const std::filesystem::path &path)
    : profile(path), extensions(path), updates(path), passwords(path / "passwords.aura") {
    previous_windows = profile.windows;
    if (previous_windows.empty() && !profile.session.empty())
        previous_windows.push_back({profile.session, profile.active});
    unclean_start = !profile.session_clean;
    profile.session_clean = false;
}
BrowserController::BrowserController(std::filesystem::path path, std::filesystem::path font,
                                     double factor, bool accelerated,
                                     std::shared_ptr<BrowserState> shared, bool private_mode)
    : state_(shared ? std::move(shared) : std::make_shared<BrowserState>(path)),
      profile(state_->profile), private_window(private_mode), scale(factor),
      extensions(state_->extensions), updates(state_->updates), importer(state_->importer),
      passwords(state_->passwords), request_policy(state_->policy),
      private_context_(state_->private_context) {
    resources = font.parent_path().parent_path();
    if (!request_policy->protection)
        request_policy->protection = std::make_shared<Protection>();
    if (!state_->filters)
        state_->filters =
            std::make_unique<FilterLists>(profile.directory, resources, request_policy->protection);
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
    hooks.pointer = [this](const PointerInput &p) {
        if (p.kind == PointerInput::Down)
            ++audit["native_pointer_down"];
        ui->pointer(p);
    };
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
    state_->windows.push_back(this);
}
BrowserController::~BrowserController() {
    callback_lifetime.reset();
    std::erase(state_->windows, this);
}
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
void BrowserController::apply_settings(bool update_zoom, bool propagate) {
    if (propagate)
        for (auto *other : state_->windows)
            if (other != this && !other->quitting)
                other->apply_settings(update_zoom, false);
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
std::vector<int> BrowserController::visible_tabs() const {
    std::vector<int> result;
    for (size_t i = 0; i < tabs.size(); ++i)
        if (!tabs[i]->page.closed && tabs[i]->workspace == workspace)
            result.push_back(int(i));
    return result;
}
void BrowserController::cycle_tab(int direction) {
    const auto visible = visible_tabs();
    if (visible.empty())
        return;
    const auto it = std::find(visible.begin(), visible.end(), active);
    const int position = it == visible.end() ? 0 : int(it - visible.begin());
    activate(visible[(position + direction + int(visible.size())) % visible.size()]);
}
void BrowserController::select_visible_tab(int number) {
    const auto visible = visible_tabs();
    if (!visible.empty())
        activate(visible[number == 9 ? visible.size() - 1
                                     : std::min(size_t(number - 1), visible.size() - 1)]);
}
std::string BrowserController::workspace_name() const {
    if (private_window)
        return tr("Private window");
    for (const auto &w : profile.workspaces)
        if (w.id == workspace)
            return tr(w.name);
    return tr("Personal space");
}
void BrowserController::switch_workspace(int id) {
    if (private_window || quitting || !profile.has_workspace(id))
        return;
    if (auto old = current())
        workspace_selection_[workspace] = old->id;
    workspace = id;
    auto visible = visible_tabs();
    if (visible.empty())
        open_new_tab();
    else {
        int selected = visible.front();
        for (int i : visible)
            if (tabs[i]->id == workspace_selection_[id])
                selected = i;
        activate(selected);
    }
    ++audit["workspace_switches"];
    changed(true);
}
void BrowserController::remove_workspace(int id) {
    if (!profile.remove_workspace(id))
        return;
    for (auto *other : state_->windows) {
        for (auto &t : other->tabs)
            if (t->workspace == id)
                t->workspace = 0;
        for (auto &t : other->recently_closed_)
            if (t.workspace == id)
                t.workspace = 0;
        if (other->workspace == id)
            other->switch_workspace(0);
    }
    for (auto &saved : state_->previous_windows) {
        if (saved.workspace == id)
            saved.workspace = 0;
        for (auto &t : saved.tabs)
            if (t.workspace == id)
                t.workspace = 0;
    }
    changed(true);
}
void BrowserController::start(const std::string &initial, bool private_mode, bool resume_once) {
    private_window = private_window || private_mode;
    private_mode = private_window;
    background_updates_ = !private_mode;
    apply_settings();
    if (private_mode)
        new_tab(initial.empty() ? "aura://newtab" : initial, true);
    else if (!initial.empty())
        new_tab(initial);
    else if ((profile.restore || resume_once) && !profile.windows.empty()) {
        state_->previous_restored = true;
        const auto saved = profile.windows;
        restore_window(saved.front());
        for (size_t i = 1; i < saved.size(); ++i)
            state_->pending_windows.push_back({false, saved[i], ""});
    } else if ((profile.restore || resume_once) && !profile.session.empty()) {
        state_->previous_restored = true;
        auto saved = profile.session;
        int selected = profile.active;
        restoring_ = true;
        for (auto &t : saved)
            new_tab(t.url, false, false, t.pinned);
        restoring_ = false;
        activate(std::min(selected, static_cast<int>(tabs.size()) - 1));
    } else
        new_tab(profile.startup_page == "home" ? profile.home_page : "aura://newtab");
    if (!private_mode && state_->windows.size() == 1 && state_->unclean_start &&
        !state_->previous_restored && !state_->previous_windows.empty()) {
        state_->recovery_pending = true;
        BrowserDialog dialog;
        dialog.owned = true;
        dialog.origin = tr("Restore interrupted session?");
        dialog.accept_label = "Reopen";
        dialog.message = tr("Reopen your previous normal windows? Private tabs are excluded.");
        dialog.action = [this](bool accept, const std::string &) {
            state_->recovery_pending = false;
            if (accept)
                restore_previous_session();
            changed(true);
        };
        dialogs.push_back(std::move(dialog));
    }
    if (!private_mode && profile.check_updates && updates.due())
        updates.check();
    if (profile.recovered_profile) {
        ui->toast(profile.language == "ru"
                      ? "Профиль повреждён. Загружена копия или настройки по умолчанию; исходный файл сохранён."
                      : "Profile damaged. Loaded backup or defaults; original file preserved.");
        profile.recovered_profile = false;
    }
}
void BrowserController::changed(bool persistent) {
    for (auto *other : state_->windows)
        if (other->window)
            other->window->invalidate();
    if (persistent) {
        needs_save_ = true;
        save_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    }
}
void BrowserController::defer(std::function<void()> action) {
    pending_.push_back(std::move(action));
}
void BrowserController::tick() {
    if (background_updates_ && profile.check_updates && updates.due())
        updates.check();
    auto &filters = filter_lists();
    if (background_updates_ && profile.adblock && profile.filter_updates &&
        std::chrono::steady_clock::now() >= state_->filters_after && filters.due())
        filters.check();
    const auto filter_revision = filters.revision;
    if (filters.tick()) {
        if (filters.revision != filter_revision)
            for (auto *other : state_->windows)
                for (auto &t : other->tabs)
                    other->update_site_tools(*t);
        changed();
    }
    const bool password_changed = passwords.tick();
    if (password_changed || password_generation_seen_ != passwords.generation) {
        password_generation_seen_ = passwords.generation;
        if (!passwords.vault.unlocked()) {
            for (auto *other : state_->windows) {
                other->ui->passwords_locked();
                std::erase_if(other->dialogs, [](const auto &d) { return d.sensitive; });
            }
        }
        changed();
    }
    if (importer.tick())
        changed();
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
    const bool had_update = bool(updates.release);
    if (updates.tick()) {
        changed();
        if (!had_update && updates.release && !updates.busy())
            ui->toast(tr("A new Aura release is available."));
    }
    if (extension_busy != extensions.busy)
        changed();
    auto work = std::move(pending_);
    pending_.clear();
    for (auto &action : work)
        action();
    const int old_active_id = current() ? current()->id : 0;
    const bool active_closed = current() && current()->page.closed;
    for (int i = static_cast<int>(tabs.size()) - 1; i >= 0; --i)
        if (tabs[i]->page.closed && !tabs[i]->popup_pending) {
            if (tabs[i]->remember_on_close && !tabs[i]->private_tab && !quitting) {
                recently_closed_.push_back(
                    {tabs[i]->page.url, tabs[i]->pinned, tabs[i]->workspace});
                if (recently_closed_.size() > 30)
                    recently_closed_.erase(recently_closed_.begin());
            }
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
    else if (!quitting && (active_closed || !current() || current()->workspace != workspace)) {
        const auto visible = visible_tabs();
        if (visible.empty())
            open_new_tab();
        else
            activate(visible[std::min(
                int(visible.size()) - 1,
                profile.close_selection == "last" ? int(visible.size()) - 1
                : profile.close_selection == "first"
                    ? 0
                    : int(std::lower_bound(visible.begin(), visible.end(), active) -
                          visible.begin()))]);
    } else if (!quitting && current() && current()->id != old_active_id)
        activate(active);
    if (private_context_) {
        bool private_alive = false;
        for (const auto *other : state_->windows)
            for (const auto &t : other->tabs)
                private_alive = private_alive || (t->private_tab && !t->page.closed);
        if (!private_alive)
            private_context_ = nullptr;
    }
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
    t.page.http_status = 0;
    t.page.error.clear();
    t.page.loaded = false;
    t.loading = true;
    t.progress = .04;
    if (t.browser) {
        if (!t.site_tools_ready) {
            t.pending_url = url;
            return;
        }
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
    t.pending_url = url;
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
void BrowserController::new_tab(std::string input, bool private_tab, bool foreground, bool pinned,
                                int space) {
    if (quitting)
        return;
    private_tab = private_tab || private_window;
    std::string url;
    try {
        url = navigation_url(std::move(input), profile.search_engine);
    } catch (const std::exception &e) {
        ui->toast(tr(e.what()));
        return;
    }
    auto t = std::make_unique<Tab>();
    t->id = next_id_++;
    t->workspace = private_window ? 0 : profile.has_workspace(space) ? space : workspace;
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
    if (auto old = current())
        workspace_selection_[old->workspace] = old->id;
    active = index;
    workspace = tabs[index]->workspace;
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
    if ((profile.quit_last_tab || private_window) && !quitting &&
        std::count_if(tabs.begin(), tabs.end(), [](auto &t) { return !t->closing; }) == 1) {
        request_quit();
        return;
    }
    cancel_permissions(t.id);
    if (t.closing)
        return;
    t.remember_on_close = !t.private_tab;
    close_menu();
    t.closing = true;
    if (t.browser) {
        t.browser->GetHost()->CloseDevTools();
        t.browser->GetHost()->CloseBrowser(false);
    } else if (!t.popup_pending)
        t.page.closed = true;
    ++audit["tabs_closed_by_user"];
    changed(true);
}
void BrowserController::reopen() {
    if (recently_closed_.empty())
        return;
    auto t = recently_closed_.back();
    recently_closed_.pop_back();
    new_tab(t.url, false, true, t.pinned, profile.has_workspace(t.workspace) ? t.workspace : 0);
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
            if (t->browser) {
                // A real history entry unloads the old document and honours
                // beforeunload. Commit the native start page only on address change.
                t->browser->GetMainFrame()->LoadURL("about:blank#aura-newtab");
            } else {
                t->ntp = true;
                t->page.url = url;
                t->page.title = tr(t->private_tab ? "Private tab" : "New tab");
                window->title(t->page.title + " — Aura");
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
        t->browser->GoBack();
        ++audit["back"];
        changed();
    }
}
void BrowserController::forward() {
    close_extension_popup();
    if (auto t = current(); t && t->browser && t->forward) {
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
    menu_width = 240;
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
        if (auto t = tab(dialog.tab))
            t->remember_on_close = false;
        // Cancel the whole exit transaction, including restart/update intent.
        if (state_->exiting) {
            state_->exiting = false;
            state_->session_frozen = false;
            for (auto *other : state_->windows) {
                other->restart_on_exit = false;
                other->install_update_on_exit = false;
                other->quitting = false;
                other->quit_confirmed_ = false;
                for (auto &t : other->tabs)
                    if (!t->page.closed)
                        t->closing = false;
            }
        }
        quitting = false;
        quit_snapshot_.clear();
        changed(true);
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
void BrowserController::restart() {
    if (quitting || extensions.busy || passwords.busy || importer.busy || updates.busy() ||
        filter_lists().busy()) {
        ui->toast(tr("Finish the current operation before restarting."));
        return;
    }
    confirm_action(tr("Restart Aura?"), tr("Regular tabs will reopen. Private tabs will close."),
                   [this] {
                       try {
                           save_session();
                       } catch (...) {
                           ui->toast(tr("Cannot save the session. Restart cancelled."));
                           return;
                       }
                       restart_on_exit = true;
                       request_exit(true);
                   });
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
    const bool another_normal =
        std::any_of(state_->windows.begin(), state_->windows.end(),
                    [this](auto *w) { return w != this && !w->quitting && !w->private_window; });
    if (!state_->exiting && another_normal)
        quitting = true;
    try {
        save_session();
    } catch (const std::exception &e) {
        spdlog::error("{}", e.what());
    }
    quitting = true;
    close_extension_popup(false);
    // Keep existing beforeunload prompts: closing must never decide for the user.
    if (force) {
        while (!dialogs.empty())
            answer_dialog(dialogs.front().before_unload);
    } else {
        for (auto it = dialogs.begin(); it != dialogs.end();) {
            if (it->before_unload) {
                ++it;
                continue;
            }
            auto dialog = std::move(*it);
            it = dialogs.erase(it);
            if (dialog.callback)
                dialog.callback->Continue(false, "");
            if (dialog.action)
                dialog.action(false, "");
        }
    }
    quitting = true;
    for (auto &t : tabs) {
        t->closing = true;
        if (t->browser) {
            t->browser->GetHost()->CloseDevTools();
            t->browser->GetHost()->CloseBrowser(force);
        } else if (!t->popup_pending)
            t->page.closed = true;
    }
}
bool BrowserController::finished() {
    const bool browsers_closed =
        quitting && tools_open == 0 && !extension_popup &&
        std::all_of(closing_popups_.begin(), closing_popups_.end(),
                    [](const auto &p) { return p->page.closed; }) &&
        std::all_of(tabs.begin(), tabs.end(), [](auto &t) { return t->page.closed; });
    const bool all_closed =
        std::all_of(state_->windows.begin(), state_->windows.end(), [](auto *w) {
            return w->quitting && w->tools_open == 0 && !w->extension_popup &&
                   std::all_of(w->closing_popups_.begin(), w->closing_popups_.end(),
                               [](const auto &p) { return p->page.closed; }) &&
                   std::all_of(w->tabs.begin(), w->tabs.end(),
                               [](const auto &t) { return t->page.closed; });
        });
    if (browsers_closed && all_closed && !state_->cleanup_started) {
        state_->cleanup_started = true;
        profile.session_clean = true;
        try {
            profile.save();
        } catch (const std::exception &e) {
            spdlog::error("{}", e.what());
        }
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
    importer.shutdown();
    passwords.shutdown();
    updates.shutdown();
    extensions.shutdown();
    filter_lists().shutdown();
    detach_window();
    private_context_ = nullptr;
}
void BrowserController::detach_window() {
    ui->passwords_locked();
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
}
void BrowserController::new_window(bool private_mode, std::string url) {
    if (!state_->exiting)
        state_->pending_windows.push_back({private_mode, {}, std::move(url)});
}
void BrowserController::restore_window(const SavedWindow &saved) {
    background_updates_ = !private_window;
    workspace = saved.workspace;
    restoring_ = true;
    for (const auto &t : saved.tabs) {
        new_tab(t.url, false, false, t.pinned, t.workspace);
        if (t.workspace_active && !tabs.empty())
            workspace_selection_[t.workspace] = tabs.back()->id;
    }
    restoring_ = false;
    if (tabs.empty())
        open_new_tab();
    activate(std::clamp(saved.active, 0, int(tabs.size()) - 1));
}
void BrowserController::restore_previous_session() {
    if (state_->previous_restored || state_->previous_windows.empty())
        return;
    state_->previous_restored = true;
    size_t first = 0;
    if (!private_window && tabs.size() == 1 && tabs.front()->ntp && !tabs.front()->browser) {
        tabs.clear();
        restore_window(state_->previous_windows.front());
        first = 1;
    }
    for (size_t i = first; i < state_->previous_windows.size(); ++i)
        state_->pending_windows.push_back({false, state_->previous_windows[i], ""});
}
void BrowserController::request_exit(bool force) {
    if (!force && profile.confirm_quit) {
        confirm_action(tr("Close Aura?"),
                       tr("All windows will close. Private tabs will not be saved."),
                       [this] { request_exit(true); });
        return;
    }
    try {
        save_session();
    } catch (const std::exception &e) {
        if (!force) {
            ui->toast(tr(e.what()));
            return;
        }
        spdlog::error("Could not save all windows: {}", e.what());
    }
    state_->session_frozen = true;
    state_->exiting = true;
    state_->pending_windows.clear();
    for (auto *other : state_->windows)
        if (!other->quitting) {
            // `force` above acknowledges Aura's application-level confirmation,
            // not the individual websites' unsaved-data prompts.
            other->quit_confirmed_ = true;
            other->request_quit(false);
        }
}
void BrowserController::save_session() {
    if (!state_->session_frozen && !state_->recovery_pending) {
        std::vector<SavedWindow> saved;
        for (const auto *other : state_->windows) {
            if (other->private_window || other->quitting)
                continue;
            SavedWindow window;
            window.workspace = other->workspace;
            const auto current = other->active >= 0 && other->active < int(other->tabs.size())
                                     ? other->tabs[other->active].get()
                                     : nullptr;
            for (const auto &t : other->tabs)
                if (!t->private_tab && !t->closing) {
                    const auto remembered = other->workspace_selection_.find(t->workspace);
                    const bool selected =
                        t->workspace == other->workspace && current && !current->private_tab
                            ? t.get() == current
                            : remembered != other->workspace_selection_.end() &&
                                  remembered->second == t->id;
                    if (selected && t->workspace == window.workspace)
                        window.active = int(window.tabs.size());
                    window.tabs.push_back({t->pending_url.empty() ? t->page.url : t->pending_url,
                                           t->pinned, t->workspace, selected});
                }
            if (!window.tabs.empty())
                saved.push_back(std::move(window));
        }
        // A private-only remainder must not erase the last normal session.
        if (!saved.empty()) {
            profile.windows = std::move(saved);
            profile.session = profile.windows.front().tabs;
            profile.active = profile.windows.front().active;
        }
    }
    profile.save();
}
std::string BrowserController::report() const {
    std::ostringstream out;
    out << "{\"product\":\"Aura\",\"pid\":" << process_id() << ",\"window\":" << window->native_id()
        << ",\"cef_created\":" << created_ << ",\"cef_closed\":" << closed_ << ",\"compositor\":\""
        << json_escape(window->compositor()) << "\",\"renderer\":\""
        << json_escape(window->renderer())
        << "\",\"version\":" << json_quote(std::string(browser_version))
        << ",\"release_sequence\":" << browser_release_sequence
        << ",\"presentation_frames\":" << window->frames()
        << ",\"presentation\":" << window->presentation_stats()
        << ",\"interface\":{\"requested\":" << profile.ui_zoom << ",\"applied\":" << ui_scale * 100
        << ",\"device_scale\":" << scale << ",\"width\":" << width << ",\"height\":" << height
        << "}"
        << ",\"active_tab\":" << active << ",\"workspace\":" << workspace
        << ",\"workspace_name\":" << json_quote(workspace_name())
        << ",\"address_input_bytes\":" << (ui->editing ? ui->address.value.size() : 0)
        << ",\"private_window\":" << (private_window ? "true" : "false")
        << ",\"panel\":" << static_cast<int>(ui->visible_panel())
        << ",\"blocked_requests\":" << request_policy->protection->blocked.load()
        << ",\"rewritten_requests\":" << request_policy->protection->rewritten_requests.load()
        << ",\"adblock_ready\":" << (request_policy->protection->ready() ? "true" : "false")
        << ",\"extensions\":" << extensions.report() << ",\"filters_ui\":" << ui->filters_report()
        << ",\"workspaces_ui\":" << ui->workspaces_report()
        << ",\"filter_lists\":" << state_->filters->report()
        << ",\"extensions_ui\":" << ui->extensions_report()
        << ",\"extension_popup\":" << ui->extension_popup_report()
        << ",\"extension_toolbar\":" << ui->extension_toolbar_report()
        << ",\"shortcuts_ui\":" << ui->shortcuts_report()
        << ",\"start_page\":" << ui->start_page_report()
        << ",\"settings_ui\":" << ui->settings_report()
        << ",\"updates_ui\":" << ui->updates_report()
        << ",\"passwords_ui\":" << ui->passwords_report()
        << ",\"import_ui\":" << ui->import_report()
        << ",\"updates\":{\"busy\":" << (updates.busy() ? "true" : "false")
        << ",\"status\":" << json_quote(updates.status)
        << ",\"installation\":" << json_quote(updates.installation_kind())
        << ",\"ready\":" << (!updates.ready_file.empty() ? "true" : "false") << "}"
        << ",\"dialog_count\":" << dialogs.size()
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
        const bool popup = t->popup_pending;
        t->popup_pending = false;
        std::erase_if(pending_popups_, [id](const auto &entry) { return entry.second == id; });
        if (popup) {
            ++audit["page_opened_tabs"];
            browser->GetHost()->SetZoomLevel(t->zoom);
            browser->GetHost()->SetAudioMuted(t->muted);
            browser->GetHost()->WasHidden(t != current());
            browser->GetHost()->SetFocus(t == current());
        }
        if (t->private_tab)
            configure_font_preferences(browser->GetHost()->GetRequestContext());

        t->devtools = new DevToolsSession(browser);
        t->devtools->attach();
        setup_site_tools(id, [this, id] {
            if (auto t = tab(id); t && t->browser && !t->closing) {
                t->site_tools_ready = true;
                auto initial = std::move(t->pending_url);
                t->pending_url.clear();
                if (!t->ntp && !initial.empty())
                    t->browser->GetMainFrame()->LoadURL(initial);
            }
        });
        ++created_;
        if (t->closing || quitting)
            browser->GetHost()->CloseBrowser(true);
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
    const int space = t->workspace;
    defer([this, url = std::move(url), private_tab, foreground, space] {
        new_tab(url, private_tab, foreground && profile.foreground_links && space == workspace,
                false, space);
    });
    ++audit["page_opened_tabs"];
}
Tab *BrowserController::prepare_popup(int source, int popup_id, const std::string &url,
                                      bool foreground) {
    auto opener = tab(source);
    if (!opener || opener->closing || quitting)
        return nullptr;
    const bool private_tab = opener->private_tab;
    const int id = next_id_;
    const int space = opener->workspace;
    new_tab("aura://newtab", private_tab,
            foreground && profile.foreground_links && space == workspace, false, space);
    auto target = tab(id);
    if (!target)
        return nullptr;
    target->ntp = false;
    target->popup_pending = true;
    target->loading = true;
    target->page.url = url.empty() ? "about:blank" : url;
    const auto bounds = ui->web_bounds();
    target->page.view_width = std::max(1, int(std::lround(bounds.width * ui_scale)));
    target->page.view_height = std::max(1, int(std::lround(bounds.height * ui_scale)));
    target->page.invalidate = [this] { changed(); };
    target->page.repaint = [this, id] {
        if (current() && current()->id == id)
            window->invalidate(ui->web_bounds());
    };
    pending_popups_[{source, popup_id}] = id;
    return target;
}
void BrowserController::abort_popup(int source, int popup_id) {
    const auto it = pending_popups_.find({source, popup_id});
    if (it == pending_popups_.end())
        return;
    if (auto target = tab(it->second)) {
        target->popup_pending = false;
        target->page.closed = true;
        target->client = nullptr;
    }
    pending_popups_.erase(it);
    changed();
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
                                       std::function<void()> action, bool sensitive) {
    BrowserDialog dialog;
    dialog.sensitive = sensitive;
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
    std::filesystem::create_directories(downloads_directory());
    open_directory(downloads_directory());
}
void BrowserController::open_directory(const std::filesystem::path &directory) {
    const auto path = utf8_path(directory);
#ifdef _WIN32
    const auto folder = path_from_utf8(path).wstring();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window->handle(), L"open", folder.c_str(), nullptr,
                                                nullptr, SW_SHOWNORMAL)) <= 32)
        ui->toast(tr("Could not open the folder"));
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
        ui->toast(tr("Could not open the folder"));
#endif
}
} // namespace aura
