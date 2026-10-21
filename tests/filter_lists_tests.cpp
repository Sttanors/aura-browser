#include "common/config.hpp"
#include "common/platform.hpp"
#include "core/browser/filter_lists.hpp"
#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <toml++/toml.hpp>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace aura;
namespace aura {
struct FilterListsTestAccess {
    static void fetcher(FilterLists &m,
                        std::function<std::shared_ptr<FilterTransfer>(const std::string &)> fetch) {
        m.fetch_override_ = std::move(fetch);
    }
    static void expire(FilterLists &m) {
        m.deadline_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    }
};
} // namespace aura
namespace {
std::string list(const std::string &prefix) {
    std::string text = "[Adblock Plus 2.0]\n! fictional test list\n";
    for (int i = 0; i < 120; ++i)
        text += "||" + prefix + std::to_string(i) + ".test^$script\n";
    return text;
}
void write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}
struct Fixture {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("aura-filter-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path resources = root / "resources", profile = root / "profile";
    std::shared_ptr<Protection> engine;
    Fixture() {
        std::filesystem::create_directories(resources / "protection");
        write(resources / "protection/easylist.txt", list("bundled"));
        write(resources / "protection/easyprivacy.txt", list("privacy"));
        engine = std::make_shared<Protection>();
    }
    ~Fixture() {
        std::filesystem::remove_all(root);
    }
};
void done(FilterLists &m) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (m.busy() && std::chrono::steady_clock::now() < deadline) {
        m.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_FALSE(m.busy());
}
bool blocks(const std::shared_ptr<Protection> &e, const std::string &name) {
    return e->block("https://" + name + ".test/ads.js", "https://page.test/", 3);
}
void good_fetch(FilterLists &m) {
    FilterListsTestAccess::fetcher(m, [](const std::string &url) {
        auto t = std::make_shared<FilterTransfer>();
        t->complete = t->ok = true;
        t->bytes = list(url.ends_with("easylist.txt") ? "fresh" : "tracking");
        return t;
    });
}
} // namespace
TEST(FilterLists, UpdatesBothListsKeepsCustomRulesAndReloadsVerifiedCache) {
    Fixture f;
    {
        FilterLists m(f.profile, f.resources, f.engine);
        EXPECT_TRUE(blocks(f.engine, "bundled0"));
        m.set_custom("||custom.test^$script\npage.test##.local-ad\n");
        done(m);
        ASSERT_EQ(m.revision, 1);
        good_fetch(m);
        m.check();
        done(m);
        ASSERT_EQ(m.revision, 2);
        EXPECT_FALSE(blocks(f.engine, "bundled0"));
        EXPECT_TRUE(blocks(f.engine, "fresh0"));
        EXPECT_TRUE(blocks(f.engine, "custom"));
        EXPECT_NE(f.engine->selectors("https://page.test/").find(".local-ad"), std::string::npos);
        EXPECT_FALSE(m.due());
        EXPECT_TRUE(m.due(std::time(nullptr) + 86401));
        EXPECT_GT(m.settings().updated[0], 0);
        EXPECT_GT(m.settings().updated[1], 0);
    }
    auto engine = std::make_shared<Protection>();
    FilterLists restored(f.profile, f.resources, engine);
    EXPECT_TRUE(blocks(engine, "fresh0"));
    EXPECT_TRUE(blocks(engine, "custom"));
    EXPECT_FALSE(blocks(engine, "bundled0"));
    EXPECT_EQ(restored.status, "Saved filter lists are active.");
}
TEST(FilterLists, PartialDownloadFailureAndTimeoutKeepTheWorkingEngine) {
    Fixture f;
    FilterLists m(f.profile, f.resources, f.engine);
    good_fetch(m);
    m.check();
    done(m);
    const auto before = read_resource(f.profile / "filter-lists/active.toml");
    FilterListsTestAccess::fetcher(m, [](const std::string &url) {
        auto t = std::make_shared<FilterTransfer>();
        t->complete = true;
        t->ok = url.ends_with("easylist.txt");
        t->bytes = list("uncommitted");
        return t;
    });
    m.check();
    done(m);
    EXPECT_EQ(m.revision, 1);
    EXPECT_TRUE(blocks(f.engine, "fresh0"));
    EXPECT_FALSE(blocks(f.engine, "uncommitted0"));
    EXPECT_EQ(read_resource(f.profile / "filter-lists/active.toml"), before);
    auto held = std::make_shared<FilterTransfer>();
    FilterListsTestAccess::fetcher(m, [held](auto &) { return held; });
    m.check();
    FilterListsTestAccess::expire(m);
    EXPECT_TRUE(m.tick());
    EXPECT_FALSE(m.busy());
    EXPECT_EQ(m.status, "Filter download timed out. Previous rules are kept.");
    EXPECT_TRUE(blocks(f.engine, "fresh0"));
}
TEST(FilterLists, RejectsBadHeadersUtf8TruncationAndOversizedRules) {
    EXPECT_THROW(FilterLists::validate_download("<html>Error</html>"), std::exception);
    EXPECT_THROW(FilterLists::validate_download("[Adblock Plus 2.0]\n||one.test^"), std::exception);
    EXPECT_THROW(FilterLists::validate_download(list("fresh") + std::string(1, char(0xFF))),
                 std::exception);
    EXPECT_THROW(FilterLists::validate_download(list("fresh") + std::string(32769, 'a')),
                 std::exception);
    EXPECT_THROW(FilterLists::validate_download(std::string(8 * 1024 * 1024 + 1, 'x')),
                 std::exception);
    EXPECT_EQ(FilterLists::validate_download("\xEF\xBB\xBF" + list("fresh")), list("fresh"));
}
TEST(FilterLists, FailedAtomicActivationRollsBackListChoicesAndEngine) {
    Fixture f;
    FilterLists m(f.profile, f.resources, f.engine);
    std::filesystem::create_directory(f.profile / "filter-lists/active.toml");
    m.set_list(0, false);
    done(m);
    EXPECT_EQ(m.revision, 0);
    EXPECT_TRUE(m.settings().enabled[0]);
    EXPECT_TRUE(blocks(f.engine, "bundled0"));
    for (const auto &entry : std::filesystem::directory_iterator(f.profile / "filter-lists"))
        EXPECT_FALSE(utf8_path(entry.path().filename()).starts_with("generation-"));
}
TEST(FilterLists, CancelledDownloadAndCompilationCannotApplyLaterResults) {
    Fixture f;
    FilterLists m(f.profile, f.resources, f.engine);
    auto held = std::make_shared<FilterTransfer>();
    FilterListsTestAccess::fetcher(m, [held](auto &) { return held; });
    m.check();
    m.cancel();
    held->complete = held->ok = true;
    held->bytes = list("stale");
    m.tick();
    EXPECT_FALSE(m.busy());
    EXPECT_EQ(m.revision, 0);
    m.set_custom("||cancelled.test^$script");
    m.cancel();
    done(m);
    EXPECT_EQ(m.revision, 0);
    EXPECT_TRUE(m.settings().custom.empty());
    EXPECT_FALSE(blocks(f.engine, "cancelled"));
    EXPECT_TRUE(blocks(f.engine, "bundled0"));
    EXPECT_FALSE(std::filesystem::exists(f.profile / "filter-lists/active.toml"));
}
TEST(FilterLists, CustomImportMergesDeduplicatesAndFailuresPreserveRules) {
    Fixture f;
    FilterLists m(f.profile, f.resources, f.engine);
    m.set_custom("||custom.test^$script\n");
    done(m);
    ASSERT_EQ(m.revision, 1);
    auto file = f.root / "custom rules.txt";
    write(file, "\xEF\xBB\xBF||custom.test^$script\r\n||added.test^$script\r\n");
    m.import_custom(file);
    done(m);
    ASSERT_EQ(m.revision, 2);
    EXPECT_EQ(m.settings().custom, "||custom.test^$script\n||added.test^$script\n");
    EXPECT_TRUE(blocks(f.engine, "added"));
    auto before = m.settings().custom;
    m.set_custom(std::string(1, char(0xFF)));
    done(m);
    EXPECT_EQ(m.settings().custom, before);
    EXPECT_EQ(m.revision, 2);
    write(file, std::string(1024 * 1024 + 1, 'x'));
    m.import_custom(file);
    done(m);
    EXPECT_EQ(m.settings().custom, before);
#ifndef _WIN32
    ASSERT_EQ(mkfifo((f.root / "pipe.txt").c_str(), 0600), 0);
    m.import_custom(f.root / "pipe.txt");
    done(m);
    EXPECT_EQ(m.settings().custom, before);
#endif
    m.import_custom(f.root);
    done(m);
    EXPECT_EQ(m.settings().custom, before);
    m.import_custom(f.root / "missing.txt");
    done(m);
    EXPECT_EQ(m.settings().custom, before);
    EXPECT_EQ(m.revision, 2);
}
TEST(FilterLists, ListChoicesAndBundledRecoveryPreserveCustomRules) {
    Fixture f;
    FilterLists m(f.profile, f.resources, f.engine);
    good_fetch(m);
    m.check();
    done(m);
    m.set_custom("||custom.test^$script");
    done(m);
    m.set_list(0, false);
    done(m);
    EXPECT_FALSE(blocks(f.engine, "fresh0"));
    EXPECT_TRUE(blocks(f.engine, "tracking0"));
    m.set_list(1, false);
    done(m);
    EXPECT_FALSE(m.due(std::time(nullptr) + 100000));
    EXPECT_TRUE(blocks(f.engine, "custom"));
    m.restore_bundled();
    done(m);
    EXPECT_TRUE(blocks(f.engine, "custom"));
    EXPECT_FALSE(m.settings().enabled[0]);
    m.set_list(0, true);
    done(m);
    EXPECT_TRUE(blocks(f.engine, "bundled0"));
    EXPECT_FALSE(blocks(f.engine, "fresh0"));
}
TEST(FilterLists, CorruptCacheFallsBackWithoutDeletingSavedPackages) {
    Fixture f;
    std::filesystem::path generation;
    {
        FilterLists m(f.profile, f.resources, f.engine);
        good_fetch(m);
        m.check();
        done(m);
        auto active = toml::parse(read_resource(f.profile / "filter-lists/active.toml"));
        generation = f.profile / "filter-lists" / active["generation"].value_or(std::string{});
    }
    write(generation / "0.txt", list("tampered"));
    auto engine = std::make_shared<Protection>();
    FilterLists restored(f.profile, f.resources, engine);
    EXPECT_EQ(restored.status, "Saved filter cache is damaged. Bundled lists are active.");
    EXPECT_TRUE(blocks(engine, "bundled0"));
    EXPECT_FALSE(blocks(engine, "tampered0"));
    EXPECT_TRUE(std::filesystem::exists(generation / "custom.txt"));
}
TEST(Protection, AtomicRuleSwapIsSafeDuringConcurrentNetworkChecks) {
    Fixture f;
    auto a = Protection::prepare(list("first")), b = Protection::prepare(list("second"));
    std::atomic<bool> stop = false;
    std::atomic<unsigned> checks = 0;
    std::thread requests([&] {
        while (!stop) {
            f.engine->block("https://first0.test/a.js", "https://page.test/", 3);
            ++checks;
        }
    });
    while (!checks)
        std::this_thread::yield();
    for (int i = 0; i < 1000; ++i)
        f.engine->install(i % 2 ? a : b);
    stop = true;
    requests.join();
    EXPECT_GT(checks.load(), 0u);
    EXPECT_TRUE(f.engine->ready());
}

TEST(FilterLists, PinnedBundledListsPassTheSameValidationAsDownloads) {
    for (const auto *name : {"easylist.txt", "easyprivacy.txt"}) {
        auto path = std::filesystem::path(AURA_SOURCE_DIR) / "resources/protection" / name;
        auto bytes = read_resource(path);
        EXPECT_EQ(FilterLists::validate_download(bytes), bytes);
    }
}

TEST(Protection, KnownRedirectResourcesExceptionsAndRemoveparamHaveDistinctActions) {
    Fixture f;
    f.engine->install(Protection::prepare(
        "||ads.test^$script,redirect=noop.js\n"
        "||styles.test^$stylesheet,redirect=noop.css\n"
        "||pixels.test^$image,redirect=1x1.gif\n"
        "||unknown.test^$script,redirect=unavailable.js\n"
        "||exception.test^$script,redirect=noop.js\n@@||exception.test^$script\n"
        "||track.test^$removeparam=utm_source\n"));
    auto action = [&](const std::string &url, int type) {
        return f.engine->decide(url, "https://page.test/", type).action;
    };
    EXPECT_EQ(action("https://ads.test/a.js", 3), Protection::Action::NoopScript);
    EXPECT_EQ(action("https://styles.test/a.css", 2), Protection::Action::NoopStyle);
    EXPECT_EQ(action("https://pixels.test/a.gif", 4), Protection::Action::TransparentImage);
    EXPECT_EQ(action("https://unknown.test/a.js", 3), Protection::Action::Block);
    EXPECT_EQ(action("https://exception.test/a.js", 3), Protection::Action::Allow);
    EXPECT_EQ(action("https://ads.test/a.js", 0), Protection::Action::Allow);
    auto result =
        f.engine->decide("https://track.test/a?utm_source=ad&keep=1", "https://page.test/", 13);
    EXPECT_EQ(result.action, Protection::Action::Allow);
    EXPECT_EQ(result.rewritten, "https://track.test/a?keep=1");
    EXPECT_EQ(f.engine->rewritten_requests.load(), 1);
    f.engine->configure(true, true, "page.test");
    EXPECT_EQ(action("https://ads.test/a.js", 3), Protection::Action::Allow);
}
TEST(Protection, NetworkMethodRulesUseTheActualMethod) {
    Fixture f;
    f.engine->install(Protection::prepare("||method.test^$method=post\n"));
    EXPECT_EQ(f.engine->decide("https://method.test/a", "https://page.test/", 13, "GET").action,
              Protection::Action::Allow);
    EXPECT_EQ(f.engine->decide("https://method.test/a", "https://page.test/", 13, "POST").action,
              Protection::Action::Block);
}
TEST(Protection, ProceduralRulesAreReturnedAsStructuredDataAndUnsupportedActionsAreReported) {
    Fixture f;
    f.engine->install(Protection::prepare("page.test##.promotion:has-text(Sponsored)\npage.test##."
                                          "paint:style(color: red !important)\n"));
    auto result = f.engine->selectors("https://page.test/");
    EXPECT_TRUE(result.starts_with("{")) << result;
    EXPECT_NE(result.find("has-text"), std::string::npos);
    EXPECT_NE(result.find("Sponsored"), std::string::npos);
    EXPECT_NE(result.find("style"), std::string::npos);
    EXPECT_EQ(Protection::inspect("page.test##+js(set-constant, foo, true)\n").skipped, 1);
    EXPECT_EQ(Protection::inspect("page.test##.promotion:has-text(/bad+/)\n").skipped, 1);
    EXPECT_EQ(Protection::inspect("||csp.test^$csp=script-src 'none'\n").skipped, 1);
}
