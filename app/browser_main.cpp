#include "common/config.hpp"
#include "common/platform.hpp"
#include "core/browser/browser.hpp"
#include "include/base/cef_compiler_specific.h"
#include "include/cef_command_line.h"
#include <csignal>
#include <fstream>
#include <iostream>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <thread>
#ifdef _WIN32
#include "include/cef_sandbox_win.h"
#include "include/cef_version.h"
#include <windows.h>
#endif
namespace {
int run(const CefMainArgs &args, CefRefPtr<CefCommandLine> command, void *sandbox_info) {
    const auto binary = aura::executable_directory();
    if (command->HasSwitch("help")) {
        std::cout << "Aura browser\n  --url=URL --data-dir=PROFILE --theme=aurora-dark "
                     "--scale=1.5\n  --private --report=FILE --software-rendering (default) --gpu "
                     "(experimental)\n";
        return 0;
    }
    const auto path = command->HasSwitch("data-dir")
                          ? std::filesystem::absolute(aura::path_from_utf8(
                                command->GetSwitchValue("data-dir").ToString()))
                          : binary / "profile";
    std::filesystem::create_directories(path);
    auto logger = spdlog::basic_logger_mt("aura", (path / "aura.log").native());
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::info);
    double scale = 1;
    if (command->HasSwitch("scale")) {
        const auto input = command->GetSwitchValue("scale").ToString();
        std::size_t parsed = 0;
        scale = std::stod(input, &parsed);
        if (parsed != input.size() || !std::isfinite(scale) || scale < 1 || scale > 3)
            throw std::runtime_error("Scale must be between 1 and 3");
    }
    const bool accelerated = command->HasSwitch("gpu") && !command->HasSwitch("software-rendering");
    aura::BrowserController browser(path, binary / "fonts/InterVariable.ttf", scale, accelerated);
    aura::PumpSchedule schedule([&browser] { browser.window->wake(); });
    if (command->HasSwitch("theme")) {
        const auto theme = command->GetSwitchValue("theme").ToString();
        const std::string ids[] = {"aurora-dark", "aurora-light",  "ember-dark",
                                   "ember-light", "monolith-dark", "monolith-light"};
        auto it = std::find(std::begin(ids), std::end(ids), theme);
        if (it == std::end(ids))
            throw std::runtime_error("Unknown theme");
        browser.profile.theme = it - std::begin(ids);
    }
    std::vector<std::string> restart_arguments{"--data-dir=" + aura::utf8_path(path),
                                               "--resume-session"};
    for (const char *name : {"report", "scale", "gpu", "software-rendering"})
        if (command->HasSwitch(name)) {
            auto value = command->GetSwitchValue(name).ToString();
            if (std::string_view(name) == "report" && !value.empty())
                value = aura::utf8_path(std::filesystem::absolute(aura::path_from_utf8(value)));
            restart_arguments.push_back(std::string("--") + name +
                                        (value.empty() ? "" : "=" + value));
        }
    auto extension_paths = browser.extensions.enabled_paths();
    if (command->HasSwitch("load-extension"))
        extension_paths = command->GetSwitchValue("load-extension").ToString();
    CefRefPtr<aura::DiagnosticApp> app = new aura::DiagnosticApp(
        schedule, false, !accelerated, extension_paths.empty() ? "-" : extension_paths);
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.external_message_pump = true;
    CefString(&settings.root_cache_path) = aura::utf8_path(path / "cache");
    CefString(&settings.cache_path) = aura::utf8_path(path / "cache/default");
    settings.persist_session_cookies = true;
    CefString(&settings.accept_language_list) = browser.profile.site_language;
    CefString(&settings.log_file) = aura::utf8_path(path / "cef.log");
    if (!CefInitialize(args, settings, app, sandbox_info))
        throw std::runtime_error(
            "Chromium could not initialize. Check whether this profile is already open.");
    std::string report;
    const auto report_path =
        command->HasSwitch("report")
            ? aura::path_from_utf8(command->GetSwitchValue("report").ToString())
            : std::filesystem::path{};
    auto write_report = [&](const std::string &text) {
        if (report_path.empty())
            return;
        if (!report_path.parent_path().empty())
            std::filesystem::create_directories(report_path.parent_path());
        auto temporary = report_path;
        temporary += ".tmp";
        {
            std::ofstream out(temporary);
            out << text << '\n';
            if (!out)
                throw std::runtime_error("Cannot write browser report");
        }
        aura::replace_file(temporary, report_path);
    };
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!app->context_ready()) {
            CefDoMessageLoopWork();
            browser.window->dispatch();
            browser.window->paint();
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Chromium startup timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        browser.start(command->GetSwitchValue("url").ToString(), command->HasSwitch("private"),
                      command->HasSwitch("resume-session"));
        browser.window->show();
        auto last_report = std::chrono::steady_clock::now();
        auto last_pump = std::chrono::steady_clock::now();
        schedule.schedule(0);
        while (!browser.finished()) {
            browser.window->dispatch();
            if (schedule.take_due() ||
                std::chrono::steady_clock::now() - last_pump >= std::chrono::milliseconds(16)) {
                CefDoMessageLoopWork();
                last_pump = std::chrono::steady_clock::now();
            }
            browser.tick();
            if (!report_path.empty() &&
                std::chrono::steady_clock::now() - last_report > std::chrono::milliseconds(250)) {
                write_report(browser.report());
                last_report = std::chrono::steady_clock::now();
            }
            browser.window->paint();
            browser.window->wait(std::min(16, schedule.wait_ms()));
        }
        report = browser.report();
        browser.prepare_shutdown();
        app = nullptr;
        CefShutdown();
    } catch (...) {
        browser.request_quit(true);
        while (!browser.finished()) {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        browser.prepare_shutdown();
        app = nullptr;
        CefShutdown();
        throw;
    }
    write_report(report);
    if (browser.install_update_on_exit && !browser.updates.launch_installer()) {
        spdlog::error("Could not launch the verified Aura installer");
        std::cerr << "Aura: could not launch the verified update installer.\n";
    }
    if (browser.restart_on_exit && !browser.install_update_on_exit) {
#ifdef _WIN32
        const auto executable = binary / "aura_browser.exe";
#else
        const auto executable = binary / "aura_browser";
#endif
        if (!aura::launch_process(executable, restart_arguments)) {
            spdlog::error("Could not restart Aura");
            std::cerr << "Aura: could not restart. Open Aura again to restore the saved session.\n";
        }
    }
    spdlog::info("Aura closed; all browser callbacks completed before CefShutdown");
    spdlog::shutdown();
    return 0;
}
} // namespace
#ifdef _WIN32
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance, LPWSTR, int, void *sandbox_info,
                                    cef_version_info_t *) {
    CefMainArgs args(instance);
    const auto child = CefExecuteProcess(args, nullptr, sandbox_info);
    if (child >= 0)
        return child;
    auto command = CefCommandLine::CreateCommandLine();
    command->InitFromString(GetCommandLineW());
    try {
        return run(args, command, sandbox_info);
    } catch (const std::exception &error) {
        std::cerr << "Aura: " << error.what() << '\n';
        MessageBoxW(nullptr, CefString(error.what()).ToWString().c_str(),
                    L"Aura — initialization error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
#else
NO_STACK_PROTECTOR int main(int argc, char **argv) {
    CefMainArgs args(argc, argv);
    const auto child = CefExecuteProcess(args, nullptr, nullptr);
    if (child >= 0)
        return child;
    auto command = CefCommandLine::CreateCommandLine();
    command->InitFromArgv(argc, argv);
    try {
        return run(args, command, nullptr);
    } catch (const std::exception &error) {
        std::cerr << "Aura: " << error.what() << '\n';
        return 1;
    }
}

#endif
