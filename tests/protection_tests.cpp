#include "core/browser/extensions.hpp"
#include "core/browser/protection.hpp"
#include "include/cef_api_hash.h"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
using namespace aura;
namespace {
const auto *cef_api = cef_api_hash(CEF_API_VERSION, 0);
struct Temporary {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("aura-crx-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temporary() {
        std::filesystem::remove_all(path);
    }
};
std::filesystem::path fixture(const char *name) {
    return std::filesystem::path(AURA_SOURCE_DIR) / "tests/fixtures/extensions" / name;
}
std::string fixture_id() {
    auto id = read_resource(fixture("id.txt"));
    while (!id.empty() && (id.back() == '\r' || id.back() == '\n'))
        id.pop_back();
    return id;
}
} // namespace
TEST(Protection, RulesRespectTypesAndSiteExceptions) {
    Protection engine(std::filesystem::path(AURA_SOURCE_DIR) / "resources");
    ASSERT_TRUE(engine.ready());
    const std::string ad = "https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js",
                      page = "https://example.org/";
    EXPECT_TRUE(engine.block(ad, page, 3));
    EXPECT_FALSE(engine.block(ad, page, 0));
    EXPECT_FALSE(engine.block("https://example.org/news/article", page, 3));
    EXPECT_FALSE(engine.block("http://127.0.0.2/extension-blocked.js", "http://127.0.0.2/", 3));
    engine.configure(true, true, "example.org");
    EXPECT_FALSE(engine.block(ad, "https://www.example.org/", 3));
    EXPECT_TRUE(engine.block(ad, "https://other.org/", 3));
    EXPECT_FALSE(engine.block(ad, "https://badexample.org/", 0));
    engine.configure(false, true, "");
    EXPECT_FALSE(engine.block(ad, page, 3));
}
TEST(Protection, CosmeticSelectorsAndExceptions) {
    Protection engine(std::filesystem::path(AURA_SOURCE_DIR) / "resources");
    EXPECT_NE(engine.selectors("https://example.org/", "[\"ad-banner-container\"]")
                  .find(".ad-banner-container"),
              std::string::npos);
    EXPECT_NE(engine.selectors("http://127.0.0.2:12345/", "[\"ad-banner-container\"]")
                  .find(".ad-banner-container"),
              std::string::npos);
    engine.configure(true, true, "example.org");
    EXPECT_EQ(engine.selectors("https://example.org/", "[\"ad-banner-container\"]"), "[]");
    EXPECT_TRUE(site_exception("https://sub.example.org/", "other.org, example.org"));
    EXPECT_TRUE(site_exception("http://127.0.0.2:8765/", "127.0.0.2"));
    EXPECT_TRUE(site_exception("https://USER@SUB.EXAMPLE.ORG.:8443/", "example.org"));
    EXPECT_FALSE(site_exception("https://badexample.org/", "example.org"));
    EXPECT_EQ(toggle_site_exception("https://sub.example.org/", "example.org"), "");
}
TEST(Extensions, StoreURLsAreStrict) {
    const auto id = fixture_id();
    EXPECT_EQ(chrome_store_id(id), id);
    EXPECT_EQ(chrome_store_id("  " + id + "\n"), id);
    EXPECT_EQ(chrome_store_id(id + "\r\n"), id);
    EXPECT_EQ(chrome_store_id("https://chromewebstore.google.com/detail/test/" + id + "?hl=ru"),
              id);
    EXPECT_TRUE(
        chrome_store_id("https://chromewebstore.google.com.evil.org/detail/x/" + id).empty());
    EXPECT_TRUE(chrome_store_id("https://user@chromewebstore.google.com/detail/x/" + id).empty());
    EXPECT_TRUE(chrome_store_id("http://chromewebstore.google.com/detail/x/" + id).empty());
    EXPECT_TRUE(chrome_store_id(std::string(32, 'z')).empty());
}
TEST(Extensions, SignedCRXPreservesIdentityAndPermissions) {
    Temporary temp;
    const auto id = fixture_id();
    auto c = inspect_crx(read_resource(fixture("valid.crx")), id, temp.path);
    EXPECT_EQ(c.extension.id, id);
    EXPECT_EQ(c.extension.name, "Aura MV3 integration test");
    EXPECT_EQ(c.extension.popup, "popup.html");
    EXPECT_GE(c.extension.permissions.size(), 3u);
    EXPECT_NE(read_resource(temp.path / "manifest.json").find("\"key\""), std::string::npos);
}
TEST(Extensions, RejectTamperingIdentityMismatchAndArchiveTraversal) {
    Temporary temp;
    auto bytes = read_resource(fixture("valid.crx"));
    bytes.back() ^= 1;
    EXPECT_THROW(inspect_crx(bytes, "", temp.path), std::exception);
    EXPECT_FALSE(std::filesystem::exists(temp.path));
    EXPECT_THROW(inspect_crx(read_resource(fixture("valid.crx")), std::string(32, 'a'), temp.path),
                 std::exception);
    EXPECT_THROW(inspect_crx(read_resource(fixture("traversal.crx")), "", temp.path),
                 std::exception);
    EXPECT_FALSE(std::filesystem::exists(temp.path));
    EXPECT_THROW(inspect_crx(read_resource(fixture("mv2.crx")), "", temp.path), std::exception);
    EXPECT_FALSE(std::filesystem::exists(temp.path));
}
TEST(Extensions, RegistryPersistsEnablementAndRemoval) {
    Temporary temp;
    ExtensionManager manager(temp.path);
    manager.candidate = std::make_unique<ExtensionCandidate>(
        inspect_crx(read_resource(fixture("valid.crx")), "", temp.path / "extensions/pending-1"));
    manager.accept();
    ASSERT_EQ(manager.items.size(), 1u);
    EXPECT_TRUE(manager.restart_required);
    EXPECT_FALSE(manager.items[0].loaded);
    manager.toggle(0);
    {
        ExtensionManager restored(temp.path);
        ASSERT_EQ(restored.items.size(), 1u);
        EXPECT_FALSE(restored.items[0].enabled);
        EXPECT_TRUE(restored.enabled_paths().empty());
        restored.remove(0);
    }
    ExtensionManager removed(temp.path);
    EXPECT_TRUE(removed.items.empty());
}
TEST(Extensions, VerifyDownloadedWebStorePackageWhenAvailable) {
    const auto path = std::filesystem::path(AURA_SOURCE_DIR) / "artifacts/features-darkreader.crx";
    if (!std::filesystem::is_regular_file(path))
        GTEST_SKIP() << "Optional real Web Store download is not cached";
    Temporary temp;
    const auto c = inspect_crx(read_resource(path), "eimadpbcbfnmbkopoojfekhnkhdbieeh", temp.path);
    EXPECT_EQ(c.extension.id, "eimadpbcbfnmbkopoojfekhnkhdbieeh");
    EXPECT_NE(c.extension.name.find("Dark Reader"), std::string::npos);
    EXPECT_FALSE(c.extension.permissions.empty());
}
TEST(Extensions, RegistryCorruptionPreservesPackagesAndUnmanagedFiles) {
    Temporary temp;
    const auto root = temp.path / "extensions";
    const auto package = root / (std::string(32, 'a') + "-123");
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(root / "user-notes");
    for (const auto *invalid : {"[[not valid toml", "extensions = 42", "other = true"}) {
        {
            std::ofstream out(root / "registry.toml");
            out << invalid;
        }
        {
            ExtensionManager manager(temp.path);
            EXPECT_TRUE(manager.items.empty());
            EXPECT_FALSE(manager.status.empty());
        }
        EXPECT_TRUE(std::filesystem::exists(package));
        EXPECT_TRUE(std::filesystem::exists(root / "user-notes"));
    }
}
TEST(Extensions, PinningPersistsWithoutRestartAndSurvivesUpdate) {
    Temporary temp;
    {
        ExtensionManager manager(temp.path);
        manager.candidate = std::make_unique<ExtensionCandidate>(inspect_crx(
            read_resource(fixture("valid.crx")), "", temp.path / "extensions/pending-1"));
        manager.accept();
    }
    {
        ExtensionManager manager(temp.path);
        ASSERT_EQ(manager.items.size(), 1u);
        EXPECT_FALSE(manager.items[0].pinned);
        manager.pin(0);
        EXPECT_TRUE(manager.items[0].pinned);
        EXPECT_FALSE(manager.restart_required);
        EXPECT_EQ(manager.items[0].icon, "icon.png");
        EXPECT_TRUE(std::filesystem::is_regular_file(manager.icon_path(0)));
        manager.candidate = std::make_unique<ExtensionCandidate>(inspect_crx(
            read_resource(fixture("valid.crx")), "", temp.path / "extensions/pending-2"));
        manager.accept();
        EXPECT_TRUE(manager.items[0].pinned);
    }
    ExtensionManager restored(temp.path);
    ASSERT_EQ(restored.items.size(), 1u);
    EXPECT_TRUE(restored.items[0].pinned);
    restored.pin(0);
    EXPECT_FALSE(restored.items[0].pinned);
    EXPECT_FALSE(restored.restart_required);
}
