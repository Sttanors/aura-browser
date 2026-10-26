#pragma once
#include "common/text_editor.hpp"
#include "core/browser/devtools.hpp"
#include "core/browser/duplicate_tabs.hpp"
#include "core/browser/extensions.hpp"
#include "core/browser/filter_lists.hpp"
#include "core/browser/i18n.hpp"
#include "core/browser/imports.hpp"
#include "core/browser/passwords.hpp"
#include "core/browser/process_tasks.hpp"
#include "core/browser/profile.hpp"
#include "core/browser/profiles.hpp"
#include "core/browser/protection.hpp"
#include "core/browser/reading_list.hpp"
#include "core/browser/screenshot.hpp"
#include "core/browser/tab_tree.hpp"
#include "core/browser/updates.hpp"
#include "core/cef/diagnostic_client.hpp"
#include "include/cef_context_menu_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_jsdialog_handler.h"
#include "include/cef_task_manager.h"
#include "platform/browser_window.hpp"
#include <atomic>
#include <deque>
#include <map>
#include <set>
namespace aura {
std::string password_origin(const std::string &url);
std::string permission_origin(const std::string &url);
class BrowserUi;
struct SiteDataState {
    std::string origin, error, message;
    int tab = 0, container = 0, pending = 0;
    bool private_mode = false, clearing = false;
    uint64_t serial = 0, usage = 0, quota = 0, cookies = 0;
    std::map<std::string, uint64_t> types;
    std::chrono::steady_clock::time_point started{};
};
struct ScreenshotState {
    int tab = 0, job_tab = 0, mode = 0, width = 0, height = 0, area_view_width = 0,
        area_view_height = 0;
    int surface_width = 0, surface_height = 0, restore_context = 0;
    double scroll_x = 0, scroll_y = 0;
    double client_width = 0, client_height = 0, area_zoom = 0;
    uint64_t navigation = 0, serial = 0, job = 0;
    bool pending = false, native_running = false, selecting = false, reduced = false,
         emulating = false;
    std::string source, error;
    std::chrono::steady_clock::time_point started{};
};
struct Tab {
    int id = 0;
    int workspace = 0;
    int group = 0;
    int container = 0;
    int parent = 0; // Window-local tab ID; zero is a root.
    bool tree_collapsed = false;
    uint64_t navigation_serial = 0;
    bool sleeping = false, sleep_pending = false, media_capture = false;
    bool media_video = false, media_audio = false;
    std::set<std::string> permission_origins;
    bool reader_active = false, reader_pending = false, reading_pending = false;
    uint64_t reading_generation = 0;
    int reader_context = 0;
    std::string reader_ui = "null";
    std::string selected_text;
    std::chrono::steady_clock::time_point reader_started{};
    std::map<std::string, bool> audio_contexts;
    std::chrono::steady_clock::time_point inactive_since = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point sleep_retry_after{};
    DiagnosticState page;
    std::shared_ptr<BrowserWindow> cef_parent; // Keep the immutable OSR native parent alive.
    CefRefPtr<DiagnosticClient> client;
    CefRefPtr<CefBrowser> browser;
    PaintBuffer favicon;
    bool ntp = true, private_tab = false, pinned = false, loading = false, back = false,
         forward = false, muted = false, closing = false;
    double progress = 0, zoom = 0;
    std::string icon_url, pending_url, pending_download_url;
    bool site_tools_ready = false, process_ended = false;
    bool popup_pending = false, creation_pending = false;
    std::chrono::steady_clock::time_point context_wait_since{};
    bool remember_on_close = false;
    CefRefPtr<DevToolsSession> devtools;
    std::map<int, std::string> protection_contexts;
    std::string protection_script;
    std::set<int> spelling_contexts;
    std::string spelling_script;
    bool spelling_supported = false;
    uint64_t spelling_generation = 0;
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
    int source_tab = 0, container = 0;
    uint64_t source_window = 0;
    bool private_mode = false;
    std::string name, path, url, sha256, checksum_status;
    long long speed = 0;
    uint64_t checksum_generation = 0;
    int percent = 0;
    long long received = 0, total = 0;
    bool complete = false, cancelled = false, paused = false, interrupted = false;
    int interrupt_reason = 0;
    CefRefPtr<CefDownloadItemCallback> callback;
};
struct BrowserDialog {
    int tab = 0;
    std::string origin, message, accept_label, context;
    bool prompt = false, before_unload = false;
    TextEditor input;
    CefRefPtr<CefJSDialogCallback> callback;
    std::function<void(bool, const std::string &)> action;
    std::function<void(bool)> user_decision;
    std::vector<SitePermission> permission_rules;
    bool permission_private = false;
    bool container_route = false;
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
    std::string shortcut;
};
class BrowserController;
struct WindowRequest {
    bool private_mode = false;
    std::optional<SavedWindow> saved;
    std::string url;
};
struct DuplicateMerge {
    bool active = false;
    uint64_t requester = 0, generation = 0;
    DuplicateTab keep;
    std::deque<DuplicateTab> pending;
    std::optional<DuplicateTab> checking, waiting;
    size_t closed = 0, skipped = 0;
    std::string status;
    std::chrono::steady_clock::time_point started{};
};
struct TabDrop {
    uint64_t source = 0, target = 0;
    int tab = 0, before = 0;
    Rect marker;
};
struct BrowserState {
    explicit BrowserState(const std::filesystem::path &path);
    Profile profile;
    DuplicateMerge duplicate_merge;
    TabDrop tab_drop;
    std::vector<std::string> private_spelling_words;
    bool spelling_resources_ready = false;
    ReadingList reading_list;
    std::vector<Download> downloads;
    std::vector<std::filesystem::path> download_paths;
    CefRefPtr<CefTaskManager> task_manager;
    uint64_t next_window_uid = 0;
    uint64_t profile_revision = 0;
    ProfileCatalog profiles;
    std::vector<SitePermission> private_site_permissions;
    std::vector<SiteZoom> private_site_zooms;
    bool private_permissions_initialized = false;
    ExtensionManager extensions;
    UpdateManager updates;
    BrowserImporter importer;
    PasswordManager passwords;
    std::unique_ptr<FilterLists> filters;
    std::chrono::steady_clock::time_point filters_after =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::shared_ptr<RequestPolicy> policy = std::make_shared<RequestPolicy>();
    CefRefPtr<CefRequestContext> private_context;
    std::map<int, CefRefPtr<CefRequestContext>> container_contexts;
    std::set<int> container_contexts_ready;
    std::vector<BrowserController *> windows;
    std::deque<WindowRequest> pending_windows;
    std::vector<SavedWindow> previous_windows;
    bool previous_restored = false, unclean_start = false, recovery_pending = false;
    bool exiting = false, session_frozen = false, cleanup_started = false;
};
class BrowserController {
    std::shared_ptr<BrowserState> state_;

  public:
    BrowserController(std::filesystem::path profile_path, std::filesystem::path font, double scale,
                      bool accelerated = false, std::shared_ptr<BrowserState> shared = {},
                      bool private_window = false);
    ~BrowserController();
    std::shared_ptr<void> callback_lifetime = std::make_shared<int>(0);
    Profile &profile;
    SiteDataState site_data;
    ScreenshotState screenshot;
    void capture_screenshot(CaptureRect relative = {});
    void cancel_screenshot();
    void open_site_data();
    void refresh_site_data(const std::string &origin = "");
    void clear_site_data(bool cookies);
    bool private_window = false;
    std::shared_ptr<BrowserState> state() const {
        return state_;
    }
    void new_window(bool private_mode = false, std::string url = "");
    void restore_window(const SavedWindow &saved);
    void restore_previous_session();
    void request_exit(bool force = false);
    std::shared_ptr<BrowserWindow> window;
    std::unique_ptr<BrowserUi> ui;
    std::vector<std::unique_ptr<Tab>> tabs;
    std::vector<Download> &downloads;
    const uint64_t window_uid;
    std::deque<BrowserDialog> dialogs;
    std::vector<MenuEntry> menu;
    CefRefPtr<CefRunContextMenuCallback> menu_callback;
    std::function<void(int)> menu_action;
    int menu_x = 0, menu_y = 0, menu_width = 240;
    int active = 0, width = 1360, height = 900;
    int workspace = 0;
    std::array<int, 2> split_tabs{};
    bool split_enabled() const {
        return split_tabs[0] && split_tabs[1];
    }
    bool tab_visible(int id) const;
    void toggle_split();
    void end_split();
    void resize_pages();
    std::vector<int> visible_tabs(bool include_closed = false) const;
    void cycle_tab(int direction);
    void select_visible_tab(int number);
    std::string workspace_name() const;
    void switch_workspace(int id);
    void remove_workspace(int id);
    void move_tab_to_workspace(int tab_id, int workspace_id);
    void set_tab_group(int tab_id, int group_id, bool branch = false);
    void remove_group(int group_id);
    CefRefPtr<CefRequestContext> request_context(int container, bool private_tab = false);
    bool container_in_use(int id) const;
    void remove_container(int id);
    bool route_container(int tab_id, const std::string &url, const std::string &method = "GET");
    bool save_container_rule(const std::string &host, int container, bool subdomains);
    std::string permission_value(const Tab &, const std::string &requesting,
                                 const std::string &id) const;
    std::string permission_site(const Tab &, const std::string &requesting,
                                const std::string &id) const;
    bool change_site_permission(const std::string &origin, int container, bool private_mode,
                                const std::string &id, const std::string &value,
                                bool native = true);
    void reset_site_permissions(const std::string &origin, int container, bool private_mode);
    CefRefPtr<CefRequestContext> permission_context(int container, bool private_mode) const;
    std::string native_permission_value(const std::string &origin, int container, bool private_mode,
                                        const std::string &id) const;
    void open_profile(int id);
    std::string profile_name() const;
    void update_window_title();
    double scale = 1, ui_scale = 1;
    bool quitting = false;
    int find_matches = 0, find_active = 0, tools_open = 0;
    std::map<std::string, int> audit;
    ExtensionManager &extensions;
    UpdateManager &updates;
    BrowserImporter &importer;
    PasswordManager &passwords;
    FilterLists &filter_lists() {
        return *state_->filters;
    }
    void page_login(bool fill, size_t index = 0);
    bool install_update_on_exit = false, restart_on_exit = false;
    void restart();
    void refresh_extension_state();
    std::shared_ptr<ExtensionPopup> extension_popup;
    std::filesystem::path resources;
    std::shared_ptr<RequestPolicy> request_policy;
    Tab *current();
    Tab *tab(int id);
    void start(const std::string &initial, bool private_mode, bool resume_once = false);
    void tick();
    void changed(bool persistent = false);
    void new_tab(std::string url = "aura://newtab", bool private_tab = false,
                 bool foreground = true, bool pinned = false, int workspace = -1, int group = -1,
                 int container = -1, int parent = 0);
    void activate(int index, bool reveal_group = true);
    void close_tab(int index, bool preserve_window = false);
    std::vector<DuplicateTab> duplicate_tabs(bool include_closing = false) const;
    void merge_duplicates(const DuplicateGroup &, DuplicateRef keep);
    void tick_duplicate_merge();
    void stop_duplicate_merge(const std::string &reason = "Remaining duplicates were kept.");
    void probe_duplicate_media(DuplicateTab, uint64_t generation);
    void show_duplicate_tab(DuplicateRef);
    void reopen();
    void move_tab(int from, int to);
    std::string tab_transfer_problem(int id, const BrowserController &target) const;
    bool move_tab_to_window(int id, BrowserController &target, int before = 0);
    void tab_drag(int id, int x, int y, bool release = false);
    void cancel_tab_drag();
    void bind_tab_callbacks(Tab &);
    void bind_site_tool_events(int id);
    std::vector<TreeTab> tree_nodes() const;
    std::vector<TreeRow> tree_rows(bool hide_collapsed = true) const;
    void normalize_tree();
    void detach_tree_tab(int id);
    void toggle_tree(int id);
    void reveal_tree(int id);
    void navigate(std::string input);
    void back();
    void forward();
    void reload(bool ignore_cache = false);
    void zoom(double delta, bool reset = false);
    void apply_site_zoom(Tab &tab, const std::string &url = {});
    void bookmark();
    void pin();
    void mute();
    bool can_sleep(const Tab &tab, bool allow_visible = false) const;
    void sleep_tab(int id, bool manual = true);
    void save_reading_article();
    void open_reading_article(int id);
    void toggle_reader();
    void reader_event(Tab &, CefRefPtr<CefDictionaryValue>);
    void wake_tab(int id);
    void devtools();
    void find_text(const std::string &text, bool next = false, bool backwards = false);
    void close_menu();
    void answer_dialog(bool accept);
    void request_quit(bool force = false);
    bool finished();
    void prepare_shutdown();
    void detach_window();
    void save_session();
    std::vector<SavedWindow> capture_session() const;
    void restore_named_session(int id);
    int created_count() const {
        return created_;
    }
    int closed_count() const {
        return closed_;
    }
    std::string report() const;
    void defer(std::function<void()> action);
    void browser_created(int id, CefRefPtr<CefBrowser> browser);
    void browser_closed(int id);
    void page_changed(int id, bool history = false);
    void open_from_page(int source, std::string url, bool foreground);
    Tab *prepare_popup(int source, int popup_id, const std::string &url, bool foreground);
    void abort_popup(int source, int popup_id);
    void update_favicon(int id, const std::string &url);
    void download_updated(CefRefPtr<CefDownloadItem> item,
                          CefRefPtr<CefDownloadItemCallback> callback, int source_tab);
    void start_download(CefRefPtr<CefDownloadItem> item,
                        CefRefPtr<CefBeforeDownloadCallback> callback, const std::string &suggested,
                        int source_tab);
    void download_action(std::size_t index, bool cancel);
    std::vector<std::size_t> visible_downloads() const;
    bool download_visible(const Download &) const;
    void retry_download(std::size_t index);
    void begin_pending_download(int tab_id);
    std::vector<ProcessTask> process_tasks(bool *available = nullptr) const;
    void end_process(const ProcessTask &);
    void verify_download(std::size_t index);
    void open_folder();
    void open_directory(const std::filesystem::path &directory);
    std::string tr(std::string_view english) const;
    void apply_settings(bool update_zoom = false, bool propagate = true);
    std::string spelling_status() const;
    void prepare_spelling_resources();
    void configure_spelling(CefRefPtr<CefRequestContext> context);
    void setup_spelling(Tab &);
    void update_spelling(Tab &, bool future = true);
    std::vector<std::string> spelling_words(bool private_mode) const;
    bool change_spelling_word(const std::string &word, bool add, bool private_mode);
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
                               const std::string &top = "", int container = 0,
                               bool private_mode = false);

  private:
    std::vector<std::shared_ptr<ExtensionPopup>> closing_popups_;
    std::map<std::string, PaintBuffer> extension_icons_;
    int next_id_ = 1, created_ = 0, closed_ = 0;
    CefRefPtr<CefRequestContext> &private_context_;
    std::vector<SavedTab> recently_closed_;
    std::map<int, int> workspace_selection_;
    std::map<std::pair<int, int>, int> pending_popups_;
    std::vector<std::function<void()>> pending_;
    std::string quit_snapshot_;
    std::chrono::steady_clock::time_point save_at_ = std::chrono::steady_clock::now();
    bool needs_save_ = false;
    uint64_t password_generation_seen_ = 0;
    bool restoring_ = false, quit_confirmed_ = false, cleanup_started_ = false;
    bool background_updates_ = false;
    int data_tasks_ = 0;
    int browser_frame_rate_ = 60;
    void ensure_browser(Tab &tab, const std::string &url);
    void finish_download(CefRefPtr<CefBeforeDownloadCallback> callback, std::string name,
                         uint32_t id);
};
} // namespace aura
