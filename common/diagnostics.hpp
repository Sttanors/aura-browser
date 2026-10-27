#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace spdlog {
class logger;
}
namespace aura {
// Free-form messages never enter the default journal or exported report.
std::string diagnostic_event_code(std::string_view message);
std::shared_ptr<spdlog::logger> diagnostic_logger(const std::filesystem::path &, bool verbose);
std::vector<std::string> diagnostic_events();
struct DiagnosticSummary {
    bool safe_mode = false, verbose = false, gpu = false;
    int windows = 0, tabs = 0, renderer_crashes = 0, created = 0, closed = 0;
};
std::string diagnostic_report(const DiagnosticSummary &);
bool diagnostic_method_allowed(std::string_view method, bool safe_mode);
} // namespace aura
