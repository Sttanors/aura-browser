#include "core/browser/extensions.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include "core/browser/protection.hpp"
#include "include/cef_parser.h"
#include "include/cef_version.h"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <set>
#include <toml++/toml.hpp>
namespace aura {
std::optional<int> compare_extension_versions(const std::string &a, const std::string &b) {
    auto parse = [](const std::string &text) -> std::optional<std::array<int, 4>> {
        std::array<int, 4> numbers{};
        size_t start = 0, index = 0;
        while (start < text.size() && index < 4) {
            auto end = text.find('.', start);
            if (end == std::string::npos)
                end = text.size();
            if (end == start || (end - start > 1 && text[start] == '0'))
                return {};
            int value = 0;
            auto parsed = std::from_chars(text.data() + start, text.data() + end, value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + end || value < 0 ||
                value > 65535)
                return {};
            numbers[index++] = value;
            if (end == text.size()) {
                start = end;
                break;
            }
            start = end + 1;
            if (start == text.size())
                return {};
        }
        if (start != text.size() || index == 0 ||
            std::all_of(numbers.begin(), numbers.end(), [](int n) { return n == 0; }))
            return {};
        return numbers;
    };
    auto x = parse(a), y = parse(b);
    if (!x || !y)
        return {};
    return *x < *y ? -1 : *x > *y ? 1 : 0;
}
namespace {
std::string chromium_version() {
    return std::to_string(CHROME_VERSION_MAJOR) + "." + std::to_string(CHROME_VERSION_MINOR) + "." +
           std::to_string(CHROME_VERSION_BUILD) + "." + std::to_string(CHROME_VERSION_PATCH);
}
std::string read_crx_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read this extension file.");
    std::string bytes;
    char chunk[65536];
    while (in) {
        in.read(chunk, sizeof chunk);
        auto n = in.gcount();
        if (bytes.size() + n > 50 * 1024 * 1024)
            throw std::runtime_error("Extension package exceeds 50 MB.");
        bytes.append(chunk, static_cast<size_t>(n));
    }
    if (!in.eof())
        throw std::runtime_error("Cannot read this extension file.");
    return bytes;
}
bool valid_id(const std::string &id) {
    return id.size() == 32 &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; });
}
std::string extension_id(const unsigned char *bytes) {
    std::string id;
    for (int i = 0; i < 16; i++) {
        id += char('a' + (bytes[i] >> 4));
        id += char('a' + (bytes[i] & 15));
    }
    return id;
}
uint32_t little(const char *p) {
    return uint32_t((unsigned char)p[0]) | (uint32_t((unsigned char)p[1]) << 8) |
           (uint32_t((unsigned char)p[2]) << 16) | (uint32_t((unsigned char)p[3]) << 24);
}
struct Field {
    unsigned number;
    std::string_view data;
};
std::vector<Field> protobuf(std::string_view input) {
    std::vector<Field> out;
    size_t pos = 0;
    auto varint = [&]() {
        uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (pos >= input.size())
                throw std::runtime_error("Truncated CRX header");
            auto c = (unsigned char)input[pos++];
            value |= uint64_t(c & 127) << shift;
            if (!(c & 128))
                return value;
        }
        throw std::runtime_error("Invalid CRX header");
    };
    while (pos < input.size()) {
        auto key = varint();
        if ((key & 7) != 2)
            throw std::runtime_error("Unsupported CRX header field");
        auto size = varint();
        if (size > input.size() - pos)
            throw std::runtime_error("Invalid CRX header length");
        out.push_back({unsigned(key >> 3), input.substr(pos, size)});
        pos += size;
    }
    return out;
}
std::string base64(std::string_view s) {
    std::string out(4 * ((s.size() + 2) / 3), '\0');
    EVP_EncodeBlock((unsigned char *)out.data(), (const unsigned char *)s.data(), s.size());
    return out;
}
bool safe_path(const std::string &path) {
    if (path.empty() || path.size() > 1000 || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos)
        return false;
    std::filesystem::path p(path);
    if (p.is_absolute())
        return false;
    for (const auto &part : p)
        if (part == ".." || part == ".")
            return false;
    return true;
}
std::string entry(CefRefPtr<CefDictionaryValue> dict, const char *key) {
    auto v = dict->GetString(key).ToString();
    if (!v.empty() && !safe_path(v))
        throw std::runtime_error("Unsafe extension entry point");
    return v;
}
std::string manifest_icon(CefRefPtr<CefDictionaryValue> m) {
    auto pick = [](CefRefPtr<CefDictionaryValue> d, const char *key) -> std::string {
        if (d->GetType(key) == VTYPE_STRING) {
            auto p = d->GetString(key).ToString();
            return safe_path(p) ? p : "";
        }
        if (auto icons = d->GetDictionary(key)) {
            CefDictionaryValue::KeyList keys;
            icons->GetKeys(keys);
            int best = 100000;
            std::string path;
            for (const auto &k : keys) {
                try {
                    int size = std::stoi(k.ToString());
                    int score = size >= 32 ? size - 32 : 1000 + 32 - size;
                    auto p = icons->GetString(k).ToString();
                    if (size > 0 && safe_path(p) && score < best) {
                        path = p;
                        best = score;
                    }
                } catch (const std::exception &) {
                }
            }
            return path;
        }
        return {};
    };
    if (auto action = m->GetDictionary("action")) {
        auto icon = pick(action, "default_icon");
        if (!icon.empty())
            return icon;
    }
    return pick(m, "icons");
}
InstalledExtension manifest(const std::filesystem::path &dir, const std::string &id,
                            const std::string &key) {
    auto value = CefParseJSON(read_resource(dir / "manifest.json"), JSON_PARSER_RFC);
    if (!value || value->GetType() != VTYPE_DICTIONARY)
        throw std::runtime_error("Invalid extension manifest");
    auto m = value->GetDictionary();
    if (m->GetInt("manifest_version") != 3)
        throw std::runtime_error(
            "Only Manifest V3 extensions are supported by this Chromium version");
    InstalledExtension e;
    e.id = id;
    e.name = m->GetString("name").ToString();
    e.version = m->GetString("version").ToString();
    if (e.name.empty() || !compare_extension_versions(e.version, e.version) ||
        e.name.size() > 256 || e.version.size() > 64)
        throw std::runtime_error("Invalid extension name or version");
    if (m->HasKey("minimum_chrome_version")) {
        auto order =
            compare_extension_versions(m->GetString("minimum_chrome_version"), chromium_version());
        if (!order || *order > 0)
            throw std::runtime_error(
                "This extension requires a newer Chromium version. Update Aura first.");
    }
    // Resolve the displayed Web Store name from the bundled default locale.
    if (e.name.starts_with("__MSG_") && e.name.ends_with("__")) {
        auto locale = m->GetString("default_locale").ToString();
        if (!locale.empty() &&
            locale.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                std::string::npos) {
            auto localized = CefParseJSON(
                read_resource(dir / "_locales" / locale / "messages.json"), JSON_PARSER_RFC);
            if (localized && localized->GetType() == VTYPE_DICTIONARY) {
                auto names = localized->GetDictionary();
                auto namekey = e.name.substr(6, e.name.size() - 8);
                auto message = names->GetDictionary(namekey);
                if (message)
                    e.name = message->GetString("message").ToString();
            }
        }
    }
    if (auto action = m->GetDictionary("action"))
        e.popup = entry(action, "default_popup");
    e.options = entry(m, "options_page");
    if (auto options = m->GetDictionary("options_ui"))
        e.options = entry(options, "page");
    std::set<std::string> permissions;
    for (const auto *kind : {"permissions", "host_permissions"})
        if (auto list = m->GetList(kind))
            for (size_t i = 0; i < list->GetSize(); i++)
                if (list->GetType(i) == VTYPE_STRING)
                    permissions.insert(list->GetString(i).ToString());
    for (const auto *kind : {"optional_permissions", "optional_host_permissions"})
        if (auto list = m->GetList(kind))
            for (size_t i = 0; i < list->GetSize(); ++i)
                if (list->GetType(i) == VTYPE_STRING)
                    permissions.insert("optional:" + list->GetString(i).ToString());
    // Content scripts can access matched sites even without host_permissions.
    if (auto list = m->GetList("content_scripts"))
        for (size_t i = 0; i < list->GetSize(); i++)
            if (auto script = list->GetDictionary(i))
                if (auto matches = script->GetList("matches"))
                    for (size_t j = 0; j < matches->GetSize(); j++)
                        permissions.insert(matches->GetString(j).ToString());
    e.permissions.assign(permissions.begin(), permissions.end());
    e.icon = manifest_icon(m);
    if (auto background = m->GetDictionary("background");
        background && background->HasKey("service_worker")) {
        const auto worker = entry(background, "service_worker");
        if (worker.empty() || !std::filesystem::is_regular_file(dir / path_from_utf8(worker)))
            throw std::runtime_error("Extension background worker is missing or invalid.");
        const auto original = path_from_utf8(worker);
        const auto alias =
            original.parent_path() / path_from_utf8(utf8_path(original.stem()) + "-aura-" +
                                                    e.version + utf8_path(original.extension()));
        try {
            std::filesystem::copy_file(dir / original, dir / alias,
                                       std::filesystem::copy_options::none);
        } catch (...) {
            throw std::runtime_error("Cannot prepare the extension background worker.");
        }
        auto portable = alias.generic_u8string();
        background->SetString(
            "service_worker",
            std::string(reinterpret_cast<const char *>(portable.data()), portable.size()));
    }
    m->SetString("key", key); // Preserve the signed Web Store ID when loading unpacked in CEF.
    std::ofstream out(dir / "manifest.json", std::ios::binary);
    out << CefWriteJSON(value, JSON_WRITER_PRETTY_PRINT).ToString();
    if (!out)
        throw std::runtime_error("Cannot save verified manifest");
    return e;
}
} // namespace
std::string chrome_store_id(const std::string &raw) {
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto input = raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
    if (valid_id(input))
        return input;
    if (!input.starts_with("https://chromewebstore.google.com/detail/") &&
        !input.starts_with("https://chrome.google.com/webstore/detail/"))
        return {};
    auto path = input.substr(0, input.find_first_of("?#"));
    auto id = path.substr(path.find_last_of('/') + 1);
    return valid_id(id) ? id : "";
}
ExtensionCandidate inspect_crx(const std::string &bytes, const std::string &expected,
                               const std::filesystem::path &dir) {
    if (bytes.size() < 12 || bytes.size() > 50 * 1024 * 1024 || bytes.compare(0, 4, "Cr24") ||
        little(bytes.data() + 4) != 3)
        throw std::runtime_error("Expected a signed CRX3 extension package");
    auto size = little(bytes.data() + 8);
    if (size > 1024 * 1024 || size > bytes.size() - 12)
        throw std::runtime_error("Invalid CRX3 header size");
    auto fields = protobuf(std::string_view(bytes).substr(12, size));
    std::string_view signed_data, public_key;
    for (const auto &f : fields)
        if (f.number == 10000)
            signed_data = f.data;
    if (signed_data.empty())
        throw std::runtime_error("Missing CRX3 signature data");
    std::string id;
    for (const auto &f : protobuf(signed_data))
        if (f.number == 1 && f.data.size() == 16)
            id = extension_id((const unsigned char *)f.data.data());
    if (!valid_id(id) || (!expected.empty() && id != expected))
        throw std::runtime_error("Extension ID does not match the requested Web Store item");
    std::string signed_bytes("CRX3 SignedData\0", 16);
    uint32_t length = signed_data.size();
    for (int i = 0; i < 4; i++)
        signed_bytes += char((length >> (i * 8)) & 255);
    signed_bytes += signed_data;
    signed_bytes.append(bytes, 12 + size, std::string::npos);
    bool verified = false;
    for (const auto &f : fields)
        if (f.number == 2 || f.number == 3) {
            std::string_view key, signature;
            for (const auto &proof : protobuf(f.data)) {
                if (proof.number == 1)
                    key = proof.data;
                if (proof.number == 2)
                    signature = proof.data;
            }
            if (key.empty() || signature.empty())
                continue;
            unsigned char digest[32];
            unsigned int digest_size = 0;
            EVP_Digest(key.data(), key.size(), digest, &digest_size, EVP_sha256(), nullptr);
            if (extension_id(digest) != id)
                continue;
            const unsigned char *cursor = (const unsigned char *)key.data();
            auto *pkey = d2i_PUBKEY(nullptr, &cursor, key.size());
            auto *ctx = EVP_MD_CTX_new();
            if (pkey && ctx &&
                EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
                EVP_DigestVerify(ctx, (const unsigned char *)signature.data(), signature.size(),
                                 (const unsigned char *)signed_bytes.data(),
                                 signed_bytes.size()) == 1) {
                verified = true;
                public_key = key;
            }
            EVP_MD_CTX_free(ctx);
            EVP_PKEY_free(pkey);
            if (verified)
                break;
        }
    if (!verified)
        throw std::runtime_error("CRX3 signature verification failed");
    if (std::filesystem::exists(dir))
        throw std::runtime_error("Extension staging directory already exists");
    std::filesystem::create_directories(dir);
    try {
        auto *raw = archive_read_new();
        if (!raw)
            throw std::runtime_error("Cannot create archive reader");
        std::unique_ptr<archive, decltype(&archive_read_free)> a(raw, archive_read_free);
        archive_read_support_format_zip(a.get());
        if (archive_read_open_memory(a.get(), bytes.data() + 12 + size, bytes.size() - 12 - size) !=
            ARCHIVE_OK)
            throw std::runtime_error("Invalid CRX3 ZIP payload");
        archive_entry *item = nullptr;
        size_t total = 0, count = 0;
        std::set<std::string> names;
        int next;
        while ((next = archive_read_next_header(a.get(), &item)) == ARCHIVE_OK) {
            const auto *filename = archive_entry_pathname_utf8(item);
            if (!filename)
                filename = archive_entry_pathname(item);
            std::string name = filename ? filename : "";
            const auto type = archive_entry_filetype(item);
            if (!safe_path(name) || archive_entry_symlink(item) || archive_entry_hardlink(item) ||
                (type != AE_IFREG && type != AE_IFDIR) ||
                !names.insert(std::filesystem::path(name).lexically_normal().string()).second ||
                ++count > 15000)
                throw std::runtime_error("Unsafe extension archive entry");
            auto path = dir / path_from_utf8(name);
            if (type == AE_IFDIR) {
                std::filesystem::create_directories(path);
                continue;
            }
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            char buffer[32768];
            la_ssize_t n;
            while ((n = archive_read_data(a.get(), buffer, sizeof(buffer))) > 0) {
                total += n;
                if (total > 150 * 1024 * 1024)
                    throw std::runtime_error("Extension exceeds the extraction limit");
                out.write(buffer, n);
            }
            if (n < 0 || !out)
                throw std::runtime_error("Cannot extract extension package");
        }
        if (next != ARCHIVE_EOF)
            throw std::runtime_error("Corrupted extension ZIP payload");
        return {manifest(dir, id, base64(public_key)), dir};
    } catch (...) {
        std::filesystem::remove_all(dir);
        throw;
    }
}
ExtensionManager::ExtensionManager(std::filesystem::path profile) : root_(profile / "extensions") {
    std::filesystem::create_directories(root_);
    bool registry_read = !std::filesystem::exists(root_ / "registry.toml");
    try {
        auto registry = toml::parse_file(utf8_path(root_ / "registry.toml"));
        auto list = registry["extensions"].as_array();
        if (!list)
            throw std::runtime_error("Invalid extension registry");
        for (auto &node : *list)
            if (auto row = node.as_table()) {
                InstalledExtension e;
                e.id = (*row)["id"].value_or("");
                e.name = (*row)["name"].value_or("");
                e.version = (*row)["version"].value_or("");
                e.directory = (*row)["directory"].value_or("");
                e.enabled = (*row)["enabled"].value_or(false);
                e.load_requested = e.enabled;
                e.runtime_version = e.enabled ? e.version : "";
                e.pinned = (*row)["pinned"].value_or(false);
                e.icon = (*row)["icon"].value_or("");
                if (!e.icon.empty() && !safe_path(e.icon))
                    e.icon.clear();
                e.popup = (*row)["popup"].value_or("");
                e.options = (*row)["options"].value_or("");
                e.source = (*row)["source"].value_or("");
                if (auto p = (*row)["permissions"].as_array())
                    for (auto &v : *p)
                        if (auto s = v.value<std::string>())
                            e.permissions.push_back(*s);
                if (!valid_id(e.id) || !safe_path(e.directory) ||
                    !e.directory.starts_with(e.id + "-") ||
                    !std::filesystem::is_regular_file(root_ / e.directory / "manifest.json"))
                    throw std::runtime_error("Invalid extension registry");
                items.push_back(e);
            } else
                throw std::runtime_error("Invalid extension registry");
        registry_read = true;
    } catch (const std::exception &) {
        if (!registry_read)
            status = "Could not read the extension registry. Installed files were preserved.";
    }
    // Remove abandoned staging packages and packages removed in the previous session.
    std::set<std::string> keep;
    for (const auto &e : items)
        keep.insert(e.directory);
    for (const auto &e : std::filesystem::directory_iterator(root_)) {
        const auto name = e.path().filename().string();
        const auto suffix = name.starts_with("pending-") ? name.substr(8)
                            : name.size() > 33 && valid_id(name.substr(0, 32)) && name[32] == '-'
                                ? name.substr(33)
                                : "";
        const bool managed =
            !suffix.empty() &&
            std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (e.is_directory() && managed && !keep.contains(name) &&
            (registry_read || name.starts_with("pending-")))
            std::filesystem::remove_all(e.path());
    }
}
ExtensionManager::~ExtensionManager() {
    shutdown();
}
void ExtensionManager::save() const {
    toml::array list;
    for (const auto &e : items) {
        toml::array permissions;
        for (auto &p : e.permissions)
            permissions.push_back(p);
        list.push_back(toml::table{{"id", e.id},
                                   {"name", e.name},
                                   {"version", e.version},
                                   {"directory", e.directory},
                                   {"enabled", e.enabled},
                                   {"popup", e.popup},
                                   {"pinned", e.pinned},
                                   {"icon", e.icon},
                                   {"options", e.options},
                                   {"source", e.source},
                                   {"permissions", permissions}});
    }
    auto temporary = root_ / "registry.toml.tmp";
    {
        std::ofstream out(temporary);
        out << toml::table{{"extensions", list}};
        if (!out)
            throw std::runtime_error("Cannot save extension registry");
    }
    replace_file(temporary, root_ / "registry.toml");
}
std::string ExtensionManager::enabled_paths() const {
    std::string out;
    for (const auto &e : items)
        if (e.enabled) {
            if (!out.empty())
                out += ',';
            out += utf8_path(root_ / e.directory);
        }
    return out;
}
void ExtensionManager::unpack(std::string bytes, std::string id, std::string source) {
    auto directory =
        root_ /
        ("pending-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        future_ = std::async(std::launch::async, [bytes = std::move(bytes), id, source, directory] {
            auto c = inspect_crx(bytes, id, directory);
            c.extension.source = source;
            return c;
        });
    } catch (...) {
        busy = false;
        status = "Cannot start extension verification.";
    }
}
class ExtensionFetch final : public CefURLRequestClient {
  public:
    ExtensionFetch(ExtensionManager &manager, std::string id)
        : manager_(manager), id_(std::move(id)), generation_(manager.request_generation_) {}
    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        if (generation_ != manager_.request_generation_)
            return;
        if (manager_.discard_download_) {
            manager_.busy = false;
            return;
        }
        auto response = request->GetResponse();
        if (request->GetRequestStatus() != UR_SUCCESS || !response ||
            response->GetStatus() != 200 ||
            !request->GetRequest()->GetURL().ToString().starts_with("https://")) {
            manager_.status = "Could not download this extension. Check its Web Store availability "
                              "and your connection.";
            manager_.busy = false;
        } else
            manager_.unpack(std::move(bytes_), id_, "Chrome Web Store");
    }
    void OnDownloadData(CefRefPtr<CefURLRequest> request, const void *data, size_t size) override {
        if (generation_ != manager_.request_generation_) {
            request->Cancel();
            return;
        }
        if (bytes_.size() + size > 50 * 1024 * 1024) {
            request->Cancel();
            return;
        }
        bytes_.append((const char *)data, size);
    }
    void OnDownloadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    void OnUploadProgress(CefRefPtr<CefURLRequest>, int64_t, int64_t) override {}
    bool GetAuthCredentials(bool, const CefString &, int, const CefString &, const CefString &,
                            CefRefPtr<CefAuthCallback>) override {
        return false;
    }

  private:
    ExtensionManager &manager_;
    std::string id_, bytes_;
    uint64_t generation_;
    IMPLEMENT_REFCOUNTING(ExtensionFetch);
};
void ExtensionManager::fetch(const std::string &input) {
    if (busy)
        return;
    reject();
    discard_download_ = false;
    ++request_generation_;
    status.clear();
    busy = true;
    auto id = chrome_store_id(input);
    if (id.empty()) {
        try {
            auto path = path_from_utf8(input);
            if (!path.is_absolute() || path.extension() != ".crx" ||
                !std::filesystem::is_regular_file(path) ||
                std::filesystem::file_size(path) > 50 * 1024 * 1024) {
                busy = false;
                status = "Enter a Chrome Web Store URL, extension ID or absolute .crx file path.";
                return;
            }
            const auto directory =
                root_ /
                ("pending-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            future_ = std::async(std::launch::async, [path, directory] {
                auto candidate = inspect_crx(read_crx_file(path), "", directory);
                candidate.extension.source = "Local CRX3";
                return candidate;
            });
        } catch (...) {
            busy = false;
            status = "Cannot read this extension file.";
        }
        return;
    }
    status = "Downloading and verifying the extension...";
    auto req = CefRequest::Create();
    req->SetURL("https://clients2.google.com/service/update2/crx?response=redirect&prodversion=" +
                chromium_version() + "&acceptformat=crx3&x=id%3D" + id + "%26uc");
    req->SetFlags(UR_FLAG_NONE);
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    request_ = CefURLRequest::Create(req, new ExtensionFetch(*this, id), nullptr);
    if (!request_) {
        busy = false;
        status = "Could not start extension download.";
    }
}
void ExtensionManager::cancel() {
    if (!busy)
        return;
    discard_download_ = true;
    ++request_generation_;
    if (request_)
        request_->Cancel();
    status = "Extension operation cancelled.";
    if (!future_.valid())
        busy = false;
}
void ExtensionManager::check_update(size_t index) {
    if (index >= items.size() || busy)
        return;
    if (items[index].source != "Chrome Web Store") {
        status = "Choose a newer signed CRX file to update this local extension.";
        return;
    }
    fetch(items[index].id);
}
std::vector<std::string> ExtensionManager::added_permissions() const {
    if (!candidate)
        return {};
    auto permissions = candidate->extension.permissions;
    for (const auto &e : items)
        if (e.id == candidate->extension.id) {
            std::erase_if(permissions, [&](const auto &p) {
                return std::find(e.permissions.begin(), e.permissions.end(), p) !=
                       e.permissions.end();
            });
            break;
        }
    return permissions;
}
void ExtensionManager::tick() {
    if (busy && request_ && !future_.valid() && std::chrono::steady_clock::now() >= deadline_) {
        cancel();
        status = "Extension download timed out. Try again.";
    }
    if (future_.valid() && future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto result = future_.get();
            if (discard_download_) {
                std::error_code error;
                std::filesystem::remove_all(result.staging, error);
            } else {
                candidate = std::make_unique<ExtensionCandidate>(std::move(result));
                status = "Review permissions before installing.";
                for (const auto &old : items)
                    if (old.id == candidate->extension.id) {
                        auto order =
                            compare_extension_versions(candidate->extension.version, old.version);
                        if (!order || *order <= 0) {
                            reject();
                            status = order && *order == 0
                                         ? "This extension is already up to date."
                                         : "An older extension version was rejected.";
                        } else
                            status = "Review the update and its new permissions.";
                        break;
                    }
            }
        } catch (const std::exception &e) {
            if (!discard_download_)
                status = e.what();
        }
        busy = false;
        request_ = nullptr;
    }
}
void ExtensionManager::accept() {
    if (!candidate || busy)
        return;
    auto e = candidate->extension;
    for (const auto &old : items)
        if (old.id == e.id) {
            auto order = compare_extension_versions(e.version, old.version);
            if (!order || *order <= 0) {
                reject();
                status = order && *order == 0 ? "This extension is already up to date."
                                              : "An older extension version was rejected.";
                return;
            }
            e.pinned = old.pinned;
            e.enabled = old.enabled;
            e.load_requested = old.load_requested;
            e.observed = old.observed;
            e.runtime_version = old.runtime_version;
            e.pending_update = true;
        }
    auto before = items;
    e.directory = e.id + "-" + candidate->staging.filename().string().substr(8);
    const auto staging = candidate->staging, target = root_ / e.directory;
    bool moved = false;
    try {
        std::filesystem::rename(staging, target);
        moved = true;
        std::erase_if(items, [&](const auto &x) { return x.id == e.id; });
        items.push_back(e);
        save();
        candidate.reset();
        restart_required = true;
        status = "Installed. Restart Aura to load the extension.";
    } catch (...) {
        items = std::move(before);
        if (moved) {
            std::error_code error;
            std::filesystem::rename(target, staging, error);
            if (error)
                candidate.reset();
        }
        status = "Cannot save extension changes. Previous settings were kept.";
    }
}
void ExtensionManager::reject() {
    if (candidate) {
        std::error_code error;
        std::filesystem::remove_all(candidate->staging, error);
        candidate.reset();
        status = "Installation cancelled.";
    }
}
void ExtensionManager::toggle(size_t i) {
    if (i >= items.size() || busy)
        return;
    items[i].enabled = !items[i].enabled;
    try {
        save();
        restart_required = true;
        status = "Restart Aura to apply extension changes.";
    } catch (...) {
        items[i].enabled = !items[i].enabled;
        status = "Cannot save extension changes. Previous settings were kept.";
    }
}
bool ExtensionManager::observe_origin(const std::string &origin) {
    if (!origin.starts_with("chrome-extension://"))
        return false;
    auto id = origin.substr(19, 32);
    if (origin.size() > 51 && origin[51] != '/')
        return false;
    for (auto &e : items)
        if (e.id == id && e.load_requested && !e.observed) {
            e.observed = true;
            return true;
        }
    return false;
}
void ExtensionManager::pin(size_t index) {
    if (index >= items.size())
        return;
    items[index].pinned = !items[index].pinned;
    try {
        save();
    } catch (...) {
        items[index].pinned = !items[index].pinned;
        status = "Cannot save extension changes. Previous settings were kept.";
    }
}
std::filesystem::path ExtensionManager::icon_path(size_t index) const {
    if (index >= items.size())
        return {};
    const auto &e = items[index];
    auto icon = e.icon;
    if (icon.empty()) {
        auto m =
            CefParseJSON(read_resource(root_ / e.directory / "manifest.json"), JSON_PARSER_RFC);
        if (m && m->GetType() == VTYPE_DICTIONARY)
            icon = manifest_icon(m->GetDictionary());
    }
    return !icon.empty() && safe_path(icon) ? root_ / e.directory / icon : std::filesystem::path{};
}
void ExtensionManager::remove(size_t i) {
    if (i >= items.size())
        return;
    auto before = items;
    items.erase(items.begin() + i);
    try {
        save();
        restart_required = true;
        status = "Removed. Restart Aura to finish unloading the extension.";
    } catch (...) {
        items = std::move(before);
        status = "Cannot save extension changes. Previous settings were kept.";
    }
}
void ExtensionManager::shutdown() {
    discard_download_ = true;
    ++request_generation_;
    if (request_)
        request_->Cancel();
    request_ = nullptr;
    if (future_.valid()) {
        try {
            auto c = future_.get();
            std::filesystem::remove_all(c.staging);
        } catch (...) {
        }
    }
    reject();
}
std::string ExtensionManager::report() const {
    std::string out = "{\"busy\":" + std::string(busy ? "true" : "false") +
                      ",\"restart\":" + (restart_required ? "true" : "false") +
                      ",\"status\":" + json_quote(status) + ",\"candidate\":" +
                      (candidate ? json_quote(candidate->extension.name) : "null") + ",\"items\":[";
    bool first = true;
    for (const auto &e : items) {
        if (!first)
            out += ',';
        first = false;
        out += "{\"id\":" + json_quote(e.id) + ",\"name\":" + json_quote(e.name) +
               ",\"enabled\":" + (e.enabled ? "true" : "false") +
               ",\"loaded\":" + (e.observed ? "true" : "false") +
               ",\"load_requested\":" + (e.load_requested ? "true" : "false") +
               ",\"runtime_version\":" + json_quote(e.runtime_version) +
               ",\"observed\":" + (e.observed ? "true" : "false") +
               ",\"pending_update\":" + (e.pending_update ? "true" : "false") +
               ",\"version\":" + json_quote(e.version) +
               ",\"pinned\":" + (e.pinned ? "true" : "false") + ",\"icon\":" + json_quote(e.icon) +
               "}";
    }
    out += "],\"candidate_version\":" +
           (candidate ? json_quote(candidate->extension.version) : "null") +
           ",\"added_permissions\":[";
    first = true;
    for (const auto &permission : added_permissions()) {
        if (!first)
            out += ",";
        first = false;
        out += json_quote(permission);
    }
    return out + "]}";
}
} // namespace aura
