#include "common/diagnostics.hpp"
#include "common/update_key.hpp"
#include <algorithm>
#include <deque>
#include <mutex>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
namespace aura {
std::string diagnostic_event_code(std::string_view message) {
    if (message.starts_with("CEF browser created:"))
        return "browser.created";
    if (message.starts_with("CEF OnBeforeClose"))
        return "browser.closed";
    if (message.starts_with("Main frame loaded;"))
        return "page.loaded";
    if (message.starts_with("Main frame load failed:"))
        return "page.load_failed";
    if (message.starts_with("Renderer terminated:"))
        return "renderer.terminated";
    if (message.starts_with("OSR paint failed:"))
        return "paint.failed";
    if (message.starts_with("Download request"))
        return "download.requested";
    if (message.starts_with("Download "))
        return "download.updated";
    if (message.starts_with("MV3 diagnostic:"))
        return "console.probe";
    return "runtime.event";
}
namespace {
std::mutex events_mutex;
std::deque<std::string> events;
class EventSink final : public spdlog::sinks::base_sink<std::mutex> {
    spdlog::sinks::rotating_file_sink_mt file_;

  public:
    explicit EventSink(const std::filesystem::path &path) : file_(path.native(), 1024 * 1024, 2) {
        file_.set_pattern("%v");
    }

  protected:
    void sink_it_(const spdlog::details::log_msg &message) override {
        auto code = diagnostic_event_code({message.payload.data(), message.payload.size()});
        {
            std::lock_guard lock(events_mutex);
            events.push_back(code);
            if (events.size() > 128)
                events.pop_front();
        }
        spdlog::details::log_msg clean(message.logger_name, message.level, code);
        file_.log(clean);
    }
    void flush_() override { file_.flush(); }
};
} // namespace
std::shared_ptr<spdlog::logger> diagnostic_logger(const std::filesystem::path &profile,
                                                  bool verbose) {
    const auto directory = profile / "diagnostics";
    std::filesystem::create_directories(directory);
    {
        std::lock_guard lock(events_mutex);
        events.clear();
    }
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<EventSink>(directory / "events.log")};
    if (verbose)
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (directory / "verbose.log").native(), 4 * 1024 * 1024, 2));
    auto logger = std::make_shared<spdlog::logger>("aura", sinks.begin(), sinks.end());
    logger->flush_on(spdlog::level::info);
    return logger;
}
std::vector<std::string> diagnostic_events() {
    std::lock_guard lock(events_mutex);
    return {events.begin(), events.end()};
}
std::string diagnostic_report(const DiagnosticSummary &s) {
    std::string json = "{\"schema\":1,\"product\":\"Aura\",\"version\":\"" +
                       std::string(browser_version) + "\",\"platform\":\"";
#ifdef _WIN32
    json += "windows";
#else
    json += "linux";
#endif
    json += "\",\"safe_mode\":" + std::string(s.safe_mode ? "true" : "false") +
            ",\"verbose_logging\":" + (s.verbose ? "true" : "false") + ",\"renderer\":\"" +
            (s.gpu ? "gpu" : "raster") +
            "\",\"windows\":" + std::to_string(std::max(0, s.windows)) +
            ",\"tabs\":" + std::to_string(std::max(0, s.tabs)) +
            ",\"renderer_crashes\":" + std::to_string(std::max(0, s.renderer_crashes)) +
            ",\"cef_created\":" + std::to_string(std::max(0, s.created)) +
            ",\"cef_closed\":" + std::to_string(std::max(0, s.closed)) + ",\"events\":[";
    bool first = true;
    for (const auto &event : diagnostic_events()) {
        if (!first)
            json += ',';
        first = false;
        json += '"' + event + '"';
    }
    return json + "]}";
}
bool diagnostic_method_allowed(std::string_view method, bool safe_mode) {
    if (!safe_mode)
        return true;
    // An allowlist prevents future browser features from accidentally injecting code.
    for (auto allowed : {"Runtime.enable", "Page.enable", "WebAudio.enable", "Target.getTargets",
                         "Target.getTargetInfo", "Page.getFrameTree", "Page.getLayoutMetrics",
                         "Page.captureScreenshot"})
        if (method == allowed)
            return true;
    return false;
}
} // namespace aura
