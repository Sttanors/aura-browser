#pragma once
#include "core/browser/preferences.hpp"
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
};
struct Shortcut {
    std::string title, url;
};
struct SavedTab {
    std::string url;
    bool pinned = false;
    int workspace = 0;
    bool workspace_active = false;
    int group = 0;
};
struct SavedWindow {
    std::vector<SavedTab> tabs;
    int active = 0;
    int workspace = 0;
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
class Profile : public BrowserPreferences {
  public:
    explicit Profile(std::filesystem::path directory);
    std::filesystem::path directory;
    std::vector<Visit> history, bookmarks;
    std::vector<SavedTab> session;
    std::vector<SavedWindow> windows;
    std::vector<Workspace> workspaces{{0, "Personal space"}};
    bool has_workspace(int id) const;
    std::string workspace_label(int id) const;
    int add_workspace(std::string name);
    bool rename_workspace(int id, std::string name);
    bool remove_workspace(int id);
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
