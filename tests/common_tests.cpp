#include "aura/design_tokens.hpp"
#include "common/config.hpp"
#include "common/file_lock.hpp"
#include "common/geometry.hpp"
#include "common/paint_buffer.hpp"
#include "common/platform.hpp"
#include "common/pump_schedule.hpp"
#include "core/browser/profiles.hpp"
#include "core/browser/translation.hpp"
#include <climits>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <thread>

using namespace aura;
TEST(PaintBuffer, FirstPaintOwnsAllPixelsEvenWhenDirtyRegionIsSmall) {
    PaintBuffer buffer;
    std::vector<std::uint8_t> source(4 * 3 * 4, 17);
    const Rect dirty{1, 1, 1, 1};
    buffer.update(4, 3, source, {&dirty, 1});
    source[0] = 99;
    EXPECT_EQ(buffer.bytes()[0], 17);
    EXPECT_EQ(buffer.width(), 4);
    EXPECT_EQ(buffer.generation(), 1);
}
TEST(PaintBuffer, ClipsDirtyRectanglesAndPreservesUnchangedPixels) {
    PaintBuffer buffer;
    std::vector<std::uint8_t> source(3 * 2 * 4, 1);
    buffer.update(3, 2, source, {});
    std::fill(source.begin(), source.end(), 7);
    const Rect dirty{-1, -1, 2, 2};
    buffer.update(3, 2, source, {&dirty, 1});
    EXPECT_EQ(buffer.bytes()[0], 7);
    EXPECT_EQ(buffer.bytes()[4], 1);
    EXPECT_EQ(buffer.bytes()[12], 1);
}
TEST(PaintBuffer, ResizeReplacesEntireBackingStore) {
    PaintBuffer buffer;
    std::vector<std::uint8_t> first(16, 1), second(24, 2);
    buffer.update(2, 2, first, {});
    const Rect dirty{0, 0, 1, 1};
    buffer.update(3, 2, second, {&dirty, 1});
    EXPECT_EQ(buffer.bytes().size(), 24);
    EXPECT_EQ(buffer.bytes().back(), 2);
}
TEST(PaintBuffer, RejectsInvalidSizesBeforeAllocation) {
    PaintBuffer buffer;
    EXPECT_THROW(buffer.update(-1, 1, {}, {}), std::invalid_argument);
    EXPECT_THROW(buffer.update(INT_MAX, INT_MAX, {}, {}), std::invalid_argument);
    EXPECT_THROW(buffer.update(16384, 16384, {}, {}), std::invalid_argument);
    EXPECT_THROW(buffer.update(2, 2, {}, {}), std::invalid_argument);
    EXPECT_EQ(buffer.generation(), 0);
}
TEST(Geometry, ClipsOverflowingCoordinatesWithoutSignedOverflow) {
    const auto rect = intersect({INT_MAX - 1, 0, 100, 10}, {0, 0, 100, 100});
    EXPECT_EQ(rect.width, 0);
}
TEST(Geometry, ConvertsFractionalDpi) {
    EXPECT_EQ(pixels_from_dip(101, 1.25), 126);
    EXPECT_EQ(pixels_from_dip(101, 1.5), 152);
    EXPECT_THROW(pixels_from_dip(1, 0), std::invalid_argument);
    EXPECT_THROW(pixels_from_dip(1, std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
}
TEST(PumpSchedule, EarlierWorkCannotBePostponedByALaterRequest) {
    PumpSchedule scheduler;
    const auto now = PumpSchedule::Clock::now();
    scheduler.schedule(5, now);
    scheduler.schedule(100, now);
    EXPECT_EQ(scheduler.wait_ms(now), 5);
    EXPECT_FALSE(scheduler.take_due(now));
    EXPECT_TRUE(scheduler.take_due(now + std::chrono::milliseconds(5)));
    EXPECT_FALSE(scheduler.take_due(now + std::chrono::milliseconds(6)));
}
TEST(PumpSchedule, NegativeDelayIsImmediateAndNewWorkSurvivesConsumption) {
    PumpSchedule scheduler;
    const auto now = PumpSchedule::Clock::now();
    scheduler.schedule(-1, now);
    EXPECT_TRUE(scheduler.take_due(now));
    scheduler.schedule(0, now);
    EXPECT_TRUE(scheduler.take_due(now));
}
TEST(PumpSchedule, EarlierWorkWakesTheHostFromAnotherThreadWithoutHoldingTheLock) {
    int wakes = 0;
    PumpSchedule *host = nullptr;
    const auto now = PumpSchedule::Clock::now();
    PumpSchedule schedule([&] {
        ++wakes;
        EXPECT_GE(host->wait_ms(now), 0); // Re-entering must not deadlock.
    });
    host = &schedule;
    schedule.schedule(10, now);
    schedule.schedule(20, now);
    EXPECT_EQ(wakes, 1);
    schedule.schedule(5, now);
    EXPECT_EQ(wakes, 2);
    std::thread worker([&] { schedule.schedule(0, now); });
    worker.join();
    EXPECT_EQ(wakes, 3);
    EXPECT_TRUE(schedule.take_due(now));
    schedule.schedule(0, now);
    EXPECT_EQ(wakes, 4);
}
TEST(Config, MissingFileUsesDocumentedDefaults) {
    const auto config = load_config("/aura-m0-missing-config.toml");
    EXPECT_EQ(config.width, 1280);
    EXPECT_EQ(config.aura, "aurora");
}
TEST(Json, EscapesControlCharactersQuotesAndBackslashes) {
    EXPECT_EQ(json_escape("a\n\"\\\t"), "a\\u000a\\\"\\\\\\u0009");
    EXPECT_EQ(json_escape("Aura"), "Aura");
}
TEST(Design, IncludesAllSixThemeVariants) {
    EXPECT_EQ(design::themes.size(), 6);
    EXPECT_EQ(design::themes[0].id, "aurora-dark");
    EXPECT_EQ(design::themes[5].id, "monolith-light");
}

#include "common/text_editor.hpp"
#include "core/browser/profile.hpp"
#include <chrono>
TEST(TextEditor, MovesAndDeletesWholeUtf8Characters) {
    TextEditor editor;
    editor.set("Aя🙂B");
    editor.move(-1);
    editor.erase(true);
    EXPECT_EQ(editor.value, "AяB");
    editor.move(-1);
    editor.erase(false);
    EXPECT_EQ(editor.value, "AB");
}
TEST(TextEditor, ReplacesASelectionAndCollapsesArrowSelection) {
    TextEditor editor;
    editor.set("hello world", true);
    editor.insert("Aura");
    EXPECT_EQ(editor.value, "Aura");
    editor.select_all();
    editor.move(-1);
    EXPECT_EQ(editor.caret, 0);
    editor.insert("Hello ");
    EXPECT_EQ(editor.value, "Hello Aura");
}
TEST(Navigation, DistinguishesAddressesAndSearchQueries) {
    EXPECT_EQ(navigation_url("  localhost:8765/page  "), "http://localhost:8765/page");
    EXPECT_EQ(navigation_url("example.com/a"), "https://example.com/a");
    EXPECT_EQ(navigation_url("hello world"), "https://duckduckgo.com/?q=hello%20world");
    EXPECT_EQ(navigation_url("file:///tmp/A%20B.pdf"), "file:///tmp/A%20B.pdf");
    EXPECT_THROW(navigation_url("javascript:alert(1)"), std::invalid_argument);
}
TEST(Profile, PersistsSessionAndBookmarksAndExcludesPrivateHistory) {
    auto path = std::filesystem::temp_directory_path() /
                ("aura-profile-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Profile profile(path);
        profile.visit("https://public.test/", "Public", false);
        profile.visit("https://private.test/", "Private", true);
        profile.toggle_bookmark("https://public.test/", "Public");
        profile.session.push_back({"https://public.test/", true});
        profile.theme = 3;
        profile.save();
    }
    {
        Profile profile(path);
        ASSERT_EQ(profile.history.size(), 1);
        EXPECT_EQ(profile.history[0].title, "Public");
        EXPECT_TRUE(profile.bookmarked("https://public.test/"));
        ASSERT_EQ(profile.session.size(), 1);
        EXPECT_TRUE(profile.session[0].pinned);
        EXPECT_EQ(profile.theme, 3);
        EXPECT_EQ(profile.suggest("public").size(), 1);
    }
    std::filesystem::remove_all(path);
}

#include "core/browser/i18n.hpp"
#include <fstream>
#include <set>
namespace {
struct TemporaryProfile {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("aura-settings-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryProfile() {
        std::filesystem::create_directories(path);
    }
    ~TemporaryProfile() {
        std::filesystem::remove_all(path);
    }
};
} // namespace
TEST(Profile, MultipleWindowsKeepOrderSelectionAndPinnedTabs) {
    TemporaryProfile temp;
    Profile original(temp.path);
    original.windows = {{{{"https://one.test/", true}, {"https://two.test/", false}}, 1},
                        {{{"https://three.test/", false}}, 0}};
    original.save();
    Profile restored(temp.path);
    ASSERT_EQ(restored.windows.size(), 2);
    ASSERT_EQ(restored.windows[0].tabs.size(), 2);
    EXPECT_EQ(restored.windows[0].active, 1);
    EXPECT_TRUE(restored.windows[0].tabs[0].pinned);
    EXPECT_EQ(restored.windows[1].tabs[0].url, "https://three.test/");
}
TEST(Profile, SessionWindowReaderRejectsUnsupportedUrlsAndClampsSelection) {
    TemporaryProfile temp;
    std::ofstream(temp.path / "profile.toml") << R"TOML(schema=5
[[windows]]
active=999
[[windows.tabs]]
url="javascript:alert(1)"
[[windows.tabs]]
url="https://kept.test/"
pinned=true
[[windows]]
active=-2
[[windows.tabs]]
url="data:text/html,no"
)TOML";
    Profile restored(temp.path);
    ASSERT_EQ(restored.windows.size(), 1);
    ASSERT_EQ(restored.windows[0].tabs.size(), 1);
    EXPECT_EQ(restored.windows[0].active, 0);
    EXPECT_EQ(restored.windows[0].tabs[0].url, "https://kept.test/");
    EXPECT_TRUE(restored.windows[0].tabs[0].pinned);
}
TEST(Navigation, SelectedEngineEncodesQueriesAndLeavesAddressesAlone) {
    EXPECT_EQ(navigation_url("hello & мир", "google"),
              "https://www.google.com/search?q=hello%20%26%20%D0%BC%D0%B8%D1%80");
    EXPECT_EQ(navigation_url("hello world", "yandex"),
              "https://yandex.ru/search/?text=hello%20world");
    EXPECT_EQ(navigation_url("hello world", "bing"), "https://www.bing.com/search?q=hello%20world");
    EXPECT_EQ(navigation_url("hello world", "unknown"), navigation_url("hello world"));
    for (auto engine : {"google", "yandex", "bing", "duckduckgo"}) {
        EXPECT_EQ(navigation_url("http://localhost:1234/hello?q=world", engine),
                  "http://localhost:1234/hello?q=world");
        EXPECT_EQ(navigation_url("aura://newtab", engine), "aura://newtab");
        EXPECT_THROW(navigation_url("javascript:alert(1)", engine), std::invalid_argument);
    }
}
TEST(Profile, SettingsSurviveReloadAlongsideExistingData) {
    TemporaryProfile temp;
    {
        Profile p(temp.path);
        p.language = "ru";
        p.search_engine = "yandex";
        p.default_zoom = 125;
        p.restore = false;
        p.record_history = false;
        p.address_suggestions = false;
        p.quick_links = false;
        p.show_tab_host = false;
        p.reduce_motion = true;
        p.open_downloads = false;
        p.toggle_bookmark("https://example.com/", "Закладка");
        p.save();
    }
    Profile p(temp.path);
    EXPECT_EQ(p.language, "ru");
    EXPECT_EQ(p.search_engine, "yandex");
    EXPECT_EQ(p.default_zoom, 125);
    EXPECT_FALSE(p.restore);
    EXPECT_FALSE(p.record_history);
    EXPECT_FALSE(p.address_suggestions);
    EXPECT_FALSE(p.quick_links);
    EXPECT_FALSE(p.show_tab_host);
    EXPECT_TRUE(p.reduce_motion);
    EXPECT_FALSE(p.open_downloads);
    ASSERT_EQ(p.bookmarks.size(), 1);
    EXPECT_EQ(p.bookmarks.front().title, "Закладка");
    p.load();
    EXPECT_EQ(p.bookmarks.size(), 1);
}
TEST(Profile, LegacyProfilesKeepDefaultsAndInvalidSettingsFallBack) {
    TemporaryProfile temp;
    {
        std::ofstream file(temp.path / "profile.toml");
        file << "schema = 1\ntheme = 3\nrestore = false\n"
                "[[history]]\nurl = 'https://old.test/'\ntitle = 'Old visit'\n";
    }
    Profile legacy(temp.path);
    EXPECT_EQ(legacy.theme, 3);
    EXPECT_FALSE(legacy.restore);
    EXPECT_EQ(legacy.language, "en");
    EXPECT_EQ(legacy.search_engine, "duckduckgo");
    EXPECT_EQ(legacy.default_zoom, 100);
    EXPECT_TRUE(legacy.quick_links);
    EXPECT_TRUE(legacy.record_history);
    EXPECT_TRUE(legacy.address_suggestions);
    EXPECT_TRUE(legacy.show_tab_host);
    EXPECT_FALSE(legacy.reduce_motion);
    EXPECT_TRUE(legacy.open_downloads);
    ASSERT_EQ(legacy.history.size(), 1);
    {
        std::ofstream file(temp.path / "profile.toml");
        file << "language = 'bad'\nsearch_engine = 'javascript:alert(1)'\n"
                "default_zoom = -500\nrecord_history = 'wrong type'\n";
    }
    Profile invalid(temp.path);
    EXPECT_EQ(invalid.language, "en");
    EXPECT_EQ(invalid.search_engine, "duckduckgo");
    EXPECT_EQ(invalid.default_zoom, 100);
    EXPECT_TRUE(invalid.record_history);
}
TEST(Profile, PrivacyPreferencesControlFutureVisitsAndSuggestionsWithoutDeletingData) {
    TemporaryProfile temp;
    Profile p(temp.path);
    p.visit("https://one.test/", "One", false);
    p.toggle_bookmark("https://saved.test/", "Saved");
    p.record_history = false;
    p.visit("https://two.test/", "Two", false);
    ASSERT_EQ(p.history.size(), 1);
    EXPECT_EQ(p.history.front().title, "One");
    p.address_suggestions = false;
    EXPECT_TRUE(p.suggest("").empty());
    EXPECT_TRUE(p.bookmarked("https://saved.test/"));
    p.address_suggestions = true;
    EXPECT_EQ(p.suggest("").size(), 2);
    p.record_history = true;
    p.visit("https://private.test/", "Private", true);
    p.visit("https://three.test/", "Three", false);
    ASSERT_EQ(p.history.size(), 2);
    EXPECT_EQ(p.history.front().title, "Three");
}
TEST(Language, CatalogHasUniqueCompletePairsAndPreservesUnknownContent) {
    std::set<std::string_view> keys;
    for (const auto &entry : translations()) {
        EXPECT_FALSE(entry.english.empty());
        EXPECT_FALSE(entry.russian.empty());
        EXPECT_TRUE(keys.insert(entry.english).second) << entry.english;
        EXPECT_EQ(translate("en", entry.english), entry.english);
        EXPECT_NE(translate("ru", entry.english), entry.english);
    }
    EXPECT_EQ(translate("ru", "Example page title"), "Example page title");
    EXPECT_EQ(translate("unknown", "New tab"), "New tab");
}

TEST(Preferences, CatalogRoundTripsEverySettingAndRejectsInvalidInput) {
    TemporaryProfile temp;
    BrowserPreferences defaults;
    std::set<std::string> ids;
    {
        Profile p(temp.path);
        for (const auto &spec : settings_catalog()) {
            ASSERT_TRUE(ids.insert(spec.id).second) << spec.id;
            auto value = setting_value(p, spec);
            if (spec.kind == SettingKind::Toggle)
                value = value == "true" ? "false" : "true";
            else if (spec.kind == SettingKind::Choice)
                value = spec.choices.back().value;
            else if (spec.kind == SettingKind::Number)
                value = std::to_string(spec.maximum);
            else if (spec.kind == SettingKind::Themes)
                value = "5";
            else
                value = (spec.id == "home_page" || spec.id.starts_with("ntp_link_"))
                            ? "https://example.test/home"
                            : utf8_path(temp.path / "Загрузки");
            ASSERT_TRUE(set_setting(p, spec, value)) << spec.id;
        }
        p.save();
        Profile loaded(temp.path);
        for (const auto &spec : settings_catalog()) {
            EXPECT_EQ(setting_value(loaded, spec), setting_value(p, spec)) << spec.id;
            const auto before = setting_value(p, spec);
            EXPECT_FALSE(set_setting(p, spec, "\ninvalid")) << spec.id;
            EXPECT_EQ(setting_value(p, spec), before) << spec.id;
        }
    }
    EXPECT_GE(ids.size(), 60u);
    EXPECT_TRUE(valid_home_address("https://example.test/path?q=1"));
    for (auto address : {"javascript:alert(1)", "https://", "https:///bad", "http://?q=bad",
                         "https://example.test/\nheader"})
        EXPECT_FALSE(valid_home_address(address));
    EXPECT_FALSE(set_setting(defaults, *find_setting("download_directory"), "relative/path"));
    EXPECT_FALSE(set_setting(defaults, *find_setting("minimum_font_size"), "500"));
    EXPECT_FALSE(set_setting(defaults, *find_setting("notifications"), "allow"));
}
TEST(Profile, RetentionAndSuggestionSourcesRespectPreferences) {
    TemporaryProfile temp;
    Profile p(temp.path);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    p.history = {{"https://recent.test/", "Recent", now},
                 {"https://old.test/", "Old", now - 90 * 86400LL}};
    p.bookmarks = {{"https://saved.test/", "Saved", 0}};
    p.history_days = 7;
    p.prune_history();
    ASSERT_EQ(p.history.size(), 1);
    EXPECT_EQ(p.history[0].title, "Recent");
    p.suggest_bookmarks = false;
    ASSERT_EQ(p.suggest("").size(), 1);
    EXPECT_EQ(p.suggest("")[0].title, "Recent");
    p.suggest_history = false;
    EXPECT_TRUE(p.suggest("").empty());
    p.suggest_bookmarks = true;
    ASSERT_EQ(p.suggest("").size(), 1);
    EXPECT_EQ(p.suggest("")[0].title, "Saved");
    p.history_limit = 100;
    for (int i = 0; i < 150; ++i)
        p.visit("https://example.test/" + std::to_string(i), "Page", false);
    EXPECT_EQ(p.history.size(), 100);
    EXPECT_EQ(p.history[0].url, "https://example.test/149");
}
TEST(Language, SettingsDescriptionsAndChoicesHaveRussianTranslations) {
    for (const auto &spec : settings_catalog()) {
        EXPECT_NE(translate("ru", spec.description), spec.description) << spec.id;
        if (spec.title != "JavaScript")
            EXPECT_NE(translate("ru", spec.title), spec.title) << spec.id;
        if (spec.section != "Data")
            EXPECT_NE(translate("ru", spec.section), spec.section);
    }
}

TEST(Preferences, StartPageZoomAndTextLimits) {
    BrowserPreferences p;
    EXPECT_FALSE(set_setting(p, *find_setting("ntp_zoom"), "210"));
    EXPECT_FALSE(set_setting(p, *find_setting("ntp_zoom"), "55"));
    EXPECT_TRUE(set_setting(p, *find_setting("ntp_zoom"), "200"));
    EXPECT_FALSE(set_setting(p, *find_setting("ntp_title"), std::string(121, 'a')));
    EXPECT_TRUE(set_setting(p, *find_setting("ntp_title"), "Мой браузер"));
}

TEST(Profile, DynamicShortcutsPersistOrderAndMigrateLegacySlots) {
    TemporaryProfile temp;
    {
        std::ofstream f(temp.path / "profile.toml");
        f << "ntp_link_1=\"https://legacy.test/\"\nntp_link_2=\"\"\n";
    }
    Profile p(temp.path);
    ASSERT_EQ(p.shortcuts.size(), 2u);
    EXPECT_EQ(p.shortcuts[0].url, "https://legacy.test/");
    for (int i = 0; i < 12; ++i)
        ASSERT_TRUE(p.add_shortcut("Ссылка " + std::to_string(i),
                                   "https://example.test/" + std::to_string(i)));
    EXPECT_FALSE(p.add_shortcut("Unsafe", "javascript:alert(1)"));
    EXPECT_FALSE(p.edit_shortcut(0, "Unsafe", "file:///etc/passwd"));
    EXPECT_TRUE(p.edit_shortcut(2, "Изменено", "https://edited.test/"));
    p.move_shortcut(2, 0);
    p.remove_shortcut(1);
    p.save();
    Profile loaded(temp.path);
    ASSERT_EQ(loaded.shortcuts.size(), p.shortcuts.size());
    EXPECT_EQ(loaded.shortcuts[0].title, "Изменено");
    for (size_t i = 0; i < p.shortcuts.size(); ++i)
        EXPECT_EQ(loaded.shortcuts[i].url, p.shortcuts[i].url);
    loaded.shortcuts.clear();
    loaded.save();
    EXPECT_TRUE(Profile(temp.path).shortcuts.empty());
}

TEST(Platform, ReplacesExistingUnicodeFileAndPreservesItOnFailure) {
    const auto root =
        std::filesystem::temp_directory_path() /
        path_from_utf8("aura-замена-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto target = root / path_from_utf8("профиль.toml"), temporary = root / "pending";
    {
        std::ofstream stream(target);
        stream << "old";
    }
    {
        std::ofstream stream(temporary);
        stream << "new";
    }
    std::error_code error;
    replace_file(temporary, target, error);
    EXPECT_FALSE(error);
    EXPECT_FALSE(std::filesystem::exists(temporary));
    {
        std::ifstream stream(target);
        std::string value;
        stream >> value;
        EXPECT_EQ(value, "new");
    }
    replace_file(temporary, target, error);
    EXPECT_TRUE(error);
    {
        std::ifstream stream(target);
        std::string value;
        stream >> value;
        EXPECT_EQ(value, "new");
    }
    std::filesystem::remove_all(root);
}

#ifdef _WIN32
#include <windows.h>
TEST(Platform, ReplacesFileAfterTemporaryWindowsReaderLock) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("aura-sharing-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto target = root / "target", pending = root / "pending";
    {
        std::ofstream file(target);
        file << "before";
    }
    {
        std::ofstream file(pending);
        file << "after";
    }
    const auto reader =
        CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(reader, INVALID_HANDLE_VALUE);
    std::thread release([reader] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CloseHandle(reader);
    });
    std::error_code error;
    replace_file(pending, target, error);
    release.join();
    EXPECT_FALSE(error);
    {
        std::ifstream file(target);
        std::string value;
        file >> value;
        EXPECT_EQ(value, "after");
    }
    EXPECT_FALSE(std::filesystem::exists(pending));
    std::filesystem::remove_all(root);
}
#endif

TEST(Platform, ProcessArgumentsPreserveUnicodeQuotesAndTrailingSlashes) {
    auto root = std::filesystem::temp_directory_path() /
                ("aura-launch-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    const auto target = root / path_from_utf8("Аргументы с пробелами.txt");
    const std::vector<std::string> values = {
        "plain", "with spaces",    "quote\"inside", "C:\\path with spaces\\", "Русский",
        "",      "back\\\\\"quote"};
    std::vector<std::string> arguments = {
        "-c",
        "from pathlib import Path; import sys; p=Path(sys.argv[1]); t=p.with_suffix('.tmp'); "
        "t.write_bytes(chr(0).join(sys.argv[2:]).encode('utf-8')); t.replace(p)",
        utf8_path(target)};
    arguments.insert(arguments.end(), values.begin(), values.end());
    ASSERT_TRUE(launch_process(path_from_utf8(AURA_TEST_PYTHON), arguments));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(target) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(std::filesystem::exists(target));
    std::ifstream file(target, std::ios::binary);
    std::string actual{std::istreambuf_iterator<char>(file), {}};
    std::string expected;
    for (const auto &value : values) {
        if (&value != &values.front())
            expected += '\0';
        expected += value;
    }
    EXPECT_EQ(actual, expected);
    file.close();
    std::filesystem::remove_all(root);
    EXPECT_FALSE(launch_process(root / "nonexistent-program", {}));
}

TEST(Profile, CorruptSettingsRecoverBackupWithoutLosingOriginalBytes) {
    const auto dir = std::filesystem::temp_directory_path() /
        ("aura-recover-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Profile p(dir);
        p.language = "ru";
        p.toggle_bookmark("https://example.test/", "Preserved");
        p.save();
        p.save();
    }
    const std::string broken = "language = [\n";
    std::ofstream(dir / "profile.toml") << broken;
    {
        Profile p(dir);
        EXPECT_TRUE(p.recovered_profile);
        EXPECT_EQ(p.language, "ru");
        ASSERT_EQ(p.bookmarks.size(), 1);
        EXPECT_EQ(p.bookmarks.front().title, "Preserved");
        std::ifstream damaged(dir / "profile.toml.corrupt");
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(damaged), {}), broken);
        p.save();
    }
    std::ofstream(dir / "profile.toml") << broken;
    std::ofstream(dir / "profile.toml.bak") << broken;
    {
        Profile p(dir);
        EXPECT_TRUE(p.recovered_profile);
        EXPECT_EQ(p.language, "en");
        EXPECT_TRUE(std::filesystem::exists(dir / "profile.toml.corrupt.1"));
        p.save();
    }
    std::filesystem::remove_all(dir);
}

TEST(Navigation, ValidatesAuthorityBeforeTreatingPunctuationAsAddress) {
    EXPECT_EQ(navigation_url("[2001:db8::1]"), "https://[2001:db8::1]");
    EXPECT_EQ(navigation_url("[::1]:8765/page"), "http://[::1]:8765/page");
    EXPECT_EQ(navigation_url("intranet:8080"), "http://intranet:8080");
    EXPECT_EQ(navigation_url("пример.рф/путь"), "https://пример.рф/путь");
    EXPECT_EQ(navigation_url("example.com/a+b?q=1.5+2.5"), "https://example.com/a+b?q=1.5+2.5");
    EXPECT_EQ(navigation_url("1.5+2.5"), "https://duckduckgo.com/?q=1.5%2B2.5");
    for (const std::string input : {"[hello:world]", "[2001::db8::1]", "intranet:99999",
                                    "intranet:", "intranet:-1", "a..b", "localhostevil"})
        EXPECT_TRUE(navigation_url(input).starts_with("https://duckduckgo.com/?q=")) << input;
}

TEST(Profile, WorkspacesPersistTabsAndRemovalMovesDataToDefault) {
    auto root = std::filesystem::temp_directory_path() /
                ("aura-workspaces-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Profile p(root);
        int work = p.add_workspace("Работа");
        ASSERT_GT(work, 0);
        EXPECT_EQ(p.add_workspace("Работа"), -1);
        EXPECT_EQ(p.add_workspace("  "), -1);
        EXPECT_EQ(p.add_workspace("Bad\nName"), -1);
        p.windows = {
            {{{"https://example.test/", true, work, true}, {"https://personal.test/", false, 0}},
             0,
             work}};
        ASSERT_TRUE(p.rename_workspace(work, "Работа и учёба"));
        p.save();
    }
    {
        Profile p(root);
        ASSERT_EQ(p.workspaces.size(), 2);
        ASSERT_EQ(p.windows.size(), 1);
        const int work = p.workspaces.back().id;
        EXPECT_EQ(p.workspaces.back().name, "Работа и учёба");
        EXPECT_EQ(p.windows[0].workspace, work);
        EXPECT_EQ(p.windows[0].tabs[0].workspace, work);
        EXPECT_TRUE(p.windows[0].tabs[0].pinned);
        EXPECT_TRUE(p.windows[0].tabs[0].workspace_active);
        EXPECT_FALSE(p.remove_workspace(0));
        EXPECT_TRUE(p.remove_workspace(work));
        EXPECT_EQ(p.windows[0].workspace, 0);
        EXPECT_EQ(p.windows[0].tabs[0].workspace, 0);
        EXPECT_EQ(p.windows[0].tabs.size(), 2);
        p.save();
    }
    EXPECT_EQ(Profile(root).workspaces.size(), 1);
    std::filesystem::remove_all(root);
}

TEST(Profile, GroupsPersistNamesColorsCollapseAndReleaseTabsOnRemoval) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("aura-groups-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Profile p(root);
        const int work = p.add_workspace("Work");
        const int group = p.add_group(work, "Исследование", 3);
        ASSERT_GT(group, 0);
        EXPECT_EQ(p.add_group(work, "Исследование"), -1);
        EXPECT_EQ(p.add_group(999, "Invalid"), -1);
        p.group(group)->collapsed = true;
        EXPECT_TRUE(p.rename_group(group, "Источники"));
        p.windows = {{{{"https://example.test/", false, work, true, group}}, 0, work}};
        p.save();
    }
    {
        Profile p(root);
        ASSERT_EQ(p.groups.size(), 1);
        const auto group = p.groups[0];
        EXPECT_EQ(group.name, "Источники");
        EXPECT_EQ(group.color, 3);
        EXPECT_TRUE(group.collapsed);
        ASSERT_EQ(p.windows[0].tabs[0].group, group.id);
        EXPECT_TRUE(p.remove_workspace(group.workspace));
        EXPECT_EQ(p.groups[0].workspace, 0);
        EXPECT_EQ(p.windows[0].tabs[0].workspace, 0);
        EXPECT_TRUE(p.remove_group(group.id));
        EXPECT_EQ(p.windows[0].tabs[0].group, 0);
        EXPECT_EQ(p.windows[0].tabs[0].url, "https://example.test/");
        p.save();
    }
    EXPECT_TRUE(Profile(root).groups.empty());
    std::filesystem::remove_all(root);
}

TEST(Profile, LocalizedDefaultWorkspaceIsReservedButCustomNamesStayLiteral) {
    TemporaryProfile temporary;
    Profile p(temporary.path);
    p.language = "ru";
    EXPECT_EQ(p.workspace_label(0), "Личное пространство");
    EXPECT_EQ(p.add_workspace("Личное пространство"), -1);
    const int id = p.add_workspace("History");
    ASSERT_GT(id, 0);
    EXPECT_EQ(p.workspace_label(id), "History");
    EXPECT_TRUE(p.rename_workspace(0, "Моё"));
    EXPECT_GT(p.add_workspace("Личное пространство"), 0);
    p.save();
    Profile restored(temporary.path);
    EXPECT_EQ(restored.workspace_label(0), "Моё");
    EXPECT_EQ(restored.workspace_label(id), "History");
}

TEST(Profile, SplitSessionRoundTripsAndRejectsInvalidPaneIndices) {
    TemporaryProfile temporary;
    {
        Profile p(temporary.path);
        p.windows = {{{{"https://left.test/"}, {"https://right.test/"}}, 1, 0, 0, 1}};
        p.split_percent = 65;
        p.save();
    }
    {
        Profile p(temporary.path);
        ASSERT_EQ(p.windows.size(), 1u);
        EXPECT_EQ(p.windows[0].split_left, 0);
        EXPECT_EQ(p.windows[0].split_right, 1);
        EXPECT_EQ(p.split_percent, 65);
        p.windows[0].split_right = 500;
        p.save();
    }
    {
        Profile p(temporary.path);
        ASSERT_EQ(p.windows.size(), 1u);
        EXPECT_EQ(p.windows[0].split_left, -1);
        EXPECT_EQ(p.windows[0].split_right, -1);
        EXPECT_EQ(p.windows[0].tabs.size(), 2u);
        p.windows[0].split_left = 0;
        p.windows[0].split_right = 0;
        p.save();
    }
    Profile p(temporary.path);
    EXPECT_EQ(p.windows[0].split_left, -1);
    EXPECT_EQ(p.windows[0].split_right, -1);
}

TEST(Profile, ContainersPersistAndRemovalNeverReassignsSavedIdentity) {
    TemporaryProfile temporary;
    int work = 0;
    {
        Profile p(temporary.path);
        work = p.add_container("Work", 2);
        ASSERT_GT(work, 0);
        EXPECT_LT(p.add_container("work"), 0);
        EXPECT_LT(p.add_container("По умолчанию"), 0);
        EXPECT_LT(p.add_container("Bad\nName"), 0);
        EXPECT_FALSE(p.remove_container(0));
        EXPECT_TRUE(p.rename_container(work, "Работа", 4));
        p.windows = {{{{"https://personal.test/", false, 0, false, 0, 0},
                       {"https://work.test/", false, 0, false, 0, work},
                       {"https://other.test/", false, 0, false, 0, 0}},
                      2,
                      0,
                      0,
                      2}};
        p.session = p.windows.front().tabs;
        p.active = 2;
        p.save();
    }
    {
        Profile p(temporary.path);
        ASSERT_NE(p.container(work), nullptr);
        EXPECT_EQ(p.container(work)->name, "Работа");
        EXPECT_EQ(p.container(work)->color, 4);
        ASSERT_EQ(p.windows.size(), 1u);
        ASSERT_EQ(p.windows[0].tabs.size(), 3u);
        EXPECT_EQ(p.windows[0].tabs[1].container, work);
        EXPECT_TRUE(p.remove_container(work));
        ASSERT_EQ(p.windows[0].tabs.size(), 2u);
        EXPECT_EQ(p.windows[0].active, 1);
        EXPECT_EQ(p.windows[0].tabs[1].url, "https://other.test/");
        EXPECT_EQ(p.windows[0].split_left, -1);
        EXPECT_EQ(p.session.size(), 2u);
        EXPECT_GT(p.add_container("Replacement"), work);
        p.save();
    }
    Profile p(temporary.path);
    EXPECT_EQ(p.container(work), nullptr);
    ASSERT_EQ(p.container_trash.size(), 1u);
    EXPECT_EQ(p.container_trash[0], work);
    EXPECT_EQ(p.windows[0].tabs.size(), 2u);
}
TEST(Profile, ContainerCleanupIsDeferredAndDoesNotFollowDirectorySymlinks) {
    TemporaryProfile temporary;
    Profile p(temporary.path);
    const int id = p.add_container("Work");
    const auto data = p.container_data_directory(id);
    std::filesystem::create_directories(data);
    {
        std::ofstream file(data / "fixture");
        file << "private site data";
    }
    ASSERT_TRUE(p.remove_container(id));
    p.save();
    EXPECT_TRUE(std::filesystem::exists(data / "fixture"));
    EXPECT_TRUE(p.cleanup_container_data());
    EXPECT_FALSE(std::filesystem::exists(data));
    EXPECT_TRUE(p.container_trash.empty());
#ifndef _WIN32
    const int next = p.add_container("Next");
    const auto outside = temporary.path / "outside";
    std::filesystem::create_directories(outside);
    {
        std::ofstream file(outside / "keep");
        file << "unrelated data";
    }
    std::filesystem::create_directory_symlink(outside, p.container_data_directory(next));
    ASSERT_TRUE(p.remove_container(next));
    EXPECT_FALSE(p.cleanup_container_data());
    EXPECT_TRUE(std::filesystem::exists(outside / "keep"));
    ASSERT_EQ(p.container_trash.size(), 1u);
#endif
}
TEST(Profile, MissingContainerCatalogDoesNotRestoreTabsInDefaultIdentity) {
    TemporaryProfile temporary;
    std::ofstream file(temporary.path / "profile.toml");
    file << "[[session]]\nurl='https://isolated.test/"
            "'\ncontainer=7\n[[windows]]\nactive=0\n[[windows.tabs]]\nurl='https://isolated.test/"
            "'\ncontainer=7\n[[windows.tabs]]\nurl='https://normal.test/'\n";
    file.close();
    Profile p(temporary.path);
    EXPECT_TRUE(p.session.empty());
    ASSERT_EQ(p.windows.size(), 1u);
    ASSERT_EQ(p.windows[0].tabs.size(), 1u);
    EXPECT_EQ(p.windows[0].tabs[0].url, "https://normal.test/");
    EXPECT_EQ(p.windows[0].tabs[0].container, 0);
}

TEST(Profiles, FreshProfilesShareOnlyLanguageAndCatalogMembership) {
    TemporaryProfile temporary;
    Profile parent(temporary.path);
    parent.language = "ru";
    parent.search_engine = "yandex";
    parent.toggle_bookmark("https://parent.test/", "Parent");
    parent.visit("https://parent.test/", "Parent", false);
    parent.save();
    std::ofstream(temporary.path / "passwords.aura") << "parent vault bytes";
    std::filesystem::create_directories(temporary.path / "extensions" / "parent-extension");
    ProfileCatalog catalog(temporary.path);
    const int id = catalog.create(" Work ", "ru");
    ASSERT_GT(id, 0);
    const auto directory = catalog.data_directory(id);
    Profile child(directory);
    EXPECT_EQ(child.language, "ru");
    EXPECT_EQ(child.search_engine, "duckduckgo");
    EXPECT_TRUE(child.bookmarks.empty());
    EXPECT_TRUE(child.history.empty());
    EXPECT_FALSE(std::filesystem::exists(directory / "passwords.aura"));
    EXPECT_FALSE(std::filesystem::exists(directory / "extensions"));
    ProfileCatalog discovered(directory, child.profiles_root);
    EXPECT_EQ(discovered.root, catalog.root);
    EXPECT_EQ(discovered.current_id, id);
    EXPECT_EQ(discovered.name(id, "en"), "Work");
    child.search_engine = "bing";
    child.save();
    EXPECT_EQ(Profile(temporary.path).search_engine, "yandex");
    EXPECT_EQ(Profile(directory).search_engine, "bing");
    EXPECT_EQ(Profile(temporary.path).history.size(), 1);
}

TEST(Profiles, ConcurrentCatalogWritersReloadAndRespectExclusiveLock) {
    TemporaryProfile temporary;
    ProfileCatalog first(temporary.path), second(temporary.path);
    const int one = first.create("One", "en");
    const int two = second.create("Two", "en");
    ASSERT_GT(one, 0);
    ASSERT_GT(two, one);
    first.reload();
    EXPECT_EQ(first.items.size(), 3);
    EXPECT_LT(first.create(" one ", "en"), 0);
    EXPECT_LT(first.create("Основной профиль", "en"), 0);
    EXPECT_FALSE(first.rename(two, "One"));
    {
        ExclusiveFileLock held(first.root / "catalog.lock");
        ASSERT_TRUE(held.owns_lock());
        EXPECT_THROW(second.create("Blocked", "en"), std::runtime_error);
        EXPECT_THROW(second.rename(one, "Changed"), std::runtime_error);
    }
    EXPECT_TRUE(second.rename(one, "Renamed"));
    first.reload();
    EXPECT_EQ(first.name(one, "en"), "Renamed");
    std::filesystem::create_directories(first.data_directory(two + 1));
    EXPECT_EQ(first.create("Three", "en"), two + 2);
}

TEST(Profiles, ArchivingIsReversibleAndNeverDeletesProfileFiles) {
    TemporaryProfile temporary;
    ProfileCatalog catalog(temporary.path);
    const int id = catalog.create("Work", "en");
    const auto directory = catalog.data_directory(id);
    std::ofstream(directory / "sentinel") << "keep";
    EXPECT_FALSE(catalog.archive(0, true));
    Profile child(directory);
    ProfileCatalog own(directory, child.profiles_root);
    EXPECT_FALSE(own.archive(id, true));
    EXPECT_TRUE(catalog.archive(id, true));
    own.reload();
    EXPECT_TRUE(own.items.back().archived);
    EXPECT_TRUE(std::filesystem::exists(directory / "sentinel"));
    EXPECT_TRUE(own.archive(id, false));
    catalog.reload();
    EXPECT_FALSE(catalog.items.back().archived);
    EXPECT_TRUE(std::filesystem::exists(directory / "profile.toml"));
}

TEST(Profiles, CatalogRecoveryPreservesCorruptionAndRefusesUnrelatedRoots) {
    TemporaryProfile temporary;
    auto read_file = [](const std::filesystem::path &path) {
        std::ifstream input(path);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    ProfileCatalog catalog(temporary.path);
    const int id = catalog.create("Work", "en");
    ASSERT_TRUE(catalog.rename(id, "Team"));
    std::ofstream(catalog.root / "catalog.toml", std::ios::trunc) << "broken = [";
    ProfileCatalog recovered(temporary.path);
    EXPECT_EQ(recovered.name(id, "en"), "Work");
    EXPECT_TRUE(recovered.rename(id, "Recovered"));
    EXPECT_EQ(read_file(catalog.root / "catalog.toml.corrupt"), "broken = [");
    TemporaryProfile unrelated;
    ProfileCatalog outside(unrelated.path, utf8_path(catalog.root));
    EXPECT_EQ(outside.current_id, 0);
    EXPECT_NE(outside.root, catalog.root);
    std::ofstream(catalog.root / "catalog.toml", std::ios::trunc) << "broken = [";
    std::ofstream(catalog.root / "catalog.toml.bak", std::ios::trunc) << "also broken = [";
    ProfileCatalog broken(temporary.path);
    EXPECT_FALSE(broken.error.empty());
    EXPECT_THROW(broken.create("No overwrite", "en"), std::runtime_error);
    EXPECT_EQ(read_file(catalog.root / "catalog.toml"), "broken = [");
}

TEST(Translation, EncodesTextAndPageAsDataAndRejectsUnsupportedModes) {
    EXPECT_EQ(translation_target("interface", "ru"), "ru");
    EXPECT_EQ(translation_target("interface", "en"), "en");
    EXPECT_THROW(translation_target("auto", "en"), std::invalid_argument);
    EXPECT_EQ(translation_url("google", "ru", false, "Hello & world#?\nПривет"),
              "https://translate.google.com/"
              "?sl=auto&tl=ru&text=Hello%20%26%20world%23%3F%0A%D0%9F%D1%80%D0%B8%D0%B2%D0%B5%D1%"
              "82&op=translate");
    EXPECT_EQ(translation_url("bing", "en", false, "a/b #"),
              "https://www.bing.com/translator?from=auto&to=en&text=a%2Fb%20%23");
    EXPECT_EQ(translation_url("google", "de", true, "https://example.com/a?x=1&tl=ja#text"),
              "https://translate.google.com/"
              "translate?sl=auto&tl=de&u=https%3A%2F%2Fexample.com%2Fa%3Fx%3D1%26tl%3Dja%23text");
    for (const auto &url : {"file:///secret", "chrome-extension://abcdefgh/", "javascript:alert(1)",
                            "https://user:password@example.com/", "https:///bad"})
        EXPECT_THROW(translation_url("google", "en", true, url), std::invalid_argument);
    EXPECT_EQ(translation_url("bing", "zh", false, "Hello"),
              "https://www.bing.com/translator?from=auto&to=zh-Hans&text=Hello");
    EXPECT_THROW(translation_url("bing", "en", true, "https://example.com/"),
                 std::invalid_argument);
    EXPECT_THROW(translation_url("unknown", "en", false, "Hello"), std::invalid_argument);
    EXPECT_THROW(translation_url("google", "ru&u=secret", false, "Hello"), std::invalid_argument);
    EXPECT_THROW(translation_url("google", "en", false, " \n\t"), std::invalid_argument);
    EXPECT_THROW(translation_url("google", "en", false, std::string(5001, 'a')),
                 std::invalid_argument);
    EXPECT_NO_THROW(translation_url("google", "en", false, std::string(5000, 'a')));
}

TEST(Permissions, OriginsHaveExactSchemeHostPortAndNeverWildcardOrCredentials) {
    for (const auto &origin :
         {"https://example.com", "https://www.example.com:8443", "http://127.0.0.1:3030",
          "http://localhost", "http://dev.localhost:8090", "https://[2001:db8::1]:444",
          "http://[::1]", "http://127.0.0.2:9090"})
        EXPECT_TRUE(valid_permission_origin(origin)) << origin;
    for (const auto &origin : {"*",
                               "https://*.example.com",
                               "https://example.com/path",
                               "https://example.com/",
                               "https://user:pass@example.com",
                               "https://example.com?x=1",
                               "https://example.com#frag",
                               "https://EXAMPLE.com",
                               "http://localhost:80",
                               "https://example.com:443",
                               "https://example.com:65536",
                               "https://example.com:0444",
                               "https:///empty",
                               "https://example..com",
                               "https://999.999.999.999",
                               "https://test.123",
                               "https://[::::]",
                               "file:///home/user",
                               "data:text/plain,origin",
                               "http://127.000.000.001",
                               "https://example.com\\@bad.test"})
        EXPECT_FALSE(valid_permission_origin(origin)) << origin;
    for (const auto &origin :
         {"https://example.com", "http://localhost:8080", "http://local.localhost",
          "http://127.0.0.2:9000", "http://[::1]:9000"})
        EXPECT_TRUE(trustworthy_permission_origin(origin)) << origin;
    for (const auto &origin : {"http://example.com", "http://localhost.attacker.test",
                               "http://127.example.com", "http://192.168.1.1", "http://[::2]"})
        EXPECT_FALSE(trustworthy_permission_origin(origin)) << origin;
}
TEST(Permissions, NativeScopeAndContainerKeysDoNotBroadenExplicitGrants) {
    BrowserPreferences preferences;
    preferences.camera = "ask";
    const std::string top = "https://main.test", child = "https://embedded.test";
    EXPECT_EQ(permission_scope(child, top, "camera"), top);
    EXPECT_EQ(permission_scope(child, top, "geolocation"), top);
    EXPECT_EQ(permission_scope(child, top, "notifications"), child);
    EXPECT_EQ(permission_scope(child, "", "microphone"), child);
    EXPECT_TRUE(permission_scope(child, top, "unknown").empty());
    std::vector<SitePermission> normal, private_rules;
    ASSERT_TRUE(set_site_permission(normal, top, 0, "camera", "allow"));
    EXPECT_EQ(effective_permission(preferences, normal, top, 0, "camera"), "allow");
    EXPECT_EQ(effective_permission(preferences, normal, top, 1, "camera"), "ask");
    EXPECT_EQ(effective_permission(preferences, private_rules, top, 0, "camera"), "ask");
    EXPECT_EQ(effective_permission(preferences, normal, top + ":8443", 0, "camera"), "ask");
    EXPECT_EQ(effective_permission(preferences, normal, child, 0, "camera"), "ask");
    EXPECT_EQ(effective_permission(preferences, normal, top, 0, "camera_controls"), "ask");
    EXPECT_EQ(effective_permission(preferences, normal, "http://main.test", 0, "camera"), "block");
    EXPECT_FALSE(set_site_permission(normal, "http://main.test", 0, "camera", "allow"));
    EXPECT_FALSE(set_site_permission(normal, top, -1, "camera", "allow"));
    EXPECT_FALSE(set_site_permission(normal, top, 0, "filesystem", "allow"));
    EXPECT_FALSE(set_site_permission(normal, top, 0, "camera", "ALLOW"));
    EXPECT_TRUE(set_site_permission(normal, top, 0, "sensors", "ask"));
    EXPECT_EQ(effective_permission(preferences, normal, top, 0, "sensors"), "ask");
    EXPECT_TRUE(set_site_permission(normal, top, 0, "camera", "default"));
    EXPECT_EQ(permission_override(normal, top, 0, "camera"), "default");
    EXPECT_EQ(effective_permission(preferences, normal, top, 0, "camera"), "ask");
}
TEST(Permissions, ProfilePersistsResetMarkersAndDropsRemovedContainerRules) {
    TemporaryProfile temporary;
    int container = 0;
    {
        Profile profile(temporary.path);
        container = profile.add_container("Work");
        ASSERT_TRUE(set_site_permission(profile.site_permissions, "https://example.com", 0,
                                        "camera", "allow"));
        ASSERT_TRUE(set_site_permission(profile.site_permissions, "https://example.com", container,
                                        "camera", "block"));
        ASSERT_TRUE(set_site_permission(profile.site_permissions, "https://example.com", 0,
                                        "microphone", "default"));
        profile.save();
    }
    Profile restored(temporary.path);
    ASSERT_EQ(restored.site_permissions.size(), 3u);
    EXPECT_EQ(permission_override(restored.site_permissions, "https://example.com", 0, "camera"),
              "allow");
    EXPECT_EQ(
        permission_override(restored.site_permissions, "https://example.com", container, "camera"),
        "block");
    EXPECT_EQ(
        permission_override(restored.site_permissions, "https://example.com", 0, "microphone"),
        "default");
    ASSERT_TRUE(restored.remove_container(container));
    restored.save();
    Profile cleaned(temporary.path);
    ASSERT_EQ(cleaned.site_permissions.size(), 2u);
    EXPECT_EQ(permission_override(cleaned.site_permissions, "https://example.com", 0, "camera"),
              "allow");
    for (const auto &spec : permission_catalog())
        EXPECT_NE(translate("ru", spec.title), spec.title) << spec.id;
}
TEST(Permissions, MalformedOrFutureNamespaceRowsCannotBecomeDefaultProfileGrants) {
    TemporaryProfile temporary;
    std::ofstream output(temporary.path / "profile.toml");
    output << "schema=11\n"
              "[[site_permissions]]\norigin='https://"
              "good.test'\npermission='camera'\nvalue='block'\ncontainer=0\n"
              "[[site_permissions]]\norigin='https://"
              "bad-type.test'\npermission='camera'\nvalue='allow'\ncontainer='broken'\n"
              "[[site_permissions]]\norigin='https://"
              "removed.test'\npermission='camera'\nvalue='allow'\ncontainer=777\n"
              "[[site_permissions]]\norigin='https://"
              "private.test'\npermission='camera'\nvalue='allow'\ncontainer=0\nprivate=true\n"
              "[[site_permissions]]\norigin='https://"
              "future.test'\npermission='camera'\nvalue='allow'\ncontainer=0\nexpires=1\n"
              "[[site_permissions]]\norigin='https://"
              "*.all.test'\npermission='camera'\nvalue='allow'\ncontainer=0\n";
    output.close();
    Profile profile(temporary.path);
    ASSERT_EQ(profile.site_permissions.size(), 1u);
    EXPECT_EQ(profile.site_permissions[0].origin, "https://good.test");
    EXPECT_EQ(profile.site_permissions[0].value, "block");
}
