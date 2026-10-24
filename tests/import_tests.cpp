#include "common/config.hpp"
#include "core/browser/imports.hpp"
#include "include/cef_api_hash.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <thread>
namespace aura {
namespace {
const auto *cef_api = cef_api_hash(CEF_API_VERSION, 0);
struct Imports : testing::Test {
    std::filesystem::path directory;
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
                    ("aura-import-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
    }
    void TearDown() override {
        std::filesystem::remove_all(directory);
    }
    std::filesystem::path file(const std::string &name, const std::string &bytes) {
        auto path = directory / path_from_utf8(name);
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        return path;
    }
    BrowserImportData read(const std::filesystem::path &path) {
        return read_browser_import(path, std::make_shared<std::atomic_bool>(false));
    }
    void database(const std::filesystem::path &path, const char *sql) {
        sqlite3 *raw = nullptr;
        ASSERT_EQ(sqlite3_open(utf8_path(path).c_str(), &raw), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        ASSERT_EQ(sqlite3_exec(db.get(), sql, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(db.get());
    }
    std::string bytes(const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
};
TEST_F(Imports, HtmlDecodesUnicodeEntitiesAndNeverImportsScriptOrUnsafeSchemes) {
    auto path = file("Закладки с пробелами.html", R"HTML(<!DOCTYPE NETSCAPE-Bookmark-file-1>
<DL><DT><H3>Folder</H3><DL><DT><A HREF="https://EXAMPLE.com/?a=1&amp;b=2" ADD_DATE="1700000000">Русский &amp; English &#x1F30D;</A>
<A href='https://example.com/?a=1&amp;b=2'>Duplicate</A>
<A HREF="javascript:alert(1)">Unsafe</A><A HREF="file:///home/private">Local</A>
<script>"<A HREF='https://hidden.test/'>Hidden</A>"</script>
<!-- <A HREF="https://comment.test/">Comment</A> -->
<a href=https://second.test/>Second <b>title</b></a></DL></DL>)HTML");
    auto original = bytes(path);
    auto data = read(path);
    ASSERT_EQ(data.bookmarks.size(), 2);
    EXPECT_EQ(data.bookmarks[0].url, "https://example.com/?a=1&b=2");
    EXPECT_EQ(data.bookmarks[0].title, "Русский & English 🌍");
    EXPECT_EQ(data.bookmarks[0].time, 1700000000);
    EXPECT_EQ(data.bookmarks[1].title, "Second title");
    EXPECT_EQ(data.skipped, 2);
    EXPECT_EQ(data.duplicates, 1);
    EXPECT_EQ(bytes(path), original);
}
TEST_F(Imports, ChromiumJsonWalksNestedFoldersAndConvertsWindowsEpoch) {
    auto data = read(file("Bookmarks", R"({"roots":{"bookmark_bar":{"type":"folder","children":[
        {"type":"folder","children":[{"type":"url","url":"https://one.test/","name":"One","date_added":"13344473600000000"}]},
        {"type":"url","url":"data:text/html,bad","name":"Unsafe"}]},
        "other":{"children":[{"url":"https://two.test/","name":"Two"}]}}})"));
    ASSERT_EQ(data.bookmarks.size(), 2);
    EXPECT_EQ(data.bookmarks[0].time, 1700000000);
    EXPECT_EQ(data.skipped, 1);
}
TEST_F(Imports, FirefoxSqliteImportsBothTypesWithoutChangingSourceBytes) {
    auto path = directory / path_from_utf8("История Firefox.sqlite");
    database(path,
             "CREATE TABLE moz_places(id INTEGER PRIMARY KEY,url TEXT,title TEXT,last_visit_date "
             "INTEGER);"
             "CREATE TABLE moz_bookmarks(id INTEGER PRIMARY KEY,fk INTEGER,type INTEGER,title "
             "TEXT,dateAdded INTEGER);"
             "INSERT INTO moz_places VALUES(1,'https://firefox.test/','Page',1700000000000000);"
             "INSERT INTO moz_places VALUES(2,'about:config','Internal',1700000000000000);"
             "INSERT INTO moz_bookmarks VALUES(1,1,1,'My bookmark',1690000000000000);"
             "INSERT INTO moz_bookmarks VALUES(2,NULL,2,'Folder',1690000000000000);");
    auto original = bytes(path);
    auto data = read(path);
    ASSERT_EQ(data.bookmarks.size(), 1);
    ASSERT_EQ(data.history.size(), 1);
    EXPECT_EQ(data.bookmarks[0].title, "My bookmark");
    EXPECT_EQ(data.history[0].time, 1700000000);
    EXPECT_EQ(bytes(path), original);
}
TEST_F(Imports, ChromiumHistoryReadsCommittedWalAndConvertsTimestamps) {
    auto path = directory / "History";
    sqlite3 *raw = nullptr;
    ASSERT_EQ(sqlite3_open(utf8_path(path).c_str(), &raw), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw, sqlite3_close);
    ASSERT_EQ(
        sqlite3_exec(
            raw,
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE urls(id INTEGER PRIMARY KEY,url TEXT,title TEXT,visit_count "
            "INTEGER,last_visit_time INTEGER);"
            "INSERT INTO urls VALUES(1,'https://chromium.test/','Recent',1,13344473600000000);"
            "INSERT INTO urls VALUES(2,'https://unvisited.test/','Unvisited',0,0);",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    auto data = read(path);
    ASSERT_EQ(data.history.size(), 1);
    EXPECT_EQ(data.history[0].time, 1700000000);
    EXPECT_EQ(data.history[0].url, "https://chromium.test/");
}
TEST_F(Imports, DiscoversKnownProfilesWithoutOpeningTheirDatabaseAndReadsCombinedChromiumData) {
    auto chrome = directory / "Chrome", firefox = directory / "Firefox";
    std::filesystem::create_directories(chrome / "Profile Empty");
    std::filesystem::create_directories(chrome / "Default");
    std::filesystem::create_directories(firefox / "Crash Reports");
    std::filesystem::create_directories(firefox / "abcdef.default");
    file("Firefox/abcdef.default/places.sqlite", "Not opened while discovering profiles");
    file("Chrome/Default/Bookmarks", R"({"roots":{"other":{"children":[]}}})");
    database(chrome / "Default/History",
             "CREATE TABLE urls(id INTEGER,url TEXT,title TEXT,visit_count INTEGER,last_visit_time "
             "INTEGER);"
             "INSERT INTO urls VALUES(1,'https://combined.test/','Combined',1,13344473600000000);");
    auto sources =
        discover_browser_imports({{"Chrome", chrome, false}, {"Firefox", firefox, true}});
    ASSERT_EQ(sources.size(), 2);
    EXPECT_EQ(sources[0].path, chrome / "Default");
    auto data = read(sources[0].path);
    EXPECT_TRUE(data.bookmarks.empty());
    ASSERT_EQ(data.history.size(), 1);
    EXPECT_EQ(data.history[0].url, "https://combined.test/");
}
#ifndef _WIN32
struct ImportEnvironment {
    const char *name;
    std::optional<std::string> previous;
    ImportEnvironment(const char *key, const std::string &value) : name(key) {
        if (const char *old = std::getenv(key))
            previous = old;
        setenv(key, value.c_str(), 1);
    }
    ~ImportEnvironment() {
        if (previous)
            setenv(name, previous->c_str(), 1);
        else
            unsetenv(name);
    }
};
TEST_F(Imports, DiscoversFirefoxXdgLegacySnapAndFlatpakWithoutDuplicateSymlinkProfiles) {
    ImportEnvironment home("HOME", utf8_path(directory));
    ImportEnvironment config("XDG_CONFIG_HOME", utf8_path(directory / "custom-config"));
    for (const auto &relative :
         {".mozilla/firefox/legacy.default",
          "custom-config/mozilla/firefox/Новый профиль.default-release",
          "snap/firefox/common/.mozilla/firefox/snap.default",
          ".var/app/org.mozilla.firefox/.mozilla/firefox/flatpak.default",
          ".var/app/org.mozilla.firefox/config/mozilla/firefox/xdg.default"}) {
        auto profile = directory / path_from_utf8(relative);
        std::filesystem::create_directories(profile);
        std::ofstream(profile / "places.sqlite") << "Discovery must not open this file";
    }
    std::filesystem::create_directory_symlink(directory / "custom-config/mozilla/firefox" /
                                                  path_from_utf8("Новый профиль.default-release"),
                                              directory /
                                                  ".mozilla/firefox/linked.default-release");
    auto sources = discover_browser_imports(browser_import_roots());
    ASSERT_EQ(sources.size(), 5);
    EXPECT_EQ(
        std::count_if(sources.begin(), sources.end(),
                      [](const auto &source) { return source.path.filename() == "snap.default"; }),
        1);
    // An empty or invalid XDG_CONFIG_HOME must fall back to ~/.config.
    std::filesystem::create_directories(directory / ".config/mozilla/firefox/fallback.default");
    file(".config/mozilla/firefox/fallback.default/places.sqlite", "Not opened");
    for (const auto *value : {"", "relative/path"}) {
        ImportEnvironment invalid("XDG_CONFIG_HOME", value);
        auto fallback = discover_browser_imports(browser_import_roots());
        EXPECT_TRUE(std::any_of(fallback.begin(), fallback.end(), [](const auto &source) {
            return source.path.filename() == "fallback.default";
        }));
    }
}
#endif
TEST_F(Imports, ApplyKeepsExistingTitlesDeduplicatesAndCreatesRecoverableBackup) {
    Profile profile(directory / "aura");
    profile.bookmarks = {{"https://existing.test/", "My title", 1}};
    profile.history = {{"https://visited.test/", "Visited", 2}};
    profile.theme = 3;
    profile.save();
    auto data = read(file("Bookmarks", R"({"roots":{"other":{"children":[
        {"url":"https://EXISTING.test/","name":"Imported title"},
        {"url":"https://new.test/","name":"New"}]}}})"));
    auto result = apply_browser_import(profile, data, true, false);
    EXPECT_EQ(result.bookmarks_added, 1);
    EXPECT_EQ(result.duplicates, 1);
    ASSERT_EQ(profile.bookmarks.size(), 2);
    EXPECT_EQ(profile.bookmarks[0].title, "My title");
    EXPECT_EQ(profile.history.size(), 1);
    Profile before(result.backup);
    ASSERT_EQ(before.bookmarks.size(), 1);
    EXPECT_EQ(before.bookmarks[0].title, "My title");
    EXPECT_EQ(before.theme, 3);
    Profile saved(profile.directory);
    EXPECT_EQ(saved.bookmarks.size(), 2);
    EXPECT_EQ(apply_browser_import(profile, data, true, false).bookmarks_added, 0);
    EXPECT_TRUE(apply_browser_import(profile, data, true, false).backup.empty());
}
TEST_F(Imports, HistoryMergeRespectsRetentionAndReportsWhatWasActuallyKept) {
    Profile profile(directory / "aura");
    profile.history_limit = 2;
    profile.history = {{"https://current.test/", "Current", 200}};
    BrowserImportData data;
    data.history = {{"https://new.test/", "New", 300}, {"https://old.test/", "Old", 100}};
    auto result = apply_browser_import(profile, data, false, true);
    EXPECT_EQ(result.history_added, 1);
    EXPECT_EQ(result.history_limited, 1);
    ASSERT_EQ(profile.history.size(), 2);
    EXPECT_EQ(profile.history[0].url, "https://new.test/");
    EXPECT_EQ(profile.history[1].url, "https://current.test/");
}
TEST_F(Imports, FailedCommitDoesNotChangeTheLiveProfile) {
    Profile profile(directory / "aura");
    profile.bookmarks = {{"https://kept.test/", "Keep", 1}};
    profile.save();
    std::filesystem::rename(profile.directory / "profile.toml",
                            profile.directory / "original.toml");
    std::filesystem::create_directory(profile.directory / "profile.toml");
    BrowserImportData data;
    data.bookmarks = {{"https://new.test/", "New", 1}};
    EXPECT_THROW(apply_browser_import(profile, data, true, false), std::exception);
    ASSERT_EQ(profile.bookmarks.size(), 1);
    EXPECT_EQ(profile.bookmarks[0].url, "https://kept.test/");
    EXPECT_TRUE(std::filesystem::is_regular_file(profile.directory / "original.toml"));
}
TEST_F(Imports, RejectsMalformedUnsupportedAndCancelledInputWithoutApplyingAnything) {
    EXPECT_THROW(read(file("broken.json", "{not json}")), std::runtime_error);
    EXPECT_THROW(read(file("other.json", R"({"passwords":[]})")), std::runtime_error);
    EXPECT_THROW(read(file("unsafe.html", "<A HREF='javascript:alert(1)'>Unsafe</A>")),
                 std::runtime_error);
    auto path = file("valid.html", "<A HREF='https://valid.test/'>Valid</A>");
    EXPECT_THROW(read_browser_import(path, std::make_shared<std::atomic_bool>(true)),
                 std::runtime_error);
    BrowserImporter importer;
    importer.preview(utf8_path(path));
    importer.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (importer.busy && std::chrono::steady_clock::now() < deadline) {
        importer.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(importer.busy);
    EXPECT_FALSE(importer.candidate);
}
TEST_F(Imports, LargeHistoryShowsTheMostRecentSubsetInsteadOfFailingTheEntireImport) {
    auto path = directory / "History-large";
    database(path,
             "CREATE TABLE urls(id INTEGER,url TEXT,title TEXT,visit_count INTEGER,last_visit_time "
             "INTEGER);"
             "BEGIN; WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<100001) "
             "INSERT INTO urls SELECT x,'https://limit.test/'||x,'Entry',1,13344473600000000+x "
             "FROM n; COMMIT;");
    auto data = read(path);
    EXPECT_TRUE(data.limited);
    ASSERT_EQ(data.history.size(), 100000);
    EXPECT_EQ(data.history.front().url, "https://limit.test/100001");
}
TEST_F(Imports, AuraHtmlRoundTripPreservesFoldersTagsAndSafeText) {
    const std::vector<Visit> links = {
        {"https://round.test/a?x=1&y=2", "<Article & text>", 123, 7, {"Русский", "A&B"}},
        {"https://round.test/root", "Root", 0}};
    auto data =
        read(file("bookmarks.html", bookmarks_html(links, {{7, "Work & research"}, {8, "Empty"}})));
    ASSERT_EQ(data.bookmarks.size(), 2);
    ASSERT_EQ(data.bookmark_folders.size(), 2);
    auto article = std::find_if(data.bookmarks.begin(), data.bookmarks.end(), [](const auto &b) {
        return b.url.find("/a?") != std::string::npos;
    });
    ASSERT_NE(article, data.bookmarks.end());
    EXPECT_EQ(article->title, "<Article & text>");
    EXPECT_EQ(article->tags, links[0].tags);
    EXPECT_GT(article->folder, 0);
    Profile target(directory / "profile");
    const int existing = target.add_bookmark_folder("Work & research");
    target.toggle_bookmark("https://existing.test/", "Existing");
    target.save();
    EXPECT_EQ(apply_browser_import(target, data, true, false).bookmarks_added, 2);
    EXPECT_EQ(target.bookmark_folders.size(), 2);
    auto restored =
        std::find_if(target.bookmarks.begin(), target.bookmarks.end(),
                     [](const auto &b) { return b.url.find("/a?") != std::string::npos; });
    ASSERT_NE(restored, target.bookmarks.end());
    EXPECT_EQ(restored->folder, existing);
    EXPECT_EQ(restored->tags, links[0].tags);
    Profile restarted(directory / "profile");
    EXPECT_EQ(restarted.bookmark_folders.size(), 2);
}
TEST_F(Imports, HtmlImportReusesFoldersRegardlessOfLatinAndCyrillicCase) {
    Profile profile(directory / "case-profile");
    const int ru = profile.add_bookmark_folder("Работа"), en = profile.add_bookmark_folder("Work");
    auto data = read(file("case-folders.html",
                          "<DL><DT><H3>работа</H3><DL><DT><A HREF='https://ru.test/'>RU</A></DL>"
                          "<DT><H3>WORK</H3><DL><DT><A HREF='https://en.test/'>EN</A></DL></DL>"));
    auto result = apply_browser_import(profile, data, true, false);
    ASSERT_EQ(result.bookmarks_added, 2);
    ASSERT_EQ(profile.bookmark_folders.size(), 2);
    EXPECT_EQ(profile.bookmarks[0].folder, ru);
    EXPECT_EQ(profile.bookmarks[1].folder, en);
    Profile restored(profile.directory);
    ASSERT_EQ(restored.bookmarks.size(), 2);
    EXPECT_EQ(restored.bookmarks[0].folder, ru);
    EXPECT_EQ(restored.bookmarks[1].folder, en);
}
TEST_F(Imports, FolderLimitRejectsImportAtomicallyInsteadOfDroppingOrganization) {
    Profile profile(directory / "full-folders");
    for (int i = 0; i < 128; ++i)
        ASSERT_GT(profile.add_bookmark_folder("Folder " + std::to_string(i)), 0);
    profile.save();
    auto before = bytes(profile.directory / "profile.toml");
    auto data = read(file("new-folder.html", "<DL><DT><H3>New</H3><DL><DT><A "
                                             "HREF='https://new.test/'>New</A></DL></DL>"));
    EXPECT_THROW(apply_browser_import(profile, data, true, false), std::runtime_error);
    EXPECT_TRUE(profile.bookmarks.empty());
    EXPECT_EQ(profile.bookmark_folders.size(), 128);
    EXPECT_EQ(bytes(profile.directory / "profile.toml"), before);
}
TEST_F(Imports, NestedHtmlFoldersAreFlattenedWithoutPromotingScriptOrUnknownLinks) {
    auto data = read(file("nested.html",
                          "<DL><p><DT><H3>Parent</H3><DL><DT><H3>Child</H3><DL><DT><A "
                          "HREF='https://nested.test/' TAGS='one, two'>Title</A></DL></DL></DL>"));
    ASSERT_EQ(data.bookmarks.size(), 1);
    ASSERT_EQ(data.bookmark_folders.size(), 2);
    EXPECT_EQ(data.bookmark_folders[1].name, "Parent / Child");
    EXPECT_EQ(data.bookmarks[0].folder, data.bookmark_folders[1].id);
    EXPECT_EQ(data.bookmarks[0].tags, (std::vector<std::string>{"one", "two"}));
}
} // namespace
} // namespace aura
