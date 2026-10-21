#pragma once
#include "include/cef_urlrequest.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>
namespace aura {
struct InstalledExtension {
    std::string id, name, version, directory, popup, options, source, icon, runtime_version;
    std::vector<std::string> permissions;
    bool enabled = true, load_requested = false, pinned = false, observed = false,
         pending_update = false;
};
struct ExtensionCandidate {
    InstalledExtension extension;
    std::filesystem::path staging;
};
std::optional<int> compare_extension_versions(const std::string &a, const std::string &b);
std::string chrome_store_id(const std::string &input);
ExtensionCandidate inspect_crx(const std::string &bytes, const std::string &expected_id,
                               const std::filesystem::path &staging);
class ExtensionManager {
  public:
    explicit ExtensionManager(std::filesystem::path profile);
    ~ExtensionManager();
    std::vector<InstalledExtension> items;
    std::unique_ptr<ExtensionCandidate> candidate;
    std::string status;
    bool busy = false, restart_required = false;
    std::string enabled_paths() const;
    void fetch(const std::string &input);
    void cancel();
    void check_update(std::size_t index);
    std::vector<std::string> added_permissions() const;
    void accept();
    void reject();
    void toggle(std::size_t index);
    void pin(std::size_t index);
    bool observe_origin(const std::string &origin);
    std::filesystem::path icon_path(std::size_t index) const;
    void remove(std::size_t index);
    void tick();
    void shutdown();
    std::string report() const;

  private:
    std::filesystem::path root_;
    CefRefPtr<CefURLRequest> request_;
    std::chrono::steady_clock::time_point deadline_;
    bool discard_download_ = false;
    uint64_t request_generation_ = 0;
    std::future<ExtensionCandidate> future_;
    void save() const;
    void unpack(std::string bytes, std::string id, std::string source);
    friend class ExtensionFetch;
};
} // namespace aura
