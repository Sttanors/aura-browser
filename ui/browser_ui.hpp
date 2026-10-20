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
    enum Panel { None, Bookmarks, History, Downloads, Settings, Extensions, Shortcuts, Updates, Import };
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
    Panel visible_panel() const {
        return panel_;
    }
    std::string start_page_report() const;
    std::string shortcuts_report() const;
    std::string settings_report() const;
    std::string updates_report() const;
    std::string import_report() const;
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
    sk_sp<SkTypeface> regular_, medium_;
    sk_sp<SkImage> web_backdrop_;
    int backdrop_x_ = 0, backdrop_y_ = 0;
    int width_ = 1360, height_ = 900, hover_x_ = -1, hover_y_ = -1, suggestion_ = -1,
        panel_scroll_ = 0, tab_scroll_ = 0, drag_tab_ = -1, drag_y_ = 0;
    std::vector<Rect> focus_targets_;
    int keyboard_focus_ = -1, focus_scope_ = -1, visible_active_id_ = -1,
        visible_active_index_ = -1;
    void focus_target(Rect rect);
    int settings_section_ = 0, settings_scroll_ = 0;
    TextEditor settings_search_, settings_text_, extension_input_;
    bool extension_input_focus_ = false;
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
    void choose_file(bool for_import);
    bool picker_import_ = false;
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
