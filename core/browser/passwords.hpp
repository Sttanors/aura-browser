#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>
namespace aura {
void erase_secret(std::string &value);
struct Login {
    std::string origin, username, password;
};
// File operations are synchronous; the desktop calls them on a worker.
class PasswordVault {
  public:
    explicit PasswordVault(std::filesystem::path path);
    ~PasswordVault();
    PasswordVault(const PasswordVault &) = delete;
    PasswordVault &operator=(const PasswordVault &) = delete;
    bool exists() const;
    bool unlocked() const {
        return unlocked_;
    }
    const std::vector<Login> &logins() const {
        return logins_;
    }
    void create(const std::string &password);
    void unlock(const std::string &password);
    void lock();
    void save(Login login);
    void remove(size_t index);
    void replace(size_t index, Login login);
    void change_password(const std::string &password);
    void export_file(const std::filesystem::path &target, const std::string &password) const;
    size_t import_file(const std::filesystem::path &source, const std::string &password);

  private:
    std::filesystem::path path_;
    std::array<unsigned char, 32> key_{};
    std::array<unsigned char, 16> salt_{};
    std::vector<Login> logins_;
    bool unlocked_ = false;
    void persist(const std::vector<Login> &logins);
};

class PasswordManager {
  public:
    explicit PasswordManager(std::filesystem::path path) : vault(std::move(path)) {}
    ~PasswordManager();
    PasswordVault vault;
    bool busy = false;
    std::string status;
    void run(std::function<std::string()> action);
    bool tick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void lock();
    void touch();
    void shutdown();

  private:
    std::future<std::string> work_;
    std::chrono::steady_clock::time_point used_ = std::chrono::steady_clock::now();
};
} // namespace aura
