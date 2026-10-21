#pragma once
#include "core/browser/protection.hpp"
#include "include/cef_urlrequest.h"
#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <vector>
namespace aura {
struct FilterTransfer {
    bool complete = false, ok = false;
    std::string bytes;
};
class FilterLists {
  public:
    FilterLists(std::filesystem::path profile, std::filesystem::path resources,
                std::shared_ptr<Protection> protection);
    ~FilterLists();
    struct Settings {
        std::array<bool, 2> enabled{true, true};
        std::array<long long, 2> updated{0, 0};
        std::string custom;
    };
    const Settings &settings() const {
        return settings_;
    }
    Protection::RuleStats stats;
    std::string status = "Bundled filter lists are active.";
    bool busy() const;
    bool due(long long now = 0) const;
    bool tick();
    void check();
    void restore_bundled();
    uint64_t revision = 0;
    void cancel();
    void set_list(size_t index, bool enabled);
    void set_custom(std::string rules);
    void import_custom(const std::filesystem::path &path);
    void shutdown();
    std::string report() const;
    static std::string validate_download(std::string bytes);

  private:
    friend struct FilterListsTestAccess;
    struct Build {
        Settings settings;
        std::array<std::string, 2> lists;
        std::shared_ptr<Protection::Prepared> prepared;
        std::filesystem::path staging;
    };
    Settings settings_;
    std::array<std::string, 2> lists_, bundled_;
    std::filesystem::path root_;
    std::shared_ptr<Protection> protection_;
    enum class Phase { Idle, Download, Build } phase_ = Phase::Idle;
    size_t downloading_ = 0;
    long long last_attempt_ = 0;
    bool discarded_ = false;
    std::array<std::string, 2> downloaded_;
    std::shared_ptr<FilterTransfer> transfer_;
    CefRefPtr<CefURLRequest> request_;
    std::future<Build> work_;
    std::chrono::steady_clock::time_point deadline_;
    std::function<std::shared_ptr<FilterTransfer>(const std::string &)> fetch_override_;
    void fetch_next();
    void start_build(Settings settings, std::array<std::string, 2> lists,
                     std::filesystem::path import = {});
    void fail(const std::string &message);
    static Build build(const std::filesystem::path &root, Settings settings,
                       std::array<std::string, 2> lists, const std::filesystem::path &import);
};
} // namespace aura
