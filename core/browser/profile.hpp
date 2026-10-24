#pragma once
#include "core/browser/hotkeys.hpp"
#include "core/browser/permissions.hpp"
#include "core/browser/preferences.hpp"
#include "core/browser/site_zoom.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace aura {
std::string navigation_url(std::string input, const std::string &engine = "duckduckgo");
std::string search_engine_name(const std::string &engine);
std::string url_host(const std::string &url);
struct Visit {
    std::string url, title;
    long long time = 0;
    int folder = 0;
    std::vector<std::string> tags;
};
struct BookmarkFolder {
    int id = 0;
    std::string name;
};
std::optional<std::vector<std::string>> bookmark_tags(const std::string &text);
bool valid_bookmark_address(const std::string &value);
std::string bookmarks_html(const std::vector<Visit> &, const std::vector<BookmarkFolder> &,
                           size_t *skipped = nullptr);
struct Shortcut {
    std::string title, url;
};
struct SavedTab {
    std::string url;
    bool pinned = false;
    int workspace = 0;
    bool workspace_active = false;
    int group = 0;
    int container = 0;
};
struct SavedWindow {
    std::vector<SavedTab> tabs;
    int active = 0;
    int workspace = 0;
    int split_left = -1, split_right = -1;
};
struct Workspace {
    int id = 0;
    std::string name;
};
struct TabGroup {
    int id = 0, workspace = 0;
    std::string name;
    int color = 0;
    bool collapsed = false;
};
struct SiteContainer {
    int id = 0;
    std::string name;
    int color = 0;
};
struct NamedSession {
    int id = 0;
    std::string name;
    long long created = 0, updated = 0;
    uint64_t revision = 1;
    std::vector<SavedWindow> windows;
    std::vector<Workspace> workspaces;
    std::vector<TabGroup> groups;
};
bool valid_named_session(const NamedSession &);
void remove_container_tabs(SavedWindow &window, int container);
class Profile : public BrowserPreferences {
  public:
    explicit Profile(std::filesystem::path directory);
    std::filesystem::path directory;
    std::string profiles_root;
    std::vector<SitePermission> site_permissions;
    std::vector<SiteZoom> site_zooms;
    std::vector<HotkeyOverride> hotkeys;
    std::vector<Visit> history, bookmarks;
    std::vector<BookmarkFolder> bookmark_folders;
    int next_bookmark_folder = 1;
    bool has_bookmark_folder(int id) const;
    int bookmark_folder_id(std::string name) const;
    std::string bookmark_folder_label(int id) const;
    int add_bookmark_folder(std::string name, int assigned_id = 0);
    bool rename_bookmark_folder(int id, std::string name);
    bool remove_bookmark_folder(int id);
    bool edit_bookmark(const std::string &old_url, std::string title, std::string url, int folder,
                       const std::string &tags);
    std::vector<SavedTab> session;
    std::vector<SavedWindow> windows;
    std::vector<NamedSession> named_sessions;
    int next_named_session = 1;
    const NamedSession *named_session(int id) const;
    int save_named_session(std::string name, std::vector<SavedWindow> windows, int replace = 0);
    bool rename_named_session(int id, std::string name);
    bool remove_named_session(int id);
    bool named_session_available(int id) const;
    std::optional<std::vector<SavedWindow>> prepare_named_session(int id);
    std::vector<Workspace> workspaces{{0, "Personal space"}};
    bool has_workspace(int id) const;
    std::string workspace_label(int id) const;
    int add_workspace(std::string name);
    bool rename_workspace(int id, std::string name);
    bool remove_workspace(int id);
    std::vector<SiteContainer> containers{{0, "Default"}};
    std::vector<int> container_trash;
    const SiteContainer *container(int id) const;
    std::string container_label(int id) const;
    int add_container(std::string name, int color = 0);
    bool rename_container(int id, std::string name, int color);
    bool remove_container(int id);
    bool cleanup_container_data();
    std::filesystem::path container_data_directory(int id) const;
    std::vector<TabGroup> groups;
    TabGroup *group(int id);
    const TabGroup *group(int id) const;
    int add_group(int workspace, std::string name, int color = 0);
    bool rename_group(int id, std::string name);
    bool remove_group(int id);
    std::vector<Shortcut> shortcuts = {{"Wikipedia", "https://www.wikipedia.org/"},
                                       {"GitHub", "https://github.com/"},
                                       {"MDN Web Docs", "https://developer.mozilla.org/"}};
    std::optional<std::array<std::array<uint32_t, 5>, 2>> image_palette;
    bool add_shortcut(std::string title, std::string url);
    bool edit_shortcut(size_t index, std::string title, std::string url);
    void move_shortcut(size_t from, size_t to);
    void remove_shortcut(size_t index);
    int active = 0;
    bool session_clean = true;
    // Set only when malformed on-disk settings were preserved for recovery.
    bool recovered_profile = false;
    void prune_history();
    void load();
    void save() const;
    void visit(std::string url, std::string title, bool private_tab);
    bool bookmarked(const std::string &url) const;
    void toggle_bookmark(std::string url, std::string title);
    std::vector<Visit> suggest(const std::string &query, std::size_t limit = 7) const;
};
} // namespace aura
