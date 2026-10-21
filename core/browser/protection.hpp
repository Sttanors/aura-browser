#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
namespace aura {
// Shared by the CEF UI and IO threads. The engine and its policy outlive requests.
class Protection {
  public:
    struct RuleStats {
        uint64_t network = 0, cosmetic = 0, skipped = 0;
    };
    struct Prepared {
        Prepared() = default;
        Prepared(const Prepared &) = delete;
        Prepared &operator=(const Prepared &) = delete;
        void *handle = nullptr;
        RuleStats stats;
        ~Prepared();
    };
    static std::shared_ptr<Prepared> prepare(const std::string &rules);
    static RuleStats inspect(const std::string &rules);
    void install(std::shared_ptr<Prepared> rules);
    explicit Protection(const std::filesystem::path &resources);
    ~Protection();
    void configure(bool enabled, bool cosmetics, std::string exceptions);
    enum class Action { Allow, Block, NoopScript, NoopStyle, TransparentImage };
    struct Decision {
        Action action = Action::Allow;
        std::string rewritten;
    };
    Decision decide(const std::string &url, const std::string &source, int resource_type,
                    const std::string &method = "GET");
    bool block(const std::string &url, const std::string &source, int resource_type);
    std::string selectors(const std::string &url, const std::string &classes = "[]",
                          const std::string &ids = "[]");
    bool active(const std::string &url) const;
    std::atomic<unsigned long> blocked{0}, rewritten_requests{0};
    RuleStats rule_stats() const {
        std::lock_guard lock(mutex_);
        return compiled_ ? compiled_->stats : RuleStats{};
    }
    bool ready() const {
        std::lock_guard lock(mutex_);
        return engine_ != nullptr;
    }

  private:
    void *engine_ = nullptr;
    std::shared_ptr<Prepared> compiled_;
    mutable std::mutex mutex_;
    bool enabled_ = true, cosmetics_ = true;
    std::string exceptions_;
};
bool site_exception(const std::string &url, const std::string &exceptions);
std::string toggle_site_exception(const std::string &url, const std::string &exceptions);
std::string read_resource(const std::filesystem::path &path);
std::string json_quote(const std::string &value);
} // namespace aura
