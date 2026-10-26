#include "common/diagnostics.hpp"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <spdlog/logger.h>
using namespace aura;
TEST(Diagnostics, ScriptGateFailsClosedForUnknownMethods) {
    for (auto method : {"Runtime.evaluate", "Runtime.callFunctionOn",
                        "Page.addScriptToEvaluateOnNewDocument", "Page.createIsolatedWorld",
                        "Runtime.addBinding", "Page.setDocumentContent", "Unknown.newInjection"}) {
        EXPECT_FALSE(diagnostic_method_allowed(method, true));
        EXPECT_TRUE(diagnostic_method_allowed(method, false));
    }
    EXPECT_TRUE(diagnostic_method_allowed("Runtime.enable", true));
    EXPECT_TRUE(diagnostic_method_allowed("Page.getFrameTree", true));
}
TEST(Diagnostics, JournalAndExportNeverIncludeFreeFormPayloads) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aura-private-diag-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto logger = diagnostic_logger(path, false);
    for (auto prefix : {"CEF browser created:", "Main frame load failed:", "Renderer terminated:",
                        "MV3 diagnostic:", "unknown"})
        logger->info(
            "{} https://user:PASS@example.test/?token=SECRET /home/NAME/private document-title",
            prefix);
    logger->flush();
    logger.reset();
    std::ifstream f(path / "diagnostics/events.log");
    std::string log((std::istreambuf_iterator<char>(f)), {});
    f.close();
    auto report = diagnostic_report({.safe_mode = true, .windows = 2, .tabs = 3});
    for (const auto &text : {log, report})
        for (auto secret : {"https:", "PASS", "example.test", "SECRET", "NAME", "document-title"})
            EXPECT_EQ(text.find(secret), std::string::npos) << text;
    EXPECT_NE(log.find("page.load_failed"), std::string::npos);
    EXPECT_NE(report.find("\"safe_mode\":true"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(path / "diagnostics/verbose.log"));
    std::filesystem::remove_all(path);
}
TEST(Diagnostics, RawMessagesRequireExplicitVerboseLogging) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aura-verbose-diag-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto logger = diagnostic_logger(path, true);
    logger->info("explicit-debug-marker");
    logger->flush();
    logger.reset();
    std::ifstream f(path / "diagnostics/verbose.log");
    std::string text((std::istreambuf_iterator<char>(f)), {});
    f.close();
    EXPECT_NE(text.find("explicit-debug-marker"), std::string::npos);
    EXPECT_EQ(diagnostic_report({.verbose = true}).find("explicit-debug-marker"),
              std::string::npos);
    std::filesystem::remove_all(path);
}
