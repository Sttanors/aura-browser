#pragma once
#include "common/text_editor.hpp"
#include "core/browser/browser.hpp"
#include "include/core/SkFont.h"
#include "include/core/SkTypeface.h"
#include "ui/wallpaper.hpp"
#include <chrono>
namespace aura {
class BrowserUi {
  public:
    enum Panel {
        None,
        Bookmarks,
        History,
        Downloads,
        Settings,
        Extensions,
        Shortcuts,
        Updates,
        Import,
        Passwords,
        Filters,
        Workspaces,
        Groups,
        Containers,
        Profiles,
        Commands,
        Translation
    };
    BrowserUi(BrowserController &owner, const std::filesystem::path &font);
    ~BrowserUi();
    void draw(SkCanvas &canvas);
    bool draw_web_frame(SkCanvas &canvas);
    void resize(int width, int height);
    void pointer(const PointerInput &input);
    bool key(const CefKeyEvent &event);
    void text(const std::string &value);
    bool tick();
    void focus_address(bool select = true);
    void toast(std::string message);
    void panel(Panel value);
    Rect web_bounds() const;
    Rect page_bounds(const Tab &tab) const;
    Rect split_header(const Tab &tab) const;
    Rect split_divider() const;
    std::string split_report() const;
    Panel visible_panel() const {
        return panel_;
    }
    std::string start_page_report() const;
    std::string shortcuts_report() const;
    std::string settings_report() const;
    std::string updates_report() const;
    std::string import_report() const;
    std::string passwords_report() const;
    std::string filters_report() const;
    std::string workspaces_report() const;
    void open_workspaces(int move_tab = -1);
    void open_groups(int tab_id = -1);
    std::string groups_report() const;
    void open_containers(int tab_id = -1);
    std::string containers_report() const;
    void open_profiles();
    std::string profiles_report() const;
    void open_commands();
    std::string commands_report() const;
    void open_translation(bool selection = false, int source_tab = 0);
    std::string translation_report() const;
    std::string menu_report() const;
    std::string sidebar_report() const;
    void passwords_locked() {
        clear_password_fields();
        password_mode_ = 0;
        password_selected_ = -1;
    }
    std::string extensions_report() const;
    std::string extension_popup_report() const;
    std::string extension_toolbar_report() const;
    std::vector<std::pair<size_t, Rect>> extension_buttons() const;
    void layout_extension_popup();
    void show_protection_settings();
    std::string status;
    bool editing = false, find_open = false;
    TextEditor address, find;

  private:
    BrowserController &app_;
    int translation_tab_ = 0;
    uint64_t translation_serial_ = 0;
    bool translation_selection_ = false;
    std::string translation_text_, translation_source_;
    void draw_translation(SkCanvas &, Rect);
    void translation_pointer(Rect, int, int);
    void send_translation();
    std::string translation_problem() const;
    struct CommandResult {
        std::string kind, id, title, detail, shortcut;
        int tab = 0, score = 0;
        bool enabled = true;
    };
    TextEditor command_query_;
    int command_selected_ = 0, command_scroll_ = 0;
    std::vector<CommandResult> command_results() const;
    void draw_commands(SkCanvas &, Rect);
    void commands_pointer(Rect, int, int);
    void run_command(int);
    void move_command(int);
    bool split_drag_ = false;
    TextEditor profile_input_;
    bool profile_input_focus_ = false, profiles_archived_ = false;
    int profile_edit_ = -1, profiles_scroll_ = 0;
    std::string profile_error_;
    void save_profile();
    void draw_profiles(SkCanvas &, Rect);
    void profiles_pointer(Rect, int, int);
    std::vector<int> listed_profiles() const;
    TextEditor container_input_;
    bool container_input_focus_ = false;
    int container_edit_ = -1, container_target_ = -1, container_color_ = 0, container_scroll_ = 0;
    std::string container_error_;
    void save_container();
    void draw_containers(SkCanvas &, Rect);
    void containers_pointer(Rect, int, int);
    void draw_page(SkCanvas &, const Tab &, Rect);
    void draw_split(SkCanvas &);
    std::vector<int> sidebar_rows() const;
    TextEditor group_input_;
    bool group_input_focus_ = false;
    int group_edit_ = -1, group_target_ = -1, group_color_ = 0, group_scroll_ = 0;
    std::string group_error_;
    void save_group();
    void draw_groups(SkCanvas &, Rect);
    void groups_pointer(Rect, int, int);
    TextEditor workspace_input_;
    bool workspace_input_focus_ = false;
    int workspace_edit_ = -1, workspace_move_tab_ = -1, workspace_scroll_ = 0;
    std::string workspace_error_;
    void save_workspace();
    void draw_workspaces(SkCanvas &, Rect);
    void workspaces_pointer(Rect, int, int);
    sk_sp<SkTypeface> regular_, medium_;
    sk_sp<SkImage> web_backdrop_;
    int backdrop_x_ = 0, backdrop_y_ = 0;
    int width_ = 1360, height_ = 900, hover_x_ = -1, hover_y_ = -1, suggestion_ = -1,
        panel_scroll_ = 0, tab_scroll_ = 0, drag_tab_ = -1, drag_y_ = 0;
    std::vector<Rect> focus_targets_;
    int keyboard_focus_ = -1, focus_scope_ = -1, visible_active_id_ = -1,
        visible_active_index_ = -1, visible_active_row_ = -1;
    void focus_target(Rect rect);
    int settings_section_ = 0, settings_scroll_ = 0;
    TextEditor settings_search_, settings_text_, extension_input_;
    bool extension_input_focus_ = false;
    std::array<TextEditor, 3> password_fields_;
    int password_focus_ = -1, password_mode_ = 0, password_selected_ = -1, password_edit_ = -1;
    void clear_password_fields();
    void draw_passwords(SkCanvas &, Rect);
    void passwords_pointer(Rect, int, int);
    TextEditor filter_input_;
    bool filter_input_focus_ = false, filter_pending_ = false;
    int filter_edit_ = -1;
    uint64_t filter_seen_ = ~uint64_t{0}, filter_edit_revision_ = 0, filter_submit_revision_ = 0;
    std::string filter_submitted_;
    std::vector<std::string> filter_lines_;
    void sync_filter_lines();
    void save_filter_rule();
    void draw_filters(SkCanvas &, Rect);
    void filters_pointer(Rect, int, int);
    TextEditor import_input_;
    bool import_input_focus_ = false, import_bookmarks_ = true, import_history_ = true;
    bool settings_search_focus_ = false;
    std::string settings_edit_, settings_error_;
    struct SettingsRow {
        const SettingSpec *spec = nullptr;
        std::string action, title, description;
        Rect bounds;
    };
    unsigned web_buttons_ = 0, popup_buttons_ = 0;
    CefKeyEvent last_key_;
    Panel panel_ = None;
    std::string toast_;
    std::chrono::steady_clock::time_point toast_until_{},
        last_tick_ = std::chrono::steady_clock::now();
    float focus_animation_ = 0, progress_ = 0;
    void draw_editor(SkCanvas &canvas, const TextEditor &editor, Rect rect, float size,
                     bool active);
    bool editor_key(TextEditor &editor, const CefKeyEvent &event);
    std::vector<Visit> suggestions() const;
    void commit_address();
    void dismiss();
    Rect address_bounds() const;
    Rect panel_bounds() const;
    Rect dialog_bounds() const;
    void draw_extension_popup(SkCanvas &canvas);
    bool popup_pointer(const PointerInput &input);
    void draw_extensions(SkCanvas &canvas, Rect panel);
    void extensions_pointer(Rect panel, int x, int y);
    struct StartPageLayout {
        float scale = 1;
        int scroll = 0, maximum = 0;
        Rect viewport, clock, logo, heading, subtitle, search, customize, manage, zoom_out,
            zoom_reset, zoom_in;
        std::vector<std::string> heading_lines, subtitle_lines;
        std::vector<std::pair<int, Rect>> links;
    };
    TextEditor shortcut_title_, shortcut_url_;
    int shortcut_focus_ = -1, shortcut_edit_ = -1;
    std::string shortcut_error_;
    std::optional<std::pair<size_t, Shortcut>> shortcut_undo_;
    void open_shortcuts(int index = -1);
    void save_shortcut();
    void remove_shortcut(size_t index);
    void draw_shortcuts(SkCanvas &canvas, Rect panel);
    void shortcuts_pointer(Rect panel, int x, int y);
    Wallpaper wallpaper_;
    std::string wallpaper_key_, wallpaper_error_, picker_output_;
    int picker_pid_ = -1, picker_fd_ = -1;
    void ensure_wallpaper();
    bool import_wallpaper(const std::string &path);
    void choose_wallpaper();
    void choose_file(bool for_import, bool for_filters = false);
    bool picker_import_ = false, picker_filters_ = false;
    void poll_wallpaper_picker();
    int start_scroll_ = 0, start_tab_ = -1, start_zoom_ = 100;
    StartPageLayout start_page_layout() const;
    void draw_start_page(SkCanvas &canvas);
    void zoom_start_page(int delta, bool reset = false);
    Rect settings_category_bounds(Rect panel, int index) const;
    void draw_settings(SkCanvas &canvas, Rect panel);
    void draw_updates(SkCanvas &canvas, Rect panel);
    void updates_pointer(Rect panel, int x, int y);
    void draw_import(SkCanvas &canvas, Rect panel);
    void import_pointer(Rect panel, int x, int y);
    void settings_pointer(Rect panel, int x, int y);
    int choice_columns(const SettingSpec &spec, Rect row) const;
    Rect setting_choice(const SettingSpec &spec, Rect row, int index) const;
    int settings_max_scroll(Rect panel) const;
    std::vector<SettingsRow> settings_rows(Rect panel) const;
    void save_setting_text();
};
} // namespace aura
