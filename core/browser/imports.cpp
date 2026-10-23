#include "core/browser/imports.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include "include/cef_parser.h"
#include "include/cef_path_util.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <openssl/rand.h>
#include <sqlite3.h>
#include <unordered_set>
namespace aura {
namespace {
constexpr size_t record_limit = 100000;
struct Budget {
    std::shared_ptr<std::atomic_bool> cancel;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    void check() const {
        if (cancel && cancel->load())
            throw std::runtime_error("Import cancelled.");
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error(
                "Import took too long. Close the source browser and try again.");
    }
};
char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ascii_lower);
    return s;
}
long long integer(std::string_view text) {
    long long value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0;
}
long long timestamp(long long value, bool chromium) {
    if (value <= 0)
        return 0;
    return std::clamp(value / 1000000 - (chromium ? 11644473600LL : 0LL), 0LL,
                      static_cast<long long>(std::time(nullptr)));
}
std::string title_text(std::string text) {
    for (auto &c : text)
        if (static_cast<unsigned char>(c) < 32)
            c = ' ';
    if (text.size() > 480) {
        size_t end = 480;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
            --end;
        text.resize(end);
    }
    return text;
}
std::string canonical_url(const std::string &url) {
    if (url.empty() || url.size() > 8192 ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
        return {};
    CefURLParts parts;
    if (!CefParseURL(url, parts))
        return {};
    auto scheme = CefString(&parts.scheme).ToString();
    return (scheme == "http" || scheme == "https") && !CefString(&parts.host).empty()
               ? CefString(&parts.spec).ToString()
               : std::string{};
}
struct Reader {
    BrowserImportData data;
    Budget budget;
    std::unordered_set<std::string> bookmarks, history;
    size_t scanned = 0;
    size_t decoded_bytes = 0;
    void add(std::vector<Visit> &target, std::string url, std::string title, long long time,
             int folder = 0, std::vector<std::string> tags = {}) {
        budget.check();
        if (++scanned > 4 * record_limit)
            throw std::runtime_error("Too many records. Export a smaller selection and try again.");
        if (target.size() >= record_limit) {
            data.limited = true;
            return;
        }
        url = canonical_url(url);
        if (url.empty()) {
            ++data.skipped;
            return;
        }
        title = title.empty() ? url : title_text(std::move(title));
        size_t metadata = 0;
        for (const auto &tag : tags)
            metadata += tag.size();
        if (decoded_bytes + url.size() + title.size() + metadata > 64 * 1024 * 1024) {
            data.limited = true;
            return;
        }
        auto &seen = &target == &data.bookmarks ? bookmarks : history;
        if (!seen.insert(url).second) {
            ++data.duplicates;
            return;
        }
        decoded_bytes += url.size() + title.size() + metadata;
        target.push_back({std::move(url), std::move(title), time, folder, std::move(tags)});
    }
};
std::string entities(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        auto end = text.find(';', i + 1);
        if (end == std::string_view::npos || end - i > 16) {
            out += '&';
            continue;
        }
        auto entity = text.substr(i + 1, end - i - 1);
        uint32_t code = 0;
        if (entity == "amp")
            code = '&';
        else if (entity == "lt")
            code = '<';
        else if (entity == "gt")
            code = '>';
        else if (entity == "quot")
            code = '"';
        else if (entity == "apos")
            code = '\'';
        else if (entity == "nbsp")
            code = ' ';
        else if (entity.starts_with('#')) {
            auto number = entity.substr(1);
            int base = 10;
            if (number.starts_with('x') || number.starts_with('X')) {
                number.remove_prefix(1);
                base = 16;
            }
            auto parsed = std::from_chars(number.data(), number.data() + number.size(), code, base);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
                code = 0;
        }
        if (!code || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            out += '&';
            continue;
        }
        if (code <= 0x7f)
            out += char(code);
        else if (code <= 0x7ff) {
            out += char(0xc0 | (code >> 6));
            out += char(0x80 | (code & 63));
        } else if (code <= 0xffff) {
            out += char(0xe0 | (code >> 12));
            out += char(0x80 | ((code >> 6) & 63));
            out += char(0x80 | (code & 63));
        } else {
            out += char(0xf0 | (code >> 18));
            out += char(0x80 | ((code >> 12) & 63));
            out += char(0x80 | ((code >> 6) & 63));
            out += char(0x80 | (code & 63));
        }
        i = end;
    }
    return out;
}
bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
void read_html(Reader &r, const std::string &text) {
    r.data.source = "Bookmarks HTML";
    std::string url, title, heading, pending_folder;
    std::vector<int> folders;
    std::vector<std::string> tags;
    int link_folder = 0;
    long long date = 0;
    bool anchor = false, in_heading = false;
    auto finish = [&] {
        if (anchor)
            r.add(r.data.bookmarks, entities(url), entities(title), date, link_folder, tags);
        anchor = false;
        url.clear();
        title.clear();
        date = 0;
        tags.clear();
    };
    for (size_t pos = 0; pos < text.size();) {
        r.budget.check();
        auto start = text.find('<', pos);
        if (start == std::string::npos)
            start = text.size();
        if (in_heading && heading.size() < 4096)
            heading.append(text, pos, std::min(start - pos, 4096 - heading.size()));
        if (anchor && title.size() < 4096)
            title.append(text, pos, std::min(start - pos, 4096 - title.size()));
        if (start == text.size())
            break;
        if (text.compare(start, 4, "<!--") == 0) {
            auto end = text.find("-->", start + 4);
            pos = end == std::string::npos ? text.size() : end + 3;
            continue;
        }
        size_t end = start + 1;
        char quote = 0;
        for (; end < text.size(); ++end) {
            char c = text[end];
            if (quote) {
                if (c == quote)
                    quote = 0;
            } else if (c == '\'' || c == '"')
                quote = c;
            else if (c == '>')
                break;
        }
        if (end == text.size())
            break;
        std::string_view tag(text.data() + start + 1, end - start - 1);
        bool closing = tag.starts_with('/');
        if (closing)
            tag.remove_prefix(1);
        size_t name_end = 0;
        while (name_end < tag.size() && !space(tag[name_end]) && tag[name_end] != '/')
            ++name_end;
        auto name = lower(std::string(tag.substr(0, name_end)));
        pos = end + 1;
        if (!closing && (name == "script" || name == "style")) {
            const auto needle = "</" + name;
            auto found = std::search(text.begin() + pos, text.end(), needle.begin(), needle.end(),
                                     [](char a, char b) { return ascii_lower(a) == b; });
            pos = static_cast<size_t>(found - text.begin());
            continue;
        }
        if (name == "h3") {
            if (closing) {
                pending_folder = title_text(entities(heading));
                in_heading = false;
            } else {
                finish();
                heading.clear();
                in_heading = true;
            }
            continue;
        }
        if (name == "dl") {
            if (closing) {
                if (!folders.empty())
                    folders.pop_back();
            } else {
                int id = folders.empty() ? 0 : folders.back();
                if (!pending_folder.empty()) {
                    std::string path;
                    for (const auto &f : r.data.bookmark_folders)
                        if (f.id == id)
                            path = f.name + " / ";
                    path += pending_folder;
                    if (path.size() <= 120 && r.data.bookmark_folders.size() < 128) {
                        auto found = std::find_if(r.data.bookmark_folders.begin(),
                                                  r.data.bookmark_folders.end(),
                                                  [&](const auto &f) { return f.name == path; });
                        if (found == r.data.bookmark_folders.end()) {
                            id = int(r.data.bookmark_folders.size()) + 1;
                            r.data.bookmark_folders.push_back({id, path});
                        } else
                            id = found->id;
                    } else {
                        id = 0;
                        r.data.limited = true;
                    }
                    pending_folder.clear();
                }
                if (folders.size() >= 128)
                    throw std::runtime_error("Bookmarks folder nesting is too deep");
                folders.push_back(id);
            }
            continue;
        }
        if (name != "a")
            continue;
        if (closing) {
            finish();
            continue;
        }
        finish();
        anchor = true;
        link_folder = folders.empty() ? 0 : folders.back();
        size_t p = name_end;
        while (p < tag.size()) {
            while (p < tag.size() && space(tag[p]))
                ++p;
            size_t begin = p;
            while (p < tag.size() && !space(tag[p]) && tag[p] != '=')
                ++p;
            auto key = lower(std::string(tag.substr(begin, p - begin)));
            while (p < tag.size() && space(tag[p]))
                ++p;
            if (p == tag.size() || tag[p] != '=')
                continue;
            ++p;
            while (p < tag.size() && space(tag[p]))
                ++p;
            if (p == tag.size())
                break;
            char q = tag[p] == '\'' || tag[p] == '"' ? tag[p++] : 0;
            begin = p;
            while (p < tag.size() && (q ? tag[p] != q : !space(tag[p])))
                ++p;
            auto value = tag.substr(begin, p - begin);
            if (q && p < tag.size())
                ++p;
            if (key == "tags")
                if (auto parsed = bookmark_tags(entities(value)))
                    tags = std::move(*parsed);
            if (key == "href")
                url = std::string(value);
            if (key == "add_date")
                date = std::clamp(integer(value), 0LL, static_cast<long long>(std::time(nullptr)));
        }
    }
    finish();
}
void read_json(Reader &r, const std::string &text) {
    auto value = CefParseJSON(text, JSON_PARSER_RFC);
    if (!value || value->GetType() != VTYPE_DICTIONARY)
        throw std::runtime_error("This is not a supported browser export.");
    auto roots = value->GetDictionary()->GetDictionary("roots");
    if (!roots)
        throw std::runtime_error("This is not a supported browser export.");
    r.data.source = "Chromium bookmarks JSON";
    std::function<void(CefRefPtr<CefDictionaryValue>, int)> walk;
    size_t nodes = 0;
    walk = [&](CefRefPtr<CefDictionaryValue> node, int depth) {
        r.budget.check();
        if (!node || ++nodes > 2 * record_limit || depth > 64)
            throw std::runtime_error("Too many records. Export a smaller selection and try again.");
        if (node->GetType("url") == VTYPE_STRING)
            r.add(r.data.bookmarks, node->GetString("url"), node->GetString("name"),
                  timestamp(integer(node->GetString("date_added").ToString()), true));
        if (auto children = node->GetList("children"))
            for (size_t i = 0; i < children->GetSize(); ++i)
                if (children->GetType(i) == VTYPE_DICTIONARY)
                    walk(children->GetDictionary(i), depth + 1);
    };
    CefDictionaryValue::KeyList keys;
    roots->GetKeys(keys);
    for (const auto &key : keys)
        if (roots->GetType(key) == VTYPE_DICTIONARY)
            walk(roots->GetDictionary(key), 0);
}
std::string column(sqlite3_stmt *stmt, int index) {
    const auto *text = sqlite3_column_text(stmt, index);
    return text ? std::string(reinterpret_cast<const char *>(text),
                              sqlite3_column_bytes(stmt, index))
                : "";
}
void read_sqlite(Reader &r, const std::filesystem::path &path) {
    sqlite3 *raw = nullptr;
    const int status = sqlite3_open_v2(utf8_path(path).c_str(), &raw,
                                       SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    if (status != SQLITE_OK || !db || sqlite3_db_readonly(db.get(), "main") != 1)
        throw std::runtime_error(
            "Cannot read this database. Close the source browser and try again.");
    sqlite3_busy_timeout(db.get(), 1000);
    sqlite3_limit(db.get(), SQLITE_LIMIT_LENGTH, 128 * 1024);
    sqlite3_limit(db.get(), SQLITE_LIMIT_SQL_LENGTH, 4096);
    sqlite3_db_config(db.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_db_config(db.get(), SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    sqlite3_enable_load_extension(db.get(), 0);
    sqlite3_progress_handler(
        db.get(), 1000,
        [](void *context) {
            auto *budget = static_cast<Budget *>(context);
            return (budget->cancel && budget->cancel->load()) ||
                           std::chrono::steady_clock::now() > budget->deadline
                       ? 1
                       : 0;
        },
        &r.budget);
    if (sqlite3_exec(
            db.get(),
            "PRAGMA temp_store=FILE; PRAGMA cache_size=-8192; PRAGMA query_only=ON; BEGIN;",
            nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(
            "Cannot read this database. Close the source browser and try again.");
    auto table_exists = [&](const char *name) {
        sqlite3_stmt *stmt = nullptr;
        sqlite3_prepare_v2(db.get(), "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?1",
                           -1, &stmt, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(stmt, sqlite3_finalize);
        if (!query)
            return false;
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        return sqlite3_step(stmt) == SQLITE_ROW;
    };
    auto read = [&](const char *sql, std::vector<Visit> &target, bool chromium) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(
                "Cannot read this database. Close the source browser and try again.");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(stmt, sqlite3_finalize);
        int code;
        while ((code = sqlite3_step(stmt)) == SQLITE_ROW)
            r.add(target, column(stmt, 0), column(stmt, 1),
                  timestamp(sqlite3_column_int64(stmt, 2), chromium));
        r.budget.check();
        if (code != SQLITE_DONE)
            throw std::runtime_error(
                "Cannot read this database. Close the source browser and try again.");
    };
    if (table_exists("moz_places")) {
        r.data.source = "Firefox places.sqlite";
        if (table_exists("moz_bookmarks"))
            read("SELECT p.url, COALESCE(b.title,p.title,p.url), b.dateAdded FROM moz_bookmarks b "
                 "JOIN moz_places p ON b.fk=p.id WHERE b.type=1 ORDER BY b.id LIMIT 100001",
                 r.data.bookmarks, false);
        read("SELECT url, COALESCE(title,url), last_visit_date FROM moz_places WHERE "
             "last_visit_date>0 ORDER BY last_visit_date DESC LIMIT 100001",
             r.data.history, false);
    } else if (table_exists("urls")) {
        r.data.source = "Chromium History";
        read("SELECT url, COALESCE(title,url), last_visit_time FROM urls WHERE visit_count>0 ORDER "
             "BY last_visit_time DESC LIMIT 100001",
             r.data.history, true);
    } else
        throw std::runtime_error("This is not a supported browser export.");
}
} // namespace
namespace {
void read_file(Reader &reader, const std::filesystem::path &path) {
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 2ULL * 1024 * 1024 * 1024)
        throw std::runtime_error("Choose an existing browser export or history database.");
    reader.budget.check();
    std::ifstream in(path, std::ios::binary);
    char header[16]{};
    in.read(header, sizeof(header));
    if (in.gcount() == 16 &&
        std::string_view(header, 16) == std::string_view("SQLite format 3\0", 16))
        read_sqlite(reader, path);
    else {
        const auto size = std::filesystem::file_size(path);
        if (size > 64 * 1024 * 1024)
            throw std::runtime_error("This export is too large. Choose a file smaller than 64 MB.");
        in.clear();
        in.seekg(0);
        std::string bytes(size, '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!in && !bytes.empty())
            throw std::runtime_error("Cannot read the import file.");
        if (bytes.starts_with("\xef\xbb\xbf"))
            bytes.erase(0, 3);
        auto first = bytes.find_first_not_of(" \r\n\t");
        if (first != std::string::npos && bytes[first] == '{')
            read_json(reader, bytes);
        else if (first != std::string::npos && bytes[first] == '<')
            read_html(reader, bytes);
        else
            throw std::runtime_error("This is not a supported browser export.");
    }
}
} // namespace
BrowserImportData read_browser_import(const std::filesystem::path &path,
                                      const std::shared_ptr<std::atomic_bool> &cancel) {
    Reader reader;
    reader.budget.cancel = cancel;
    if (path.is_absolute() && std::filesystem::is_directory(path)) {
        const auto places = path / "places.sqlite", bookmarks = path / "Bookmarks",
                   history = path / "History";
        if (std::filesystem::is_regular_file(places)) {
            read_file(reader, places);
        } else {
            bool found = false;
            if (std::filesystem::is_regular_file(bookmarks)) {
                read_file(reader, bookmarks);
                found = true;
            }
            if (std::filesystem::is_regular_file(history)) {
                read_file(reader, history);
                found = true;
            }
            if (!found)
                throw std::runtime_error("Choose an existing browser export or history database.");
            reader.data.source = "Chromium profile";
        }
    } else
        read_file(reader, path);
    reader.budget.check();
    if (reader.data.bookmarks.empty() && reader.data.history.empty())
        throw std::runtime_error("No supported web addresses were found in this file.");
    return std::move(reader.data);
}
std::vector<BrowserImportRoot> browser_import_roots() {
    std::vector<BrowserImportRoot> roots;
#ifdef _WIN32
    CefString local, roaming;
    if (CefGetPath(PK_LOCAL_APP_DATA, local)) {
        auto base = path_from_utf8(local.ToString());
        roots.push_back({"Chrome", base / "Google/Chrome/User Data", false});
        roots.push_back({"Chromium", base / "Chromium/User Data", false});
        roots.push_back({"Brave", base / "BraveSoftware/Brave-Browser/User Data", false});
    }
    if (CefGetPath(PK_USER_DATA, roaming))
        roots.push_back(
            {"Firefox", path_from_utf8(roaming.ToString()) / "Mozilla/Firefox/Profiles", true});
#else
    const auto *home_value = std::getenv("HOME");
    if (!home_value)
        return roots;
    const auto home = path_from_utf8(home_value);
    if (!home.is_absolute())
        return roots;
    const auto *config_value = std::getenv("XDG_CONFIG_HOME");
    auto config = config_value ? path_from_utf8(config_value) : home / ".config";
    if (!config.is_absolute())
        config = home / ".config";
    roots = {
        {"Firefox", home / ".mozilla/firefox", true},
        {"Firefox", config / "mozilla/firefox", true},
        {"Firefox Snap", home / "snap/firefox/common/.mozilla/firefox", true},
        {"Chrome", config / "google-chrome", false},
        {"Chromium", config / "chromium", false},
        {"Brave", config / "BraveSoftware/Brave-Browser", false},
        {"Firefox Flatpak", home / ".var/app/org.mozilla.firefox/.mozilla/firefox", true},
        {"Firefox Flatpak", home / ".var/app/org.mozilla.firefox/config/mozilla/firefox", true},
        {"Chrome Flatpak", home / ".var/app/com.google.Chrome/config/google-chrome", false},
        {"Chromium Flatpak", home / ".var/app/org.chromium.Chromium/config/chromium", false}};
#endif
    return roots;
}
std::vector<BrowserImportSource>
discover_browser_imports(const std::vector<BrowserImportRoot> &roots) {
    std::vector<BrowserImportSource> sources;
    std::unordered_set<std::string> seen;
    for (const auto &root : roots) {
        std::error_code error;
        auto it = std::filesystem::directory_iterator(
            root.directory, std::filesystem::directory_options::skip_permission_denied, error);
        size_t scanned = 0;
        for (; !error && it != std::filesystem::directory_iterator{} && scanned++ < 128;
             it.increment(error)) {
            std::error_code entry_error;
            if (!it->is_directory(entry_error) || entry_error)
                continue;
            const auto name = utf8_path(it->path().filename());
            if (!root.firefox && name != "Default" && !name.starts_with("Profile "))
                continue;
            auto is_file = [&](const char *name) {
                std::error_code check_error;
                return std::filesystem::is_regular_file(it->path() / name, check_error);
            };
            bool found = root.firefox ? is_file("places.sqlite")
                                      : (is_file("Bookmarks") || is_file("History"));
            if (found) {
                // Legacy locations can link to XDG profiles. Show each physical profile once.
                auto canonical = std::filesystem::weakly_canonical(it->path(), entry_error);
                if (!entry_error && seen.insert(utf8_path(canonical)).second)
                    sources.push_back({root.browser + " · " + name, it->path()});
            }
            if (sources.size() >= 32)
                break;
        }
        if (sources.size() >= 32)
            break;
    }
    std::sort(sources.begin(), sources.end(),
              [](const auto &a, const auto &b) { return a.label < b.label; });
    return sources;
}
BrowserImportResult apply_browser_import(Profile &profile, const BrowserImportData &data,
                                         bool bookmarks, bool history) {
    BrowserImportResult result;
    result.duplicates = data.duplicates;
    Profile updated = profile;
    std::unordered_set<std::string> previous_history;
    for (const auto &v : profile.history)
        previous_history.insert(v.url);
    std::map<int, int> folder_map{{0, 0}};
    if (bookmarks)
        for (const auto &folder : data.bookmark_folders) {
            if (folder.id <= 0)
                continue;
            auto found =
                std::find_if(updated.bookmark_folders.begin(), updated.bookmark_folders.end(),
                             [&](const auto &f) { return f.name == folder.name; });
            const int id = found == updated.bookmark_folders.end()
                               ? updated.add_bookmark_folder(folder.name)
                               : found->id;
            folder_map[folder.id] = std::max(0, id);
        }
    auto merge = [&](std::vector<Visit> &target, const std::vector<Visit> &source, size_t &count) {
        std::unordered_set<std::string> seen;
        for (const auto &v : target) {
            auto normalized = canonical_url(v.url);
            seen.insert(normalized.empty() ? v.url : normalized);
        }
        for (const auto &v : source)
            if (seen.insert(v.url).second) {
                auto copy = v;
                if (&target == &updated.bookmarks) {
                    auto found = folder_map.find(v.folder);
                    copy.folder = found == folder_map.end() ? 0 : found->second;
                }
                target.push_back(std::move(copy));
                ++count;
            } else
                ++result.duplicates;
    };
    if (bookmarks)
        merge(updated.bookmarks, data.bookmarks, result.bookmarks_added);
    if (history)
        merge(updated.history, data.history, result.history_added);
    std::stable_sort(updated.history.begin(), updated.history.end(),
                     [](const auto &a, const auto &b) { return a.time > b.time; });
    updated.prune_history();
    const auto found_history = result.history_added;
    result.history_added = 0;
    if (history)
        for (const auto &v : updated.history)
            if (!previous_history.contains(v.url))
                ++result.history_added;
    result.history_limited = found_history - result.history_added;
    result.history_retained = updated.history.size();
    if (!result.bookmarks_added && !result.history_added)
        return result;
    const auto root = profile.directory / "import-backups";
    std::filesystem::create_directories(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    unsigned char random[12];
    if (RAND_bytes(random, sizeof(random)) != 1)
        throw std::runtime_error("Cannot create an import backup.");
    std::string name = "import-";
    for (auto c : random) {
        name += "0123456789abcdef"[c >> 4];
        name += "0123456789abcdef"[c & 15];
    }
    result.backup = root / name;
    if (!std::filesystem::create_directory(result.backup))
        throw std::runtime_error("Cannot create an import backup.");
    std::filesystem::permissions(result.backup, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    Profile before = profile;
    before.directory = result.backup;
    before.save();
    updated.save();
    profile = std::move(updated);
    return result;
}
BrowserImporter::~BrowserImporter() {
    shutdown();
}
void BrowserImporter::preview(const std::string &path) {
    if (busy)
        return;
    candidate.reset();
    result.reset();
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    status = "Reading browser data...";
    busy = true;
    try {
        future_ =
            std::async(std::launch::async, [file = path_from_utf8(path), cancel = cancelled_] {
                return read_browser_import(file, cancel);
            });
    } catch (const std::exception &) {
        busy = false;
        status = "Cannot start the import. Try again later.";
    }
}
void BrowserImporter::cancel() {
    if (cancelled_)
        cancelled_->store(true);
}
bool BrowserImporter::tick() {
    if (!busy || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    try {
        auto data = future_.get();
        if (cancelled_->load())
            throw std::runtime_error("Import cancelled.");
        candidate = std::make_unique<BrowserImportData>(std::move(data));
        status = "Preview ready. Choose what to import.";
    } catch (const std::filesystem::filesystem_error &) {
        status = "Choose an existing browser export or history database.";
    } catch (const std::exception &e) {
        status = e.what();
    }
    busy = false;
    return true;
}
void BrowserImporter::apply(Profile &profile, bool bookmarks, bool history) {
    if (busy || !candidate ||
        !((bookmarks && !candidate->bookmarks.empty()) || (history && !candidate->history.empty())))
        return;
    try {
        result = apply_browser_import(profile, *candidate, bookmarks, history);
        status = result->bookmarks_added || result->history_added
                     ? "Import complete. A backup was saved."
                     : "Nothing new to import.";
        candidate.reset();
    } catch (const std::exception &e) {
        status = e.what();
    }
}
void BrowserImporter::shutdown() {
    cancel();
    if (future_.valid())
        future_.wait();
    busy = false;
}
} // namespace aura
