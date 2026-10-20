#include "common/platform.hpp"
#include "core/browser/profile.hpp"
#include "common/config.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
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
    if (input.find_first_of(" \t\n") == std::string::npos &&
        (input.find('.') != std::string::npos || name.starts_with("localhost") ||
         name.starts_with("[::1]")))
        return (name.starts_with("localhost") || name.starts_with("127.") ||
                        name.starts_with("[::1]")
                    ? "http://"
                    : "https://") +
               input;
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
    if (!std::filesystem::exists(file))
        return;
    const auto table = toml::parse_file(utf8_path(file));
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
    history.clear();
    bookmarks.clear();
    session.clear();
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
                session.push_back({(*t)["url"].value_or(std::string("aura://newtab")),
                                   (*t)["pinned"].value_or(false)});
}
void Profile::save() const {
    toml::table root;
    root.insert("schema", 4);
    root.insert("active", active);
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
        tabs.push_back(toml::table{{"url", t.url}, {"pinned", t.pinned}});
    root.insert("session", std::move(tabs));
    const auto temporary = directory / "profile.toml.tmp";
    {
        std::ofstream file(temporary);
        file << root;
        file.flush();
        if (!file)
            throw std::runtime_error("Cannot save browser profile");
    }
    replace_file(temporary, directory / "profile.toml");
}
namespace {
bool valid_shortcut(const std::string &title, const std::string &url) {
    return valid_home_address(url) && url.size() <= 4096 && title.size() <= 480 &&
           title.find_first_of("\r\n\t") == std::string::npos;
}
} // namespace
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
