#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace aura {
struct ExtensionAccessSnapshot {
    std::string id, mode, error;
    std::vector<std::string> hosts;
    bool busy = false, available = false;
    uint64_t revision = 0;
};
// Native Chromium runtime host permissions, accessed only through its trusted WebUI.
class ExtensionSiteAccess {
  public:
    ExtensionSiteAccess();
    ~ExtensionSiteAccess();
    ExtensionSiteAccess(const ExtensionSiteAccess &) = delete;
    ExtensionSiteAccess &operator=(const ExtensionSiteAccess &) = delete;
    void request(const std::string &id, const std::string &operation = "read",
                 const std::string &value = "");
    const ExtensionAccessSnapshot &snapshot() const;
    void tick();
    void close();
    bool closed() const;
    int created_count() const;
    int closed_count() const;
    std::string report() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace aura
