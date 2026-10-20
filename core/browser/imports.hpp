#pragma once
#include "core/browser/profile.hpp"
#include <atomic>
#include <future>
#include <memory>
namespace aura {
struct BrowserImportData {
    std::string source;
    std::vector<Visit> bookmarks, history;
    size_t skipped = 0, duplicates = 0;
    bool limited = false;
};
struct BrowserImportResult {
    size_t bookmarks_added = 0, history_added = 0, history_retained = 0, history_limited = 0,
           duplicates = 0;
    std::filesystem::path backup;
};
struct BrowserImportRoot {
    std::string browser;
    std::filesystem::path directory;
    bool firefox = false;
};
struct BrowserImportSource {
    std::string label;
    std::filesystem::path path;
};
std::vector<BrowserImportRoot> browser_import_roots();
std::vector<BrowserImportSource>
discover_browser_imports(const std::vector<BrowserImportRoot> &roots);
BrowserImportData read_browser_import(const std::filesystem::path &path,
                                      const std::shared_ptr<std::atomic_bool> &cancel);
BrowserImportResult apply_browser_import(Profile &profile, const BrowserImportData &data,
                                         bool bookmarks, bool history);
class BrowserImporter {
  public:
    ~BrowserImporter();
    std::unique_ptr<BrowserImportData> candidate;
    std::optional<BrowserImportResult> result;
    std::string status = "Choose a file to preview its bookmarks and history.";
    bool busy = false;
    void preview(const std::string &path);
    void cancel();
    bool tick();
    void apply(Profile &profile, bool bookmarks, bool history);
    void shutdown();

  private:
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::future<BrowserImportData> future_;
};
} // namespace aura
