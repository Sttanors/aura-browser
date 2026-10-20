#include "common/config.hpp"
#include "common/pump_schedule.hpp"
#include "core/cef/diagnostic_client.hpp"
#include "render/diagnostic_scene.hpp"
#include "aura/design_tokens.hpp"
#include "include/cef_command_line.h"
#include "include/cef_version.h"
#include "include/base/cef_compiler_specific.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#ifdef _WIN32
#include "platform/windows/diagnostic_window.hpp"
#include "include/cef_sandbox_win.h"
#include <windows.h>
#else
#include "platform/linux/x11/diagnostic_window.hpp"
#include <unistd.h>
#endif

namespace aura {
namespace {
class CefSession {
public:
    explicit CefSession(DiagnosticState& state) : state_(state) {}
    ~CefSession() { shutdown(); }
    CefRefPtr<DiagnosticClient> client;
    CefRefPtr<CefBrowser> browser;
    void shutdown() noexcept {
        if (!active_) return;
        if (browser && !state_.closed) {
            state_.closing = true;
            browser->GetHost()->CloseBrowser(true);
            while (!state_.closed) {
                CefDoMessageLoopWork();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        browser = nullptr;
        client = nullptr;
        CefShutdown();
        active_ = false;
    }
private:
    DiagnosticState& state_;
    bool active_ = true;
};

std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) throw std::runtime_error("Cannot resolve executable path");
    path.resize(count);
    return std::filesystem::path(path).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}
int integer_switch(CefRefPtr<CefCommandLine> command, const char* name, int fallback, int minimum, int maximum) {
    if (!command->HasSwitch(name)) return fallback;
    const auto text = command->GetSwitchValue(name).ToString();
    std::size_t consumed = 0;
    const auto value = std::stoi(text, &consumed);
    if (consumed != text.size() || value < minimum || value > maximum)
        throw std::runtime_error(std::string("Invalid --") + name);
    return value;
}
unsigned theme_index(const std::string& name) {
    for (unsigned i = 0; i < design::themes.size(); ++i) if (design::themes[i].id == name) return i;
    throw std::runtime_error("Unknown --theme (use aurora-dark, ember-light, etc.)");
}
void write_report(const std::filesystem::path& path, const DiagnosticState& state,
                  bool captured, bool timeout, bool closed, unsigned theme) {
    std::filesystem::create_directories(path.parent_path().empty() ? "." : path.parent_path());
    std::ofstream output(path);
    if (!output) throw std::runtime_error("Cannot open report output");
    output << "{\n  \"milestone\": \"M0\",\n  \"cef\": \"" << CEF_VERSION << "\",\n"
        << "  \"skia\": \"2466dcf3937437e217e7f284afe0e1aae15891ce\",\n"
        << "  \"theme\": \"" << design::themes[theme].id << "\",\n"
        << "  \"compositor\": \"" << json_escape(state.compositor) << "\",\n"
        << "  \"graphics_renderer\": \"" << json_escape(state.graphics_renderer) << "\",\n"
        << "  \"presentation_frames\": " << state.presentation_frames << ",\n"
        << "  \"url\": \"" << json_escape(state.url) << "\",\n"
        << "  \"title\": \"" << json_escape(state.title) << "\",\n"
        << "  \"error\": \"" << json_escape(state.error) << "\",\n"
        << "  \"http_status\": " << state.http_status << ",\n"
        << "  \"paint_frames\": " << state.view.generation() << ",\n"
        << "  \"accelerated_frames\": " << state.accelerated_frames << ",\n"
        << "  \"renderer_crashes\": " << state.renderer_crashes << ",\n"
        << "  \"mv3_probe\": \"" << json_escape(state.extension_status) << "\",\n"
        << "  \"pixel_width\": " << state.view.width() << ",\n"
        << "  \"pixel_height\": " << state.view.height() << ",\n"
        << "  \"scale\": " << state.scale << ",\n" << std::boolalpha
        << "  \"loaded\": " << state.loaded << ",\n"
        << "  \"failed\": " << state.failed << ",\n"
        << "  \"snapshot_written\": " << captured << ",\n"
        << "  \"timed_out\": " << timeout << ",\n"
        << "  \"before_close_received\": " << closed << ",\n"
        << "  \"cef_shutdown_completed\": true\n}\n";
    if (!output) throw std::runtime_error("Report write failed");
}

int run(const CefMainArgs& main_args, CefRefPtr<CefCommandLine> command, void* sandbox) {
    const bool capture = command->HasSwitch("capture") || command->HasSwitch("snapshot");
#ifdef _WIN32
    const bool software = command->HasSwitch("software-rendering");
#else
    const bool software = !command->HasSwitch("gpu") || command->HasSwitch("software-rendering");
#endif
    const auto extension = command->GetSwitchValue("extension-probe").ToString();
    PumpSchedule schedule;
    CefRefPtr<DiagnosticApp> app = new DiagnosticApp(schedule, capture, software, extension);
    if (command->HasSwitch("help")) {
        std::cout << "Aura 2 / M0 diagnostics\n"
            "  --url=URL --theme=aurora-dark --config=FILE\n"
            "  --capture --snapshot=FILE.png --report=FILE.json --timeout-ms=20000\n"
            "  --scale=1.5 --capture-after-ms=1200 --software-rendering\n"
            "  --shared-texture-probe (Windows) --extension-probe=ABSOLUTE_DIR\n"
            "  --allow-unsandboxed (explicit container diagnostics only)\n";
        return 0;
    }
#ifndef _WIN32
    if (command->HasSwitch("shared-texture-probe")) throw std::runtime_error("D3D11 probe requires Windows");
#endif
    const auto binary = executable_directory();
    const auto config_path = command->HasSwitch("config")
        ? path_from_utf8(command->GetSwitchValue("config").ToString()) : binary / "default.toml";
    auto config = load_config(config_path);
    if (command->HasSwitch("url")) config.url = command->GetSwitchValue("url").ToString();
    unsigned theme = theme_index(command->HasSwitch("theme")
        ? command->GetSwitchValue("theme").ToString() : config.aura + "-dark");
    const int timeout_ms = integer_switch(command, "timeout-ms", capture ? 20000 : 0, 0, 300000);
    const int after_ms = integer_switch(command, "capture-after-ms", 1200, 100, 30000);
    const auto snapshot = std::filesystem::absolute(path_from_utf8(command->HasSwitch("snapshot")
        ? command->GetSwitchValue("snapshot").ToString() : "artifacts/m0.png"));
    const auto report = std::filesystem::absolute(path_from_utf8(command->HasSwitch("report")
        ? command->GetSwitchValue("report").ToString() : "artifacts/m0-report.json"));
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = getpid();
#endif
    const auto data_path = command->HasSwitch("data-dir")
        ? std::filesystem::absolute(path_from_utf8(command->GetSwitchValue("data-dir").ToString()))
        : std::filesystem::temp_directory_path() / ("aura2-m0-" + std::to_string(process_id));
    std::filesystem::create_directories(data_path);
    auto logger = spdlog::basic_logger_mt("aura", (data_path / "aura.log").native());
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::info);
    spdlog::info("Aura M0; CEF {}", CEF_VERSION);
    DiagnosticState state;
    state.url = config.url;
    if (!extension.empty()) state.extension_status = "not_observed";
    if (command->HasSwitch("scale")) {
        const auto text = command->GetSwitchValue("scale").ToString();
        std::size_t consumed = 0;
        state.scale = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(state.scale) || state.scale < 1 || state.scale > 3)
            throw std::runtime_error("--scale must be between 1 and 3");
    }
    DiagnosticScene scene(binary / "fonts/InterVariable.ttf");
    const auto initial_bounds = DiagnosticScene::web_bounds(config.width, config.height);
    state.view_width = initial_bounds.width;
    state.view_height = initial_bounds.height;
#ifdef _WIN32
    std::unique_ptr<DiagnosticWindow> window;
    if (!capture) window = std::make_unique<DiagnosticWindow>(main_args.instance, scene, state,
                                                           config.width, config.height, theme);
#else
    std::unique_ptr<LinuxDiagnosticWindow> window;
    if (!capture) window = std::make_unique<LinuxDiagnosticWindow>(scene, state,
                                                                config.width, config.height, theme, !software);
#endif
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.external_message_pump = true;
    settings.no_sandbox = command->HasSwitch("allow-unsandboxed");
    CefString(&settings.root_cache_path) = utf8_path(data_path / "cache");
    CefString(&settings.log_file) = utf8_path(data_path / "cef.log");
    if (settings.no_sandbox) spdlog::warn("Explicit diagnostic request: sandbox disabled");
    if (!CefInitialize(main_args, settings, app, sandbox))
        throw std::runtime_error("CefInitialize failed; see " + utf8_path(data_path / "cef.log"));
    CefSession session(state);
    const auto context_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!app->context_ready()) {
        CefDoMessageLoopWork();
        if (std::chrono::steady_clock::now() >= context_deadline) {
            app = nullptr;
            session.shutdown();
            throw std::runtime_error("CEF context initialization timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto& client = session.client;
    client = new DiagnosticClient(state);
    CefWindowInfo info;
#ifdef _WIN32
    info.SetAsWindowless(window ? window->handle() : nullptr);
    info.shared_texture_enabled = command->HasSwitch("shared-texture-probe");
#else
    info.SetAsWindowless(window ? window->handle() : 0);
#endif
    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 30;
    browser_settings.background_color = 0xFF182033;
    auto& browser = session.browser;
    browser = CefBrowserHost::CreateBrowserSync(info, client, config.url, browser_settings, nullptr, nullptr);
    if (!browser) {
        client = nullptr;
        app = nullptr;
        session.shutdown();
        throw std::runtime_error("CEF browser creation failed");
    }
    if (window) window->attach(client);
    browser->GetHost()->SetFocus(true);
    const auto started = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> loaded_at;
    bool captured = false, timed_out = false, closing = false;
    auto last_pump = std::chrono::steady_clock::now();
    schedule.schedule(0);
    while (!state.closed) {
        if (window) window->dispatch();
        if (schedule.take_due() || std::chrono::steady_clock::now() - last_pump >= std::chrono::milliseconds(16)) {
            CefDoMessageLoopWork();
            last_pump = std::chrono::steady_clock::now();
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (state.loaded && !loaded_at) loaded_at = now;
        bool request_close = state.failed && capture;
        if (window && window->wants_close()) request_close = true;
        if (capture && !captured && loaded_at && state.view.generation() &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - *loaded_at).count() >= after_ms) {
            captured = scene.snapshot(snapshot, config.width, config.height, state, theme, elapsed / 1000.0);
            if (!captured) { state.failed = true; state.error = "PNG snapshot failed"; }
            request_close = true;
        }
        if (timeout_ms && elapsed >= timeout_ms && !closing) {
            timed_out = true;
            request_close = true;
        }
        if (request_close && !closing) {
            closing = true;
            state.closing = true;
            browser->GetHost()->CloseBrowser(true);
        }
        // Close completion is mandatory: CefShutdown must never race a live browser.
        const auto wait = schedule.wait_ms();
#ifdef _WIN32
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(wait), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
#else
        if (window) {
            window->render_if_needed();
            window->wait(wait);
        } else if (wait) std::this_thread::sleep_for(std::chrono::milliseconds(wait));
#endif
    }
    if (window) theme = window->theme();
    window.reset();
    client = nullptr;
    browser = nullptr;
    app = nullptr;
    session.shutdown();
    spdlog::info("CefShutdown completed");
    write_report(report, state, captured, timed_out, state.closed, theme);
    spdlog::shutdown();
    return state.failed || timed_out || (capture && !captured) ? 1 : 0;
}
}
}

#ifdef _WIN32
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance, LPWSTR, int, void* sandbox_info,
                                    cef_version_info_t*) {
    CefMainArgs args(instance);
    const auto process_result = CefExecuteProcess(args, nullptr, sandbox_info);
    if (process_result >= 0) return process_result;
    auto command = CefCommandLine::CreateCommandLine();
    command->InitFromString(GetCommandLineW());
    try { return aura::run(args, command, sandbox_info); }
    catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Aura 2 / initialization error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
#else
NO_STACK_PROTECTOR int main(int argc, char** argv) {
    CefMainArgs args(argc, argv);
    const auto process_result = CefExecuteProcess(args, nullptr, nullptr);
    if (process_result >= 0) return process_result;
    auto command = CefCommandLine::CreateCommandLine();
    command->InitFromArgv(argc, argv);
    try { return aura::run(args, command, nullptr); }
    catch (const std::exception& error) {
        std::cerr << "Aura M0: " << error.what() << '\n';
        return 1;
    }
}
#endif
