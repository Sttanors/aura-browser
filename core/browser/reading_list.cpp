#include "core/browser/reading_list.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include "common/text.hpp"
#include "core/browser/preferences.hpp"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <set>
#include <toml++/toml.hpp>
namespace aura {
namespace {
constexpr size_t maximum_bytes = 8 * 1024 * 1024;
size_t bytes(const SavedArticle &a) {
    size_t result = a.url.size() + a.title.size() + a.byline.size();
    for (const auto &b : a.blocks)
        result += b.text.size();
    return result;
}
std::string escape(const std::string &s) {
    std::string out;
    for (char c : s)
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    return out;
}
bool valid_library(const std::vector<SavedArticle> &items) {
    size_t total = 0;
    std::set<int> ids;
    std::set<std::string> urls;
    if (items.size() > 100)
        return false;
    for (const auto &a : items) {
        total += bytes(a);
        if (!valid_article(a) || !ids.insert(a.id).second || !urls.insert(a.url).second)
            return false;
    }
    return total <= maximum_bytes;
}
} // namespace
bool valid_article(const SavedArticle &a) {
    if (a.id <= 0 || a.id > 1000000 || a.title.empty() || a.title.size() > 2000 ||
        a.byline.size() > 4000 || a.url.size() > 16384 || !valid_home_address(a.url) ||
        !(a.url.starts_with("https://") || a.url.starts_with("http://")) || a.blocks.empty() ||
        a.blocks.size() > 2000 || bytes(a) > 512 * 1024 || a.saved < 0)
        return false;
    return std::all_of(a.blocks.begin(), a.blocks.end(), [](const auto &b) {
        return (b.tag == "p" || b.tag == "h2" || b.tag == "pre" || b.tag == "blockquote") &&
               !b.text.empty() && b.text.find('\0') == std::string::npos;
    });
}
std::string offline_article_html(const SavedArticle &a, bool dark, int size, int width,
                                 bool serif) {
    if (!valid_article(a))
        return {};
    std::string html = "<!doctype html><html><head><meta charset='utf-8'>"
                       "<meta http-equiv='Content-Security-Policy' content=\"default-src 'none'; "
                       "style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'\">"
                       "<meta name='viewport' content='width=device-width'><title>" +
                       escape(a.title) +
                       "</title><style>:root{color-scheme:" + std::string(dark ? "dark" : "light") +
                       "}body{background:" + (dark ? "#0D111B" : "#FAFBFF") +
                       ";color:" + (dark ? "#EDF3FF" : "#182033") +
                       ";margin:32px auto;padding:0 24px 60px;max-width:" +
                       std::to_string(std::clamp(width, 560, 900)) +
                       "px;font:" + std::to_string(std::clamp(size, 18, 32)) + "px/1.7 " +
                       (serif ? "Georgia,serif" : "system-ui,sans-serif") +
                       ";overflow-wrap:anywhere}h1{font-size:1.8em;line-height:1.3}h2{font-size:1."
                       "3em}header{font:13px/1.6 "
                       "system-ui,sans-serif;opacity:.8}pre{white-space:pre-wrap}blockquote{border-"
                       "left:3px solid #7B8ECC;padding-left:20px;margin-left:0}</style>"
                       "</head><body><header>Offline / Офлайн<br>" +
                       escape(a.url) + "<br>" + escape(a.byline) + "</header><h1>" +
                       escape(a.title) + "</h1>";
    for (const auto &b : a.blocks)
        html += "<" + b.tag + ">" + escape(b.text) + "</" + b.tag + ">";
    return html + "</body></html>";
}
ReadingList::ReadingList(std::filesystem::path directory) : directory_(std::move(directory)) {
    const auto path = directory_ / "reading-list.toml";
    try {
        if (!std::filesystem::exists(path))
            return;
        if (std::filesystem::file_size(path) > 32 * 1024 * 1024) {
            available_ = false;
            return;
        }
        const auto root = toml::parse_file(utf8_path(path));
        if (root["schema"].value_or(0) != 1 || !root["articles"].is_array()) {
            available_ = false;
            return;
        }
        for (const auto &node : *root["articles"].as_array()) {
            auto table = node.as_table();
            if (!table) {
                available_ = false;
                items_.clear();
                return;
            }
            const auto &row = *table;
            SavedArticle a;
            a.id = row["id"].value_or(0);
            a.url = row["url"].value_or("");
            a.title = row["title"].value_or("");
            a.byline = row["byline"].value_or("");
            a.saved = row["saved"].value_or(0LL);
            a.read = row["read"].value_or(false);
            if (auto blocks = row["blocks"].as_array())
                for (const auto &node : *blocks) {
                    auto b = node.as_table();
                    if (!b) {
                        available_ = false;
                        items_.clear();
                        return;
                    }
                    a.blocks.push_back({(*b)["tag"].value_or(""), (*b)["text"].value_or("")});
                }
            items_.push_back(std::move(a));
        }
        next_id_ = root["next_id"].value_or(1);
        for (const auto &a : items_)
            next_id_ = std::max(next_id_, a.id + 1);
        if (next_id_ < 1 || next_id_ > 1000001 || !valid_library(items_)) {
            items_.clear();
            available_ = false;
        }
    } catch (const std::exception &) {
        items_.clear();
        available_ = false;
    }
}
const SavedArticle *ReadingList::find(int id) const {
    for (const auto &a : items_)
        if (a.id == id)
            return &a;
    return nullptr;
}
std::vector<int> ReadingList::search(const std::string &query) const {
    std::vector<int> result;
    const auto needle = fold_text(query);
    for (const auto &a : items_) {
        bool match =
            fold_text(a.title + "\n" + a.url + "\n" + a.byline).find(needle) != std::string::npos;
        for (const auto &b : a.blocks) {
            if (match)
                break;
            match = fold_text(b.text).find(needle) != std::string::npos;
        }
        if (match)
            result.push_back(a.id);
    }
    return result;
}
bool ReadingList::commit(std::vector<SavedArticle> items) {
    if (!available_ || !valid_library(items))
        return false;
    try {
        int next = next_id_;
        for (const auto &a : items)
            next = std::max(next, a.id + 1);
        toml::array rows;
        for (const auto &a : items) {
            toml::array blocks;
            for (const auto &b : a.blocks)
                blocks.push_back(toml::table{{"tag", b.tag}, {"text", b.text}});
            rows.push_back(toml::table{{"id", a.id},
                                       {"url", a.url},
                                       {"title", a.title},
                                       {"byline", a.byline},
                                       {"saved", a.saved},
                                       {"read", a.read},
                                       {"blocks", std::move(blocks)}});
        }
        std::filesystem::create_directories(directory_);
        const auto temporary = directory_ / "reading-list.toml.tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << toml::table{{"schema", 1}, {"next_id", next}, {"articles", std::move(rows)}};
        file.flush();
        if (!file)
            return false;
        file.close();
        if (!file)
            return false;
        replace_file(temporary, directory_ / "reading-list.toml");
        next_id_ = next;
        items_ = std::move(items);
        ++revision_;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
bool ReadingList::save(SavedArticle a) {
    auto items = items_;
    int next = next_id_;
    for (const auto &old : items) {
        next = std::max(next, old.id + 1);
        if (old.url == a.url) {
            a.id = old.id;
            a.read = old.read;
            a.saved = std::time(nullptr);
            for (auto &entry : items)
                if (entry.id == a.id) {
                    entry = std::move(a);
                    break;
                }
            return commit(std::move(items));
        }
    }
    a.id = next;
    a.saved = std::time(nullptr);
    items.insert(items.begin(), std::move(a));
    return commit(std::move(items));
}
bool ReadingList::mark(int id, bool read) {
    auto items = items_;
    for (auto &a : items)
        if (a.id == id) {
            a.read = read;
            return commit(std::move(items));
        }
    return false;
}
bool ReadingList::remove(int id) {
    auto items = items_;
    if (!std::erase_if(items, [id](const auto &a) { return a.id == id; }))
        return false;
    if (!commit(std::move(items)))
        return false;
    std::error_code error;
    std::filesystem::remove(directory_ / "reading-copies" / (std::to_string(id) + ".html"), error);
    return true;
}
} // namespace aura
