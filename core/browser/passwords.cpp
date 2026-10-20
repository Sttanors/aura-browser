#include "core/browser/passwords.hpp"
#include "common/platform.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace aura {
void erase_secret(std::string &s) {
    if (!s.empty())
        OPENSSL_cleanse(s.data(), s.size());
    s.clear();
}
namespace {
constexpr std::string_view magic = "AURA-VAULT-1\n";
constexpr size_t max_bytes = 16 * 1024 * 1024, max_logins = 10000;
struct Secret {
    std::string bytes;
    ~Secret() {
        erase_secret(bytes);
    }
};
struct Key {
    std::array<unsigned char, 32> bytes{};
    ~Key() {
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
};
using Salt = std::array<unsigned char, 16>;
void random_bytes(unsigned char *p, size_t n) {
    if (RAND_bytes(p, static_cast<int>(n)) != 1)
        throw std::runtime_error("Cannot create secure random data.");
}
void derive(const std::string &password, const Salt &salt, Key &key) {
    if (password.empty() || password.size() > 4096)
        throw std::runtime_error("Enter a password of at most 4096 bytes.");
    // Fixed work factors avoid attacker-controlled memory/CPU costs in imported files.
    if (EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(), 131072, 8, 1,
                       192ULL * 1024 * 1024, key.bytes.data(), key.bytes.size()) != 1)
        throw std::runtime_error("Cannot unlock the password vault.");
}
void new_password(const std::string &password) {
    if (std::count_if(password.begin(), password.end(),
                      [](unsigned char c) { return (c & 0xc0) != 0x80; }) < 12 ||
        password.size() > 4096)
        throw std::runtime_error("Use a password of at least 12 characters.");
}
void wipe(std::vector<Login> &items) {
    for (auto &item : items) {
        erase_secret(item.password);
        erase_secret(item.username);
        erase_secret(item.origin);
    }
    items.clear();
}
struct Items {
    std::vector<Login> values;
    ~Items() {
        wipe(values);
    }
};
void validate(const Login &login) {
    // Only canonical HTTPS origins, without paths, userinfo, query, or fragment.
    const auto &o = login.origin;
    if (!o.starts_with("https://") || o.size() > 2048 || o.size() <= 8 ||
        o.find_first_of("/?#@\\ \r\n\t", 8) != std::string::npos || login.username.size() > 4096 ||
        login.password.empty() || login.password.size() > 8192)
        throw std::runtime_error("Choose a valid HTTPS site and a nonempty password.");
    if (std::any_of(o.begin(), o.end(), [](unsigned char c) { return c < 33 || c > 126; }))
        throw std::runtime_error("Choose a valid HTTPS site and a nonempty password.");
}
std::string encode(const std::vector<Login> &items) {
    toml::array values;
    for (const auto &v : items) {
        validate(v);
        values.push_back(
            toml::table{{"origin", v.origin}, {"username", v.username}, {"password", v.password}});
    }
    std::ostringstream out;
    out << toml::table{{"format", 1}, {"logins", std::move(values)}};
    auto text = out.str();
    if (text.size() > max_bytes) {
        erase_secret(text);
        throw std::runtime_error("Password vault is too large.");
    }
    return text;
}
void decode(const std::string &text, Items &items) {
    // Never forward parser diagnostics: they can include plaintext source lines.
    try {
        auto root = toml::parse(text);
        auto values = root["logins"].as_array();
        if (root["format"].value_or(0) != 1 || !values || values->size() > max_logins)
            throw std::runtime_error("invalid");
        for (const auto &node : *values) {
            auto t = node.as_table();
            if (!t)
                throw std::runtime_error("invalid");
            auto o = (*t)["origin"].value<std::string>(), u = (*t)["username"].value<std::string>(),
                 p = (*t)["password"].value<std::string>();
            if (!o || !u || !p)
                throw std::runtime_error("invalid");
            Login item{std::move(*o), std::move(*u), std::move(*p)};
            validate(item);
            if (std::any_of(items.values.begin(), items.values.end(), [&](const auto &v) {
                    return v.origin == item.origin && v.username == item.username;
                }))
                throw std::runtime_error("invalid");
            items.values.push_back(std::move(item));
        }
    } catch (...) {
        throw std::runtime_error("This is not a supported password vault.");
    }
}
std::string read(const std::filesystem::path &path) {
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path) ||
        std::filesystem::is_symlink(path) || std::filesystem::file_size(path) > max_bytes + 128)
        throw std::runtime_error("Choose an existing Aura vault file.");
    std::ifstream in(path, std::ios::binary);
    std::string data(std::filesystem::file_size(path), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in)
        throw std::runtime_error("Cannot read the password vault.");
    return data;
}
std::string seal(const std::vector<Login> &items, const std::array<unsigned char, 32> &key,
                 const Salt &salt) {
    Secret plain{encode(items)};
    std::string header(magic);
    header.append(reinterpret_cast<const char *>(salt.data()), salt.size());
    unsigned char nonce[12];
    random_bytes(nonce, sizeof nonce);
    header.append(reinterpret_cast<char *>(nonce), sizeof nonce);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    std::string cipher(plain.bytes.size() + 16, '\0');
    int n = 0, total = 0;
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce) != 1 ||
        EVP_EncryptUpdate(ctx.get(), nullptr, &n,
                          reinterpret_cast<const unsigned char *>(header.data()),
                          static_cast<int>(header.size())) != 1 ||
        EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()), &n,
                          reinterpret_cast<const unsigned char *>(plain.bytes.data()),
                          static_cast<int>(plain.bytes.size())) != 1)
        throw std::runtime_error("Cannot encrypt the password vault.");
    total = n;
    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()) + total,
                            &n) != 1)
        throw std::runtime_error("Cannot encrypt the password vault.");
    cipher.resize(total + n);
    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, sizeof tag, tag) != 1)
        throw std::runtime_error("Cannot encrypt the password vault.");
    return header + cipher + std::string(reinterpret_cast<char *>(tag), sizeof tag);
}
void open(const std::string &data, const std::string &password, Key &key, Salt &salt,
          Items &items) {
    const size_t header = magic.size() + 16 + 12;
    if (data.size() < header + 16 || data.size() > max_bytes + 128 || !data.starts_with(magic))
        throw std::runtime_error("This is not a supported password vault.");
    std::copy_n(reinterpret_cast<const unsigned char *>(data.data() + magic.size()), salt.size(),
                salt.begin());
    derive(password, salt, key);
    auto bytes = reinterpret_cast<const unsigned char *>(data.data());
    auto length = static_cast<int>(data.size() - header - 16);
    Secret plain{std::string(length + 16, '\0')};
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    int n = 0, total = 0;
    bool ok = ctx &&
              EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.bytes.data(),
                                 bytes + magic.size() + 16) == 1 &&
              EVP_DecryptUpdate(ctx.get(), nullptr, &n, bytes, static_cast<int>(header)) == 1 &&
              EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(plain.bytes.data()),
                                &n, bytes + header, length) == 1;
    total = n;
    ok = ok &&
         EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
                             const_cast<unsigned char *>(bytes + data.size() - 16)) == 1 &&
         EVP_DecryptFinal_ex(
             ctx.get(), reinterpret_cast<unsigned char *>(plain.bytes.data()) + total, &n) == 1;
    if (!ok)
        throw std::runtime_error("Wrong password or damaged vault file.");
    plain.bytes.resize(total + n);
    decode(plain.bytes, items);
}
void write(const std::filesystem::path &target, const std::string &bytes, bool replace) {
    if (!target.is_absolute() || (!replace && std::filesystem::exists(target)))
        throw std::runtime_error("Choose a new absolute file path.");
    auto parent = target.parent_path();
    if (!std::filesystem::is_directory(parent))
        throw std::runtime_error("The destination folder does not exist.");
    unsigned char random[12];
    random_bytes(random, sizeof random);
    std::string suffix;
    for (auto b : random) {
        suffix += "0123456789abcdef"[b >> 4];
        suffix += "0123456789abcdef"[b & 15];
    }
    auto temp = parent / (".aura-vault-" + suffix);
    try {
#ifdef _WIN32
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot save the password vault.");
        DWORD count = 0;
        bool ok =
            WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
            count == bytes.size() && FlushFileBuffers(file);
        CloseHandle(file);
#else
        int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (file < 0)
            throw std::runtime_error("Cannot save the password vault.");
        size_t pos = 0;
        bool ok = true;
        while (pos < bytes.size()) {
            auto n = ::write(file, bytes.data() + pos, bytes.size() - pos);
            if (n <= 0) {
                ok = false;
                break;
            }
            pos += n;
        }
        ok = ok && fsync(file) == 0;
        ::close(file);
#endif
        if (!ok)
            throw std::runtime_error("Cannot save the password vault.");
        if (replace)
            replace_file(temp, target);
        else {
            // Exclusive rename also supports transfer files on FAT/exFAT USB drives.
#ifdef _WIN32
            if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot export the password vault.");
#else
            if (syscall(SYS_renameat2, AT_FDCWD, temp.c_str(), AT_FDCWD, target.c_str(),
                        RENAME_NOREPLACE) != 0)
                throw std::runtime_error("Cannot export the password vault.");
#endif
        }
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("Cannot save the password vault. Check the path and permissions.");
    }
}
} // namespace
PasswordVault::PasswordVault(std::filesystem::path path) : path_(std::move(path)) {}
PasswordVault::~PasswordVault() {
    lock();
}
bool PasswordVault::exists() const {
    return std::filesystem::exists(path_);
}
void PasswordVault::create(const std::string &password) {
    if (exists())
        throw std::runtime_error("A vault already exists. Unlock it first.");
    new_password(password);
    Key key;
    Salt salt;
    random_bytes(salt.data(), salt.size());
    derive(password, salt, key);
    write(path_, seal({}, key.bytes, salt), false);
    lock();
    key_ = key.bytes;
    salt_ = salt;
    unlocked_ = true;
}
void PasswordVault::unlock(const std::string &password) {
    Key key;
    Salt salt;
    Items items;
    open(read(path_), password, key, salt, items);
    lock();
    key_ = key.bytes;
    salt_ = salt;
    logins_ = std::move(items.values);
    unlocked_ = true;
}
void PasswordVault::lock() {
    wipe(logins_);
    OPENSSL_cleanse(key_.data(), key_.size());
    unlocked_ = false;
}
void PasswordVault::persist(const std::vector<Login> &items) {
    if (!unlocked_)
        throw std::runtime_error("Unlock the password vault first.");
    if (items.size() > max_logins)
        throw std::runtime_error("Password vault is too large.");
    write(path_, seal(items, key_, salt_), true);
}
void PasswordVault::save(Login login) {
    struct Guard {
        Login &v;
        ~Guard() {
            erase_secret(v.password);
        }
    } guard{login};
    validate(login);
    Items items{logins_};
    auto it = std::find_if(items.values.begin(), items.values.end(), [&](const auto &v) {
        return v.origin == login.origin && v.username == login.username;
    });
    if (it == items.values.end())
        items.values.push_back(login);
    else {
        erase_secret(it->password);
        it->password = login.password;
    }
    persist(items.values);
    wipe(logins_);
    logins_ = std::move(items.values);
}
void PasswordVault::replace(size_t index, Login login) {
    struct Guard {
        Login &v;
        ~Guard() {
            erase_secret(v.password);
        }
    } guard{login};
    if (index >= logins_.size())
        throw std::runtime_error("Select a saved login.");
    validate(login);
    Items items{logins_};
    for (size_t i = 0; i < items.values.size(); ++i)
        if (i != index && items.values[i].origin == login.origin &&
            items.values[i].username == login.username)
            throw std::runtime_error("A login with this site and username already exists.");
    erase_secret(items.values[index].password);
    items.values[index] = login;
    persist(items.values);
    wipe(logins_);
    logins_ = std::move(items.values);
}
void PasswordVault::remove(size_t index) {
    if (index >= logins_.size())
        throw std::runtime_error("Select a saved login.");
    Items items{logins_};
    erase_secret(items.values[index].password);
    items.values.erase(items.values.begin() + index);
    persist(items.values);
    wipe(logins_);
    logins_ = std::move(items.values);
}
void PasswordVault::change_password(const std::string &password) {
    if (!unlocked_)
        throw std::runtime_error("Unlock the password vault first.");
    new_password(password);
    Key key;
    Salt salt;
    random_bytes(salt.data(), salt.size());
    derive(password, salt, key);
    write(path_, seal(logins_, key.bytes, salt), true);
    OPENSSL_cleanse(key_.data(), key_.size());
    key_ = key.bytes;
    salt_ = salt;
}
void PasswordVault::export_file(const std::filesystem::path &target,
                                const std::string &password) const {
    if (!unlocked_)
        throw std::runtime_error("Unlock the password vault first.");
    new_password(password);
    Key key;
    Salt salt;
    random_bytes(salt.data(), salt.size());
    derive(password, salt, key);
    write(target, seal(logins_, key.bytes, salt), false);
}
size_t PasswordVault::import_file(const std::filesystem::path &source,
                                  const std::string &password) {
    if (!unlocked_)
        throw std::runtime_error("Unlock the password vault first.");
    Key key;
    Salt salt;
    Items incoming;
    open(read(source), password, key, salt, incoming);
    Items merged{logins_};
    size_t added = 0;
    for (auto &v : incoming.values)
        if (std::none_of(merged.values.begin(), merged.values.end(), [&](const auto &old) {
                return old.origin == v.origin && old.username == v.username;
            })) {
            merged.values.push_back(v);
            ++added;
        }
    if (!added)
        return 0;
    // An encrypted backup is never overwritten and needs the current master password.
    auto backup = path_;
    backup +=
        ".backup-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    write(backup, read(path_), false);
    persist(merged.values);
    wipe(logins_);
    logins_ = std::move(merged.values);
    return added;
}
} // namespace aura
namespace aura {
PasswordManager::~PasswordManager() {
    shutdown();
}
void PasswordManager::run(std::function<std::string()> action) {
    if (busy)
        return;
    status = "Working...";
    busy = true;
    touch();
    try {
        work_ = std::async(std::launch::async, std::move(action));
    } catch (...) {
        busy = false;
        status = "Cannot start the password operation.";
    }
}
bool PasswordManager::tick(std::chrono::steady_clock::time_point now) {
    if (busy && work_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            status = work_.get();
        } catch (const std::filesystem::filesystem_error &) {
            status = "Cannot access the password vault file.";
        } catch (const std::exception &e) {
            status = e.what();
        }
        busy = false;
        touch();
        return true;
    }
    if (!busy && vault.unlocked() && now - used_ >= std::chrono::minutes(15)) {
        lock();
        return true;
    }
    return false;
}
void PasswordManager::touch() {
    used_ = std::chrono::steady_clock::now();
}
void PasswordManager::lock() {
    if (!busy) {
        vault.lock();
        status = "Password vault locked.";
    }
}
void PasswordManager::shutdown() {
    if (work_.valid())
        work_.wait();
    busy = false;
    vault.lock();
}
} // namespace aura
