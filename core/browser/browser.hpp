#pragma once
#include "common/text_editor.hpp"
#include "core/browser/devtools.hpp"
#include "core/browser/extensions.hpp"
#include "core/browser/i18n.hpp"
#include "core/browser/imports.hpp"
#include "core/browser/passwords.hpp"
#include "core/browser/profile.hpp"
#include "core/browser/protection.hpp"
#include "core/browser/updates.hpp"
#include "core/cef/diagnostic_client.hpp"
#include "include/cef_context_menu_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_jsdialog_handler.h"
#include "platform/browser_window.hpp"
#include <atomic>
#include <deque>
#include <map>
namespace aura {
std::string password_origin(const std::string &url);
class BrowserUi;
struct Tab {
    int id = 0;
    uint64_t navigation_serial = 0;
    DiagnosticState page;
    CefRefPtr<DiagnosticClient> client;
    CefRefPtr<CefBrowser> browser;
    PaintBuffer favicon;
    bool ntp = true, private_tab = false, pinned = false, loading = false, back = false,
         forward = false, muted = false, closing = false;
    double progress = 0, zoom = 0;
    std::string icon_url, pending_url;
    bool site_tools_ready = false;
    CefRefPtr<DevToolsSession> devtools;
    std::map<int, std::string> protection_contexts;
    std::string protection_script;
};
struct ExtensionPopup {
    DiagnosticState page;
    CefRefPtr<DiagnosticClient> client;
    CefRefPtr<CefBrowser> browser;
    std::string extension_id, name, url;
    int desired_width = 360, desired_height = 400;
    Rect bounds, content;
};
struct Download {
    uint32_t id = 0;
    std::string name, path;
    int percent = 0;
    long long received = 0, total = 0;
    bool complete = false, cancelled = false, paused = false, interrupted = false;
    int interrupt_reason = 0;
    CefRefPtr<CefDownloadItemCallback> callback;
};
struct BrowserDialog {
    int tab = 0;
    std::string origin, message;
    bool prompt = false, before_unload = false;
    TextEditor input;
    CefRefPtr<CefJSDialogCallback> callback;
    std::function<void(bool, const std::string &)> action;
    bool owned = false, permission = false, sensitive = false;
    uint64_t permission_id = ~uint64_t{0};
};
struct RequestPolicy {
    std::shared_ptr<Protection> protection;
    std::atomic<bool> do_not_track{false}, send_referrer{true};
};
struct MenuEntry {
    int id = 0;
    std::string label;
    bool enabled = true;
};
class BrowserController {
  public:
    BrowserController(std::filesystem::path profile_path, std::filesystem::path font, double scale,
                      bool accelerated = false);
    ~BrowserController();
    Profile profile;
    std::unique_ptr<BrowserWindow> window;
    std::unique_ptr<BrowserUi> ui;
    std::vector<std::unique_ptr<Tab>> tabs;
    std::vector<Download> downloads;
    std::deque<BrowserDialog> dialogs;
    std::vector<MenuEntry> menu;
    CefRefPtr<CefRunContextMenuCallback> menu_callback;
    std::function<void(int)> menu_action;
    int menu_x = 0, menu_y = 0;
    int active = 0, width = 1360, height = 900;
    double scale = 1, ui_scale = 1;
    bool quitting = false;
    int find_matches = 0, find_active = 0, tools_open = 0;
    std::map<std::string, int> audit;
    ExtensionManager extensions;
    UpdateManager updates;
    BrowserImporter importer;
    PasswordManager passwords;
    void page_login(bool fill, size_t index = 0);
    bool install_update_on_exit = false, restart_on_exit = false;
    void restart();
    void refresh_extension_state();
    std::shared_ptr<ExtensionPopup> extension_popup;
    std::filesystem::path resources;
    std::shared_ptr<RequestPolicy> request_policy = std::make_shared<RequestPolicy>();
    Tab *current();
    Tab *tab(int id);
    void start(const std::string &initial, bool private_mode, bool resume_once = false);
    void tick();
    void changed(bool persistent = false);
    void new_tab(std::string url = "aura://newtab", bool private_tab = false,
                 bool foreground = true, bool pinned = false);
    void activate(int index);
    void close_tab(int index);
    void reopen();
    void move_tab(int from, int to);
    void navigate(std::string input);
    void back();
    void forward();
    void reload(bool ignore_cache = false);
    void zoom(double delta, bool reset = false);
    void bookmark();
    void pin();
    void mute();
    void devtools();
    void find_text(const std::string &text, bool next = false, bool backwards = false);
    void close_menu();
    void answer_dialog(bool accept);
    void request_quit(bool force = false);
    bool finished();
    void prepare_shutdown();
    void save_session();
    std::string report() const;
    void defer(std::function<void()> action);
    void browser_created(int id, CefRefPtr<CefBrowser> browser);
    void browser_closed(int id);
    void page_changed(int id, bool history = false);
    void open_from_page(int source, std::string url, bool foreground);
    void update_favicon(int id, const std::string &url);
    void download_updated(CefRefPtr<CefDownloadItem> item,
                          CefRefPtr<CefDownloadItemCallback> callback);
    void start_download(CefRefPtr<CefDownloadItem> item,
                        CefRefPtr<CefBeforeDownloadCallback> callback,
                        const std::string &suggested);
    void download_action(std::size_t index, bool cancel);
    void open_folder();
    void open_directory(const std::filesystem::path &directory);
    std::string tr(std::string_view english) const;
    void apply_settings(bool update_zoom = false);
    void configure_font_preferences(CefRefPtr<CefRequestContext> context);
    void setup_site_tools(int id, std::function<void()> ready);
    void update_site_tools(Tab &tab);
    void protection_menu();
    void extension_page(size_t index, bool options);
    void open_extension_popup(size_t index);
    void close_extension_popup(bool restore_focus = true);
    void popup_created(const std::shared_ptr<ExtensionPopup> &popup, CefRefPtr<CefBrowser> browser);
    void popup_closed(const std::shared_ptr<ExtensionPopup> &popup);
    void resize_extension_popup();
    void pin_extension(size_t index);
    const PaintBuffer &extension_icon(size_t index);
    void open_new_tab(bool private_tab = false);
    std::filesystem::path downloads_directory() const;
    void clear_data(const std::string &kind);
    void confirm_action(std::string title, std::string message, std::function<void()> action,
                        bool sensitive = false);
    void cancel_permissions(int tab_id);
    void configure_permissions(CefRefPtr<CefRequestContext> context, const std::string &url = "",
                               const std::string &top = "");

  private:
    std::vector<std::shared_ptr<ExtensionPopup>> closing_popups_;
    std::map<std::string, PaintBuffer> extension_icons_;
    int next_id_ = 1, created_ = 0, closed_ = 0;
    CefRefPtr<CefRequestContext> private_context_;
    std::vector<SavedTab> recently_closed_;
    std::vector<std::filesystem::path> download_paths_;
    std::vector<std::function<void()>> pending_;
    std::string quit_snapshot_;
    std::chrono::steady_clock::time_point save_at_ = std::chrono::steady_clock::now();
    bool needs_save_ = false;
    bool restoring_ = false, quit_confirmed_ = false, cleanup_started_ = false;
    bool background_updates_ = false;
    int data_tasks_ = 0;
    int browser_frame_rate_ = 60;
    void ensure_browser(Tab &tab, const std::string &url);
    void finish_download(CefRefPtr<CefBeforeDownloadCallback> callback, std::string name);
};
} // namespace aura
