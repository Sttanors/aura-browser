#include "core/browser/profile.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include "core/browser/i18n.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <openssl/x509v3.h>
#include <stdexcept>
#include <toml++/toml.hpp>
namespace aura {
namespace {
std::string lower(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}
std::string encode(const std::string &text) {
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
// Validate only the authority; query/path punctuation is not a hostname.
bool bare_address(const std::string &input, bool &local) {
    if (input.find_first_of(" \t\r\n") != std::string::npos)
        return false;
    const auto authority = input.substr(0, input.find_first_of("/?#"));
    if (authority.empty())
        return false;
    std::string host, port;
    bool has_port = false, ipv6 = false;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos)
            return false;
        host = authority.substr(1, end - 1);
        auto ip = a2i_IPADDRESS(host.c_str());
        ipv6 = ip && ASN1_STRING_length(ip) == 16;
        ASN1_OCTET_STRING_free(ip);
        if (!ipv6)
            return false;
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':')
                return false;
            has_port = true;
            port = authority.substr(end + 2);
        }
    } else {
        const auto colon = authority.find(':');
        host = lower(authority.substr(0, colon));
        if (colon != std::string::npos) {
            has_port = true;
            port = authority.substr(colon + 1);
        }
        if (host.empty())
            return false;
        auto labels = host;
        if (labels.back() == '.')
            labels.pop_back();
        for (size_t begin = 0; begin < labels.size();) {
            const auto end = labels.find('.', begin);
            const auto label = labels.substr(begin, end == std::string::npos ? end : end - begin);
            if (label.empty() || label.front() == '-' || label.back() == '-' ||
                !std::all_of(label.begin(), label.end(), [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c >= 128;
                }))
                return false;
            if (end == std::string::npos)
                break;
            begin = end + 1;
            if (begin == labels.size())
                return false;
        }
        if (labels.empty())
            return false;
    }
    if (has_port) {
        unsigned value = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (error != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535)
            return false;
    }
    local = host == "localhost" || host.starts_with("127.") || host == "::1" ||
            (!ipv6 && host.find('.') == std::string::npos && has_port);
    return ipv6 || has_port || host == "localhost" || host.find('.') != std::string::npos;
}
} // namespace
std::string search_engine_name(const std::string &engine) {
    if (engine == "google")
        return "Google";
    if (engine == "bing")
        return "Bing";
    if (engine == "yandex")
        return "Yandex";
    return "DuckDuckGo";
}
std::string navigation_url(std::string input, const std::string &engine) {
    input = trim(std::move(input));
    if (input.empty() || input == "aura://newtab")
        return "aura://newtab";
    const auto name = lower(input);
    if (name.starts_with("chrome-extension://")) {
        const auto end = input.find_first_of("/?#", 19);
        const auto id = name.substr(19, end == std::string::npos ? std::string::npos : end - 19);
        if (id.size() == 32 &&
            std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; }))
            return input;
        throw std::invalid_argument("This address scheme is not supported");
    }
    if (name.starts_with("https://") || name.starts_with("http://") ||
        name.starts_with("file://") || name == "about:blank")
        return input;
    if (input.find("://") != std::string::npos || name.starts_with("javascript:") ||
        name.starts_with("data:"))
        throw std::invalid_argument("This address scheme is not supported");
    bool local = false;
    if (bare_address(input, local))
        return (local ? "http://" : "https://") + input;
    const std::string base = engine == "google"   ? "https://www.google.com/search?q="
                             : engine == "bing"   ? "https://www.bing.com/search?q="
                             : engine == "yandex" ? "https://yandex.ru/search/?text="
                                                  : "https://duckduckgo.com/?q=";
    return base + encode(input);
}
std::string url_host(const std::string &url) {
    auto start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    auto end = url.find_first_of("/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}
Profile::Profile(std::filesystem::path path) : directory(std::move(path)) {
    std::filesystem::create_directories(directory);
    load();
}
void Profile::load() {
    auto file = directory / "profile.toml";
    toml::table table;
    if (!std::filesystem::exists(file))
        return;
    try {
        table = toml::parse_file(utf8_path(file));
    } catch (const toml::parse_error &) {
        // Never overwrite damaged settings: preserve their exact bytes first.
        auto damaged = directory / "profile.toml.corrupt";
        for (int n = 1; std::filesystem::exists(damaged); ++n)
            damaged = directory / ("profile.toml.corrupt." + std::to_string(n));
        std::filesystem::rename(file, damaged);
        recovered_profile = true;
        const auto backup = directory / "profile.toml.bak";
        if (std::filesystem::exists(backup)) {
            try {
                table = toml::parse_file(utf8_path(backup));
            } catch (const toml::parse_error &) {
                // Keep a damaged backup too; defaults let the user open Aura.
            }
        }
    }
    static_cast<BrowserPreferences &>(*this) = BrowserPreferences{};
    for (const auto &setting : settings_catalog()) {
        const auto node = table[setting.id];
        std::visit(
            [&](auto member) {
                using T = std::decay_t<decltype(this->*member)>;
                if (auto value = node.value<T>()) {
                    if constexpr (std::is_same_v<T, std::string>)
                        set_setting(*this, setting, *value);
                    else if constexpr (std::is_same_v<T, bool>)
                        set_setting(*this, setting, *value ? "true" : "false");
                    else
                        set_setting(*this, setting, std::to_string(*value));
                }
            },
            setting.member);
    }
    shortcuts.clear();
    if (auto list = table["shortcuts"].as_array()) {
        for (const auto &node : *list)
            if (const auto item = node.as_table())
                add_shortcut((*item)["title"].value_or(std::string{}),
                             (*item)["url"].value_or(std::string{}));
    } else {
        const Shortcut defaults[] = {{"Wikipedia", "https://www.wikipedia.org/"},
                                     {"GitHub", "https://github.com/"},
                                     {"MDN Web Docs", "https://developer.mozilla.org/"}};
        for (int i = 0; i < 3; ++i) {
            auto url = table["ntp_link_" + std::to_string(i + 1)].value_or(defaults[i].url);
            if (!url.empty()) {
                auto title = url == defaults[i].url ? defaults[i].title : "";
                add_shortcut(std::move(title), std::move(url));
            }
        }
    }
    active = std::max(0, table["active"].value_or(0));
    workspaces = {{0, "Personal space"}};
    if (auto entries = table["workspaces"].as_array())
        for (const auto &entry : *entries) {
            if (workspaces.size() >= 32)
                break;
            if (auto row = entry.as_table()) {
                const int id = (*row)["id"].value_or(-1);
                auto name = trim((*row)["name"].value_or(std::string{}));
                if (id < 0 || id > 1000000 || name.empty() || name.size() > 120 ||
                    name.find_first_of("\r\n\t") != std::string::npos)
                    continue;
                if (id == 0)
                    workspaces.front().name = name;
                else if (!has_workspace(id))
                    workspaces.push_back({id, std::move(name)});
            }
        }
    groups.clear();
    if (auto entries = table["groups"].as_array())
        for (const auto &entry : *entries) {
            if (groups.size() >= 128)
                break;
            if (auto row = entry.as_table()) {
                int id = (*row)["id"].value_or(0), space = (*row)["workspace"].value_or(0);
                auto name = trim((*row)["name"].value_or(std::string{}));
                if (id <= 0 || id > 1000000 || group(id) || !has_workspace(space) || name.empty() ||
                    name.size() > 120 || name.find_first_of("\r\n\t") != std::string::npos)
                    continue;
                groups.push_back({id, space, std::move(name),
                                  std::clamp((*row)["color"].value_or(0), 0, 5),
                                  (*row)["collapsed"].value_or(false)});
            }
        }
    session_clean = table["session_clean"].value_or(true);
    history.clear();
    bookmarks.clear();
    session.clear();
    windows.clear();
    auto read = [&](const char *key, std::vector<Visit> &target) {
        if (auto a = table[key].as_array())
            for (auto &node : *a)
                if (auto t = node.as_table()) {
                    auto url = (*t)["url"].value_or(std::string{});
                    if (!url.empty())
                        target.push_back(
                            {url, (*t)["title"].value_or(url), (*t)["time"].value_or(0LL)});
                }
    };
    read("history", history);
    read("bookmarks", bookmarks);
    prune_history();
    if (auto a = table["session"].as_array())
        for (auto &n : *a)
            if (auto t = n.as_table())
                session.push_back(
                    {(*t)["url"].value_or(std::string("aura://newtab")),
                     (*t)["pinned"].value_or(false),
                     has_workspace((*t)["workspace"].value_or(0)) ? (*t)["workspace"].value_or(0)
                                                                  : 0,
                     (*t)["workspace_active"].value_or(false), (*t)["group"].value_or(0)});
    if (auto entries = table["windows"].as_array())
        for (const auto &node : *entries) {
            if (windows.size() >= 32)
                break;
            if (auto row = node.as_table()) {
                SavedWindow window;
                window.active = std::max(0, (*row)["active"].value_or(0));
                window.workspace = (*row)["workspace"].value_or(0);
                if (!has_workspace(window.workspace))
                    window.workspace = 0;
                if (auto tabs = (*row)["tabs"].as_array())
                    for (const auto &tab : *tabs) {
                        if (window.tabs.size() >= 500)
                            break;
                        if (auto item = tab.as_table()) {
                            auto url = (*item)["url"].value_or(std::string{});
                            try {
                                if (!url.empty())
                                    window.tabs.push_back(
                                        {navigation_url(url), (*item)["pinned"].value_or(false),
                                         has_workspace((*item)["workspace"].value_or(0))
                                             ? (*item)["workspace"].value_or(0)
                                             : 0,
                                         (*item)["workspace_active"].value_or(false),
                                         (*item)["group"].value_or(0)});
                            } catch (const std::exception &) {
                            }
                        }
                    }
                if (!window.tabs.empty()) {
                    window.active = std::min(window.active, int(window.tabs.size()) - 1);
                    windows.push_back(std::move(window));
                }
            }
        }
    auto validate_group = [this](SavedTab &t) {
        const auto g = group(t.group);
        if (!g || g->workspace != t.workspace)
            t.group = 0;
    };
    for (auto &t : session)
        validate_group(t);
    for (auto &w : windows)
        for (auto &t : w.tabs)
            validate_group(t);
}
void Profile::save() const {
    toml::table root;
    root.insert("schema", 7);
    toml::array group_rows;
    for (const auto &g : groups)
        group_rows.push_back(toml::table{{"id", g.id},
                                         {"workspace", g.workspace},
                                         {"name", g.name},
                                         {"color", g.color},
                                         {"collapsed", g.collapsed}});
    root.insert("groups", std::move(group_rows));
    toml::array spaces;
    for (const auto &space : workspaces)
        spaces.push_back(toml::table{{"id", space.id}, {"name", space.name}});
    root.insert("workspaces", std::move(spaces));
    root.insert("active", active);
    root.insert("session_clean", session_clean);
    for (const auto &setting : settings_catalog())
        std::visit([&](auto member) { root.insert(setting.id, this->*member); }, setting.member);
    auto list = [](const std::vector<Visit> &values) {
        toml::array result;
        for (auto &v : values)
            result.push_back(toml::table{{"url", v.url}, {"title", v.title}, {"time", v.time}});
        return result;
    };
    toml::array links;
    for (const auto &s : shortcuts)
        links.push_back(toml::table{{"title", s.title}, {"url", s.url}});
    root.insert("shortcuts", std::move(links));
    root.insert("history", list(history));
    root.insert("bookmarks", list(bookmarks));
    toml::array tabs;
    for (auto &t : session)
        tabs.push_back(toml::table{{"url", t.url},
                                   {"pinned", t.pinned},
                                   {"workspace", t.workspace},
                                   {"workspace_active", t.workspace_active},
                                   {"group", t.group}});
    root.insert("session", std::move(tabs));
    toml::array saved_windows;
    for (const auto &window : windows) {
        toml::array saved_tabs;
        for (const auto &t : window.tabs)
            saved_tabs.push_back(toml::table{{"url", t.url},
                                             {"pinned", t.pinned},
                                             {"workspace", t.workspace},
                                             {"workspace_active", t.workspace_active},
                                             {"group", t.group}});
        saved_windows.push_back(toml::table{{"active", window.active},
                                            {"workspace", window.workspace},
                                            {"tabs", std::move(saved_tabs)}});
    }
    root.insert("windows", std::move(saved_windows));
    const auto temporary = directory / "profile.toml.tmp";
    {
        std::ofstream file(temporary);
        file << root;
        file.flush();
        if (!file)
            throw std::runtime_error("Cannot save browser profile");
    }
    const auto current = directory / "profile.toml";
    if (std::filesystem::exists(current)) {
        // Rotate only a parseable profile; a bad file must not replace a good backup.
        try {
            (void)toml::parse_file(utf8_path(current));
            const auto backup_temp = directory / "profile.toml.bak.tmp";
            std::filesystem::copy_file(current, backup_temp,
                                       std::filesystem::copy_options::overwrite_existing);
            replace_file(backup_temp, directory / "profile.toml.bak");
        } catch (const toml::parse_error &) {
            throw std::runtime_error("Profile changed to invalid TOML; refusing to overwrite it");
        }
    }
    replace_file(temporary, current);
}
namespace {
bool valid_shortcut(const std::string &title, const std::string &url) {
    return valid_home_address(url) && url.size() <= 4096 && title.size() <= 480 &&
           title.find_first_of("\r\n\t") == std::string::npos;
}
} // namespace
bool Profile::has_workspace(int id) const {
    return std::any_of(workspaces.begin(), workspaces.end(),
                       [id](const auto &w) { return w.id == id; });
}
std::string Profile::workspace_label(int id) const {
    for (const auto &w : workspaces)
        if (w.id == id)
            return id == 0 && w.name == "Personal space" ? translate(language, w.name) : w.name;
    return translate(language, "Personal space");
}
int Profile::add_workspace(std::string name) {
    name = trim(std::move(name));
    if (workspaces.size() >= 32 || name.empty() || name.size() > 120 ||
        name.find_first_of("\r\n\t") != std::string::npos ||
        std::any_of(workspaces.begin(), workspaces.end(), [&](const auto &w) {
            return lower(w.name) == lower(name) || (w.id == 0 && w.name == "Personal space" &&
                                                    lower(translate("ru", w.name)) == lower(name));
        }))
        return -1;
    int id = 1;
    for (const auto &w : workspaces)
        id = std::max(id, w.id + 1);
    if (id > 1000000)
        return -1;
    workspaces.push_back({id, std::move(name)});
    return id;
}
bool Profile::rename_workspace(int id, std::string name) {
    name = trim(std::move(name));
    if (name.empty() || name.size() > 120 || name.find_first_of("\r\n\t") != std::string::npos ||
        std::any_of(workspaces.begin(), workspaces.end(), [&](const auto &w) {
            return w.id != id && (lower(w.name) == lower(name) ||
                                  (w.id == 0 && w.name == "Personal space" &&
                                   lower(translate("ru", w.name)) == lower(name)));
        }))
        return false;
    for (auto &w : workspaces)
        if (w.id == id) {
            w.name = std::move(name);
            return true;
        }
    return false;
}
bool Profile::remove_workspace(int id) {
    if (id == 0 || !has_workspace(id))
        return false;
    std::erase_if(workspaces, [id](const auto &w) { return w.id == id; });
    for (auto &g : groups)
        if (g.workspace == id)
            g.workspace = 0;
    for (auto &window : windows) {
        if (window.workspace == id)
            window.workspace = 0;
        for (auto &t : window.tabs)
            if (t.workspace == id)
                t.workspace = 0;
    }
    for (auto &t : session)
        if (t.workspace == id)
            t.workspace = 0;
    return true;
}
TabGroup *Profile::group(int id) {
    for (auto &g : groups)
        if (g.id == id)
            return &g;
    return nullptr;
}
const TabGroup *Profile::group(int id) const {
    for (const auto &g : groups)
        if (g.id == id)
            return &g;
    return nullptr;
}
int Profile::add_group(int workspace, std::string name, int color) {
    name = trim(std::move(name));
    if (!has_workspace(workspace) || groups.size() >= 128 || name.empty() || name.size() > 120 ||
        name.find_first_of("\r\n\t") != std::string::npos ||
        std::any_of(groups.begin(), groups.end(), [&](const auto &g) {
            return g.workspace == workspace && lower(g.name) == lower(name);
        }))
        return -1;
    int id = 1;
    for (const auto &g : groups)
        id = std::max(id, g.id + 1);
    if (id > 1000000)
        return -1;
    groups.push_back({id, workspace, std::move(name), std::clamp(color, 0, 5), false});
    return id;
}
bool Profile::rename_group(int id, std::string name) {
    auto target = group(id);
    name = trim(std::move(name));
    if (!target || name.empty() || name.size() > 120 ||
        name.find_first_of("\r\n\t") != std::string::npos ||
        std::any_of(groups.begin(), groups.end(), [&](const auto &g) {
            return g.id != id && g.workspace == target->workspace && lower(g.name) == lower(name);
        }))
        return false;
    target->name = std::move(name);
    return true;
}
bool Profile::remove_group(int id) {
    if (!group(id))
        return false;
    std::erase_if(groups, [id](const auto &g) { return g.id == id; });
    for (auto &w : windows)
        for (auto &t : w.tabs)
            if (t.group == id)
                t.group = 0;
    for (auto &t : session)
        if (t.group == id)
            t.group = 0;
    return true;
}
bool Profile::add_shortcut(std::string title, std::string url) {
    if (shortcuts.size() >= 1000 || !valid_shortcut(title, url))
        return false;
    shortcuts.push_back({std::move(title), std::move(url)});
    return true;
}
bool Profile::edit_shortcut(size_t index, std::string title, std::string url) {
    if (index >= shortcuts.size() || !valid_shortcut(title, url))
        return false;
    shortcuts[index] = {std::move(title), std::move(url)};
    return true;
}
void Profile::remove_shortcut(size_t index) {
    if (index < shortcuts.size())
        shortcuts.erase(shortcuts.begin() + index);
}
void Profile::move_shortcut(size_t from, size_t to) {
    if (from >= shortcuts.size() || to >= shortcuts.size() || from == to)
        return;
    auto link = std::move(shortcuts[from]);
    shortcuts.erase(shortcuts.begin() + from);
    shortcuts.insert(shortcuts.begin() + to, std::move(link));
}
void Profile::visit(std::string url, std::string title, bool private_tab) {
    if (!record_history || private_tab || url == "aura://newtab" || url == "about:blank" ||
        url.empty())
        return;
    history.erase(
        std::remove_if(history.begin(), history.end(), [&](auto &v) { return v.url == url; }),
        history.end());
    history.insert(history.begin(), {std::move(url), std::move(title),
                                     std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count()});
    prune_history();
}
void Profile::prune_history() {
    if (history_days > 0) {
        const auto cutoff = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() -
                            history_days * 86400LL;
        std::erase_if(history, [&](const auto &v) { return v.time > 0 && v.time < cutoff; });
    }
    if (history.size() > static_cast<std::size_t>(history_limit))
        history.resize(history_limit);
}
bool Profile::bookmarked(const std::string &url) const {
    return std::any_of(bookmarks.begin(), bookmarks.end(), [&](auto &b) { return b.url == url; });
}
void Profile::toggle_bookmark(std::string url, std::string title) {
    auto it =
        std::find_if(bookmarks.begin(), bookmarks.end(), [&](auto &b) { return b.url == url; });
    if (it != bookmarks.end())
        bookmarks.erase(it);
    else if (url != "aura://newtab")
        bookmarks.insert(bookmarks.begin(), {std::move(url), std::move(title), 0});
}
std::vector<Visit> Profile::suggest(const std::string &query, std::size_t limit) const {
    std::vector<Visit> result;
    if (!address_suggestions)
        return result;
    const auto q = lower(query);
    limit = std::min(limit, static_cast<std::size_t>(suggestion_limit));
    for (auto *list : {&bookmarks, &history}) {
        if ((list == &bookmarks && !suggest_bookmarks) || (list == &history && !suggest_history))
            continue;
        for (auto &v : *list) {
            if (result.size() >= limit)
                return result;
            if ((q.empty() || lower(v.url).find(q) != std::string::npos ||
                 lower(v.title).find(q) != std::string::npos) &&
                !std::any_of(result.begin(), result.end(), [&](auto &p) { return p.url == v.url; }))
                result.push_back(v);
        }
    }
    return result;
}
} // namespace aura
