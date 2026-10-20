#include <thread>
#include "aura/design_tokens.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include <fstream>
#include "common/geometry.hpp"
#include "common/paint_buffer.hpp"
#include "common/pump_schedule.hpp"
#include <climits>
#include <gtest/gtest.h>
#include <limits>

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
    const auto root=std::filesystem::temp_directory_path()/path_from_utf8("aura-замена-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto target=root/path_from_utf8("профиль.toml"),temporary=root/"pending";
    {std::ofstream stream(target);stream<<"old";}
    {std::ofstream stream(temporary);stream<<"new";}
    std::error_code error;
    replace_file(temporary,target,error);
    EXPECT_FALSE(error);EXPECT_FALSE(std::filesystem::exists(temporary));
    {std::ifstream stream(target);std::string value;stream>>value;EXPECT_EQ(value,"new");}
    replace_file(temporary,target,error);
    EXPECT_TRUE(error);
    {std::ifstream stream(target);std::string value;stream>>value;EXPECT_EQ(value,"new");}
    std::filesystem::remove_all(root);
}

#ifdef _WIN32
#include <windows.h>
TEST(Platform, ReplacesFileAfterTemporaryWindowsReaderLock) {
    const auto root = std::filesystem::temp_directory_path() /
        ("aura-sharing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto target = root / "target", pending = root / "pending";
    { std::ofstream file(target); file << "before"; }
    { std::ofstream file(pending); file << "after"; }
    const auto reader = CreateFileW(target.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(reader, INVALID_HANDLE_VALUE);
    std::thread release([reader] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CloseHandle(reader);
    });
    std::error_code error;
    replace_file(pending, target, error);
    release.join();
    EXPECT_FALSE(error);
    { std::ifstream file(target); std::string value; file >> value; EXPECT_EQ(value, "after"); }
    EXPECT_FALSE(std::filesystem::exists(pending));
    std::filesystem::remove_all(root);
}
#endif
