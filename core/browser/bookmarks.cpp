#include "core/browser/i18n.hpp"
#include "core/browser/profile.hpp"
#include <algorithm>
#include <set>

namespace aura {
namespace {
std::string trim(std::string value) {
    const auto a = value.find_first_not_of(" \t\r\n"), b = value.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : value.substr(a, b - a + 1);
}
std::string fold(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        auto c = static_cast<unsigned char>(value[i]);
        if (c >= 'A' && c <= 'Z')
            value[i] += 32;
        else if (c == 0xd0 && i + 1 < value.size()) {
            auto next = static_cast<unsigned char>(value[i + 1]);
            if (next >= 0x90 && next <= 0x9f)
                value[++i] = char(next + 32);
            else if (next >= 0xa0 && next <= 0xaf) {
                value[i] = char(0xd1);
                value[++i] = char(next - 32);
            } else if (next == 0x81) {
                value[i] = char(0xd1);
                value[++i] = char(0x91);
            }
        }
    }
    return value;
}
bool reserved(const std::string &value) {
    const auto name = fold(value);
    return name == "unfiled" || name == "all folders" || name == "без папки" || name == "все папки";
}
bool text(const std::string &value, size_t limit) {
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char c) { return c < 32 || c == 127; });
}
bool url(const std::string &value) { return text(value, 16384) && valid_home_address(value); }
std::string escaped(const std::string &value) {
    std::string out;
    for (unsigned char c : value) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '\"')
            out += "&quot;";
        else if (c == '\'')
            out += "&#39;";
        else if (c >= 32 || c == '\t')
            out += char(c);
    }
    return out;
}
} // namespace
std::optional<std::vector<std::string>> bookmark_tags(const std::string &input) {
    if (input.size() > 4096)
        return {};
    std::vector<std::string> tags;
    std::set<std::string> seen;
    size_t begin = 0;
    while (begin < input.size()) {
        auto end = input.find(',', begin);
        if (end == std::string::npos)
            end = input.size();
        auto tag = trim(input.substr(begin, end - begin));
        if (!tag.empty()) {
            if (!text(tag, 80))
                return {};
            if (seen.insert(fold(tag)).second) {
                if (tags.size() >= 32)
                    return {};
                tags.push_back(std::move(tag));
            }
        }
        begin = end + 1;
    }
    return tags;
}
bool Profile::has_bookmark_folder(int id) const {
    return id == 0 || std::any_of(bookmark_folders.begin(), bookmark_folders.end(),
                                  [&](const auto &f) { return f.id == id; });
}
std::string Profile::bookmark_folder_label(int id) const {
    for (const auto &f : bookmark_folders)
        if (f.id == id)
            return f.name;
    return translate(language, "Unfiled");
}
int Profile::add_bookmark_folder(std::string name, int assigned_id) {
    name = trim(std::move(name));
    if (bookmark_folders.size() >= 128 || !text(name, 120) || reserved(name) ||
        std::any_of(bookmark_folders.begin(), bookmark_folders.end(),
                    [&](const auto &f) { return fold(f.name) == fold(name); }))
        return -1;
    const int id = assigned_id ? assigned_id : next_bookmark_folder;
    if (id < 1 || id > 1000000 || has_bookmark_folder(id))
        return -1;
    next_bookmark_folder = std::max(next_bookmark_folder, id + 1);
    bookmark_folders.push_back({id, std::move(name)});
    return id;
}
bool Profile::rename_bookmark_folder(int id, std::string name) {
    name = trim(std::move(name));
    if (!id || !text(name, 120) || reserved(name))
        return false;
    for (const auto &f : bookmark_folders)
        if (f.id != id && fold(f.name) == fold(name))
            return false;
    for (auto &f : bookmark_folders)
        if (f.id == id) {
            f.name = std::move(name);
            return true;
        }
    return false;
}
bool Profile::remove_bookmark_folder(int id) {
    if (!id || !has_bookmark_folder(id))
        return false;
    std::erase_if(bookmark_folders, [&](const auto &f) { return f.id == id; });
    for (auto &b : bookmarks)
        if (b.folder == id)
            b.folder = 0;
    return true;
}
bool Profile::edit_bookmark(const std::string &old, std::string title, std::string address,
                            int folder, const std::string &input) {
    auto tags = bookmark_tags(input);
    title = trim(std::move(title));
    address = trim(std::move(address));
    if (!tags || !text(title, 2048) || !url(address) || !has_bookmark_folder(folder) ||
        std::any_of(bookmarks.begin(), bookmarks.end(),
                    [&](const auto &b) { return b.url == address && b.url != old; }))
        return false;
    for (auto &b : bookmarks)
        if (b.url == old) {
            b.title = std::move(title);
            b.url = std::move(address);
            b.folder = folder;
            b.tags = std::move(*tags);
            return true;
        }
    return false;
}
std::string bookmarks_html(const std::vector<Visit> &bookmarks,
                           const std::vector<BookmarkFolder> &folders) {
    std::string html = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<META HTTP-EQUIV=\"Content-Type\" "
                       "CONTENT=\"text/html; charset=UTF-8\">\n<TITLE>Aura "
                       "bookmarks</TITLE>\n<H1>Aura bookmarks</H1>\n<DL><p>\n";
    auto write = [&](int folder) {
        for (const auto &b : bookmarks)
            if (b.folder == folder && url(b.url)) {
                std::string tags;
                for (const auto &tag : b.tags) {
                    if (!tags.empty())
                        tags += ",";
                    tags += tag;
                }
                html += "<DT><A HREF=\"" + escaped(b.url) + "\" ADD_DATE=\"" +
                        std::to_string(std::max(0LL, b.time)) + "\"";
                if (!tags.empty())
                    html += " TAGS=\"" + escaped(tags) + "\"";
                html += ">" + escaped(b.title.empty() ? b.url : b.title) + "</A>\n";
            }
    };
    write(0);
    for (const auto &folder : folders) {
        html += "<DT><H3>" + escaped(folder.name) + "</H3>\n<DL><p>\n";
        write(folder.id);
        html += "</DL><p>\n";
    }
    std::set<int> exported{0};
    for (const auto &f : folders)
        exported.insert(f.id);
    for (const auto &b : bookmarks)
        if (exported.insert(b.folder).second)
            write(b.folder);
    return html + "</DL><p>\n";
}
} // namespace aura
