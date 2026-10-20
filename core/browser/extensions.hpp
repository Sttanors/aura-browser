#pragma once
#include "include/cef_urlrequest.h"
#include <filesystem>
#include <future>
#include <string>
#include <vector>
namespace aura {
struct InstalledExtension {
    std::string id, name, version, directory, popup, options, source, icon;
    std::vector<std::string> permissions;
    bool enabled = true, loaded = false, pinned = false;
};
struct ExtensionCandidate {
    InstalledExtension extension;
    std::filesystem::path staging;
};
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
    void accept();
    void reject();
    void toggle(std::size_t index);
    void pin(std::size_t index);
    std::filesystem::path icon_path(std::size_t index) const;
    void remove(std::size_t index);
    void tick();
    void shutdown();
    std::string report() const;

  private:
    std::filesystem::path root_;
    CefRefPtr<CefURLRequest> request_;
    std::future<ExtensionCandidate> future_;
    void save() const;
    void unpack(std::string bytes, std::string id, std::string source);
    friend class ExtensionFetch;
};
} // namespace aura
