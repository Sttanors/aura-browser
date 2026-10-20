#pragma once
#include "core/browser/update_policy.hpp"
#include "include/cef_urlrequest.h"
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
namespace aura {
struct UpdateTransfer {
    bool complete = false, ok = false;
    long long received = 0, limit = 0;
    std::string bytes;
    std::ofstream file;
};
class UpdateManager {
  public:
    explicit UpdateManager(std::filesystem::path profile);
    ~UpdateManager();
    std::string status = "Updates have not been checked yet.";
    std::optional<UpdateRelease> release;
    std::filesystem::path ready_file;
    std::string installation_kind() const;
    bool busy() const;
    bool due() const;
    int download_percent() const;
    void check();
    void download();
    bool tick();
    void cancel();
    void shutdown();
    // Called only after the browser and Chromium have shut down.
    bool launch_installer() const;
    bool open_download_folder() const;

  private:
    friend struct UpdateManagerTestAccess;
    std::string public_key_;
    std::function<std::shared_ptr<UpdateTransfer>(const std::string &, long long,
                                                  const std::filesystem::path &)>
        fetch_override_;
    enum class Phase { Idle, Index, Manifest, Signature, Download, Verify } phase_ = Phase::Idle;
    std::filesystem::path profile_, temporary_;
    long long highest_sequence_ = 0, last_check_ = 0;
    int download_percent_ = 0;
    std::string manifest_, selected_tag_;
    std::optional<UpdateArtifact> artifact_;
    CefRefPtr<CefURLRequest> request_;
    std::shared_ptr<UpdateTransfer> transfer_;
    std::future<void> verification_;
    std::chrono::steady_clock::time_point deadline_;
    void fetch(const std::string &url, long long limit, const std::filesystem::path &file = {});
    void save_state() const;
    void fail(const std::string &message);
};
} // namespace aura
