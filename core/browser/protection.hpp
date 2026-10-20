#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
namespace aura {
// Shared by the CEF UI and IO threads. The engine and its policy outlive requests.
class Protection {
  public:
    explicit Protection(const std::filesystem::path &resources);
    ~Protection();
    void configure(bool enabled, bool cosmetics, std::string exceptions);
    bool block(const std::string &url, const std::string &source, int resource_type);
    std::string selectors(const std::string &url, const std::string &classes = "[]",
                          const std::string &ids = "[]");
    bool active(const std::string &url) const;
    std::atomic<unsigned long> blocked{0};
    bool ready() const {
        return engine_ != nullptr;
    }

  private:
    void *engine_ = nullptr;
    mutable std::mutex mutex_;
    bool enabled_ = true, cosmetics_ = true;
    std::string exceptions_;
};
bool site_exception(const std::string &url, const std::string &exceptions);
std::string toggle_site_exception(const std::string &url, const std::string &exceptions);
std::string read_resource(const std::filesystem::path &path);
std::string json_quote(const std::string &value);
} // namespace aura
