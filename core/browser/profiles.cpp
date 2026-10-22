#include "core/browser/profiles.hpp"
#include "common/config.hpp"
#include "common/file_lock.hpp"
#include "common/platform.hpp"
#include "core/browser/i18n.hpp"
#include "core/browser/profile.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <toml++/toml.hpp>
namespace aura {
namespace {
std::string key(std::string text) {
    for (auto &c : text)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return text;
}
std::string clean(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n"), end = text.find_last_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string{} : text.substr(begin, end - begin + 1);
}
std::filesystem::path normalized(const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}
int member(const std::filesystem::path &root, const std::filesystem::path &current) {
    if (current == root.parent_path())
        return 0;
    if (current.parent_path() != root / "data")
        return -1;
    const auto text = current.filename().string();
    int id = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && id > 0 &&
                   id <= 1000000 && text == std::to_string(id)
               ? id
               : -1;
}
} // namespace
ProfileCatalog::ProfileCatalog(std::filesystem::path directory, std::string stored_root)
    : current(normalized(directory)) {
    root = current / "profiles";
    try {
        if (!stored_root.empty()) {
            const auto candidate = normalized(path_from_utf8(stored_root));
            const int id = member(candidate, current);
            if (id >= 0 && (std::filesystem::exists(candidate / "catalog.toml") ||
                            std::filesystem::exists(candidate / "catalog.toml.bak"))) {
                root = candidate;
                current_id = id;
            }
        }
        reload();
        if (current_id && std::none_of(items.begin(), items.end(),
                                       [&](const auto &p) { return p.id == current_id; })) {
            root = current / "profiles";
            current_id = 0;
            reload();
        }
    } catch (const std::exception &) {
        error = "Could not read the profile catalog. Existing profile files are unchanged.";
    }
}
std::filesystem::path ProfileCatalog::data_directory(int id) const {
    if (id < 0 || id > 1000000)
        throw std::invalid_argument("Invalid profile id");
    return id ? root / "data" / std::to_string(id) : root.parent_path();
}
std::string ProfileCatalog::name(int id, const std::string &language) const {
    for (const auto &p : items)
        if (p.id == id)
            return !id && p.name == "Default profile" ? translate(language, p.name) : p.name;
    return translate(language, "Local profile");
}
void ProfileCatalog::reload() {
    auto read = [&](const std::filesystem::path &path) {
        const auto table = toml::parse_file(utf8_path(path));
        std::vector<NamedProfile> result{{0, "Default profile", false}};
        if (auto rows = table["profiles"].as_array())
            for (const auto &node : *rows) {
                if (result.size() >= 32)
                    break;
                if (auto row = node.as_table()) {
                    const int id = (*row)["id"].value_or(-1);
                    auto name = clean((*row)["name"].value_or(std::string{}));
                    if (id < 0 || id > 1000000 || name.empty() || name.size() > 120 ||
                        name.find_first_of("\r\n\t") != std::string::npos)
                        continue;
                    if (id == 0)
                        result.front().name = name;
                    else if (std::none_of(result.begin(), result.end(), [&](const auto &p) {
                                 return p.id == id || key(p.name) == key(name);
                             }))
                        result.push_back({id, std::move(name), (*row)["archived"].value_or(false)});
                }
            }
        return result;
    };
    const auto path = root / "catalog.toml";
    recovered_ = false;
    try {
        if (std::filesystem::exists(path)) {
            try {
                items = read(path);
            } catch (const std::exception &) {
                items = read(root / "catalog.toml.bak");
                recovered_ = true;
            }
        } else if (std::filesystem::exists(root / "catalog.toml.bak")) {
            items = read(root / "catalog.toml.bak");
            recovered_ = true;
        } else
            items = {{0, "Default profile", false}};
        error.clear();
    } catch (const std::exception &) {
        error = "Could not read the profile catalog. Existing profile files are unchanged.";
        throw std::runtime_error(error);
    }
}
bool ProfileCatalog::valid_name(const std::string &name, int except) const {
    if (name.empty() || name.size() > 120 || name.find_first_of("\r\n\t") != std::string::npos)
        return false;
    if (except != 0 &&
        (key(name) == key(translate("ru", "Default profile")) || key(name) == "default profile"))
        return false;
    return std::none_of(items.begin(), items.end(),
                        [&](const auto &p) { return p.id != except && key(p.name) == key(name); });
}
void ProfileCatalog::save() {
    toml::array rows;
    for (const auto &p : items)
        rows.push_back(toml::table{{"id", p.id}, {"name", p.name}, {"archived", p.archived}});
    toml::table data{{"schema", 1}, {"profiles", std::move(rows)}};
    const auto path = root / "catalog.toml",
               temporary = root / ("catalog.tmp-" + std::to_string(process_id()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output << data;
        output.close();
    }
    if (recovered_ && std::filesystem::exists(path)) {
        auto corrupt = root / "catalog.toml.corrupt";
        for (int n = 1; std::filesystem::exists(corrupt); n++)
            corrupt = root / ("catalog.toml.corrupt." + std::to_string(n));
        std::filesystem::rename(path, corrupt);
    } else if (std::filesystem::exists(path))
        std::filesystem::copy_file(path, root / "catalog.toml.bak",
                                   std::filesystem::copy_options::overwrite_existing);
    replace_file(temporary, path);
    recovered_ = false;
    error.clear();
}
int ProfileCatalog::create(std::string name, const std::string &language) {
    std::filesystem::create_directories(root);
    ExclusiveFileLock lock(root / "catalog.lock");
    if (!lock.owns_lock())
        throw std::runtime_error("The profile catalog is busy. Try again.");
    reload();
    name = clean(std::move(name));
    if (items.size() >= 32 || !valid_name(name))
        return -1;
    int id = 1;
    for (const auto &p : items)
        id = std::max(id, p.id + 1);
    while (id <= 1000000 && std::filesystem::exists(data_directory(id)))
        ++id;
    if (id > 1000000)
        return -1;
    const auto directory = data_directory(id);
    std::filesystem::create_directories(directory);
    Profile seed(directory);
    seed.language = language;
    seed.profiles_root = utf8_path(root);
    seed.save();
    items.push_back({id, std::move(name), false});
    save();
    return id;
}
bool ProfileCatalog::rename(int id, std::string name) {
    std::filesystem::create_directories(root);
    ExclusiveFileLock lock(root / "catalog.lock");
    if (!lock.owns_lock())
        throw std::runtime_error("The profile catalog is busy. Try again.");
    reload();
    name = clean(std::move(name));
    if (!valid_name(name, id))
        return false;
    for (auto &p : items)
        if (p.id == id) {
            p.name = std::move(name);
            save();
            return true;
        }
    return false;
}
bool ProfileCatalog::archive(int id, bool archived) {
    if (id == 0 || (id == current_id && archived))
        return false;
    std::filesystem::create_directories(root);
    ExclusiveFileLock lock(root / "catalog.lock");
    if (!lock.owns_lock())
        throw std::runtime_error("The profile catalog is busy. Try again.");
    reload();
    for (auto &p : items)
        if (p.id == id) {
            p.archived = archived;
            save();
            return true;
        }
    return false;
}
} // namespace aura
