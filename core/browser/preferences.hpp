#pragma once
#include <span>
#include <string>
#include <variant>
#include <vector>
namespace aura {
struct BrowserPreferences {
    std::string language = "en", search_engine = "duckduckgo", startup_page = "newtab",
                home_page = "https://www.wikipedia.org/", new_tab_page = "newtab",
                site_language = "en-US,en", close_selection = "neighbor", font_family = "sans",
                download_directory;
    std::string accent = "theme", ntp_width = "normal", ntp_background = "aura", ntp_title,
                ntp_subtitle, ntp_link_columns = "auto", ntp_link_style = "cards", ntp_image_path,
                ntp_image_fit = "cover";
    int ntp_zoom = 100, ntp_image_dim = 35;
    bool ntp_image_theme = true;
    bool ntp_logo = true, ntp_subtitle_visible = true, ntp_link_icons = true;
    int ui_zoom = 100;
    bool remember_site_zoom = true;
    bool spellcheck = true;
    std::string spellcheck_languages = "both";
    bool tree_tabs = true;
    std::string tab_layout = "vertical";
    int tab_sleep_minutes = 0;
    int split_percent = 50;
    int reader_font_size = 20, reader_width = 720;
    std::string reader_font = "serif";
    std::string translation_provider = "google", translation_language = "interface";
    std::string tab_sleep_exceptions;
    std::string tab_sleep_mode = "freeze";
    int theme = 0, default_zoom = 100, default_font_size = 16, minimum_font_size = 0,
        history_limit = 1000, history_days = 0, suggestion_limit = 6;
    bool restore = true, record_history = true, address_suggestions = true, quick_links = true,
         show_tab_host = true, reduce_motion = false, open_downloads = true,
         foreground_links = true, tabs_at_end = true, confirm_quit = false, middle_close = true,
         wheel_switch = false, mute_new_tabs = false, quit_last_tab = false, show_favicons = true,
         show_status = true, ntp_clock = false, clock_24 = true, ntp_greeting = true,
         javascript = true, images = true, remote_fonts = true, local_storage = true,
         resize_textareas = true, tab_to_links = true, image_fit = true, suggest_history = true,
         suggest_bookmarks = true, clear_history_exit = false, clear_cookies_exit = false,
         do_not_track = false, send_referrer = true, block_popups = false, allow_dialogs = true,
         ask_download_name = false, date_downloads = false, notify_downloads = true,
         close_downloads = false;
    bool adblock = true, cosmetic_filtering = true;
    bool check_updates = true, filter_updates = true;
    std::string adblock_exceptions, darkreader_mode = "off", darkreader_exceptions;
    int darkreader_brightness = 100, darkreader_contrast = 100, darkreader_sepia = 0;
    std::string notifications = "block", geolocation = "block", camera = "block",
                microphone = "block", clipboard = "block", midi = "block", sensors = "block";
};
enum class SettingKind { Toggle, Choice, Number, Text, Themes };
struct SettingChoice {
    std::string value, label;
};
using PreferenceMember = std::variant<bool BrowserPreferences::*, int BrowserPreferences::*,
                                      std::string BrowserPreferences::*>;
struct SettingSpec {
    std::string id, section, title, description;
    PreferenceMember member;
    SettingKind kind = SettingKind::Toggle;
    std::vector<SettingChoice> choices;
    int minimum = 0, maximum = 0, step = 1;
    SettingSpec(std::string key, std::string group, std::string name, std::string help,
                PreferenceMember field, SettingKind type = SettingKind::Toggle,
                std::vector<SettingChoice> options = {}, int low = 0, int high = 0,
                int increment = 1)
        : id(std::move(key)), section(std::move(group)), title(std::move(name)),
          description(std::move(help)), member(field), kind(type), choices(std::move(options)),
          minimum(low), maximum(high), step(increment) {}
};
std::span<const SettingSpec> settings_catalog();
const SettingSpec *find_setting(const std::string &id);
std::string setting_value(const BrowserPreferences &preferences, const SettingSpec &setting);
bool set_setting(BrowserPreferences &preferences, const SettingSpec &setting,
                 const std::string &value);
bool valid_home_address(const std::string &value);
} // namespace aura
