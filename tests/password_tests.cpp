#include "core/browser/passwords.hpp"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
namespace aura {
namespace {
struct Passwords : testing::Test {
    std::filesystem::path dir, path;
    const std::string master = "test-only strong master", transfer = "separate transfer phrase";
    void SetUp() override {
        dir = std::filesystem::temp_directory_path() /
              ("aura-password-test-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(dir);
        path = dir / "passwords.aura";
    }
    void TearDown() override {
        std::filesystem::remove_all(dir);
    }
    std::string bytes(const std::filesystem::path &p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), {}};
    }
    void file(const std::filesystem::path &p, const std::string &v) {
        std::ofstream out(p, std::ios::binary);
        out << v;
    }
};
TEST_F(Passwords, LockedFileRoundtripAndRandomNonceNeverStoresPlaintext) {
    PasswordVault vault(path);
    vault.create(master);
    vault.save({"https://example.test", "name@example.test", "fictional-password-123"});
    auto first = bytes(path);
    EXPECT_EQ(first.find("fictional-password"), std::string::npos);
    EXPECT_EQ(first.find("example.test"), std::string::npos);
    vault.save({"https://example.test", "name@example.test", "fictional-password-123"});
    EXPECT_NE(first, bytes(path));
#ifndef _WIN32
    EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
    vault.lock();
    EXPECT_FALSE(vault.unlocked());
    EXPECT_TRUE(vault.logins().empty());
    EXPECT_THROW(vault.unlock("wrong password"), std::runtime_error);
    EXPECT_FALSE(vault.unlocked());
    vault.unlock(master);
    ASSERT_EQ(vault.logins().size(), 1);
    EXPECT_EQ(vault.logins()[0].password, "fictional-password-123");
}
TEST_F(Passwords, RejectsTamperedHeaderCiphertextAndTagWithoutChangingUnlockedState) {
    PasswordVault vault(path);
    vault.create(master);
    vault.save({"https://test.invalid", "user", "fake secret"});
    auto original = bytes(path);
    for (size_t pos :
         {size_t(0), size_t(15), size_t(35), original.size() / 2, original.size() - 1}) {
        auto data = original;
        data[pos] ^= 1;
        file(path, data);
        EXPECT_THROW(vault.unlock(master), std::runtime_error);
        ASSERT_EQ(vault.logins().size(), 1);
        EXPECT_EQ(vault.logins()[0].password, "fake secret");
    }
    file(path, original);
    vault.lock();
    vault.unlock(master);
}
TEST_F(Passwords, ExportUsesSeparatePasswordImportKeepsLocalConflictsAndMakesBackup) {
    PasswordVault source(path);
    source.create(master);
    source.save({"https://test.invalid", "same", "source password"});
    source.save({"https://other.invalid", "new", "new password"});
    auto exported = dir / "transfer.aura";
    source.export_file(exported, transfer);
    auto contents = bytes(exported);
    EXPECT_THROW(source.export_file(exported, transfer), std::runtime_error);
    EXPECT_EQ(contents, bytes(exported));
    PasswordVault target(dir / "target.aura");
    target.create("different target master");
    target.save({"https://test.invalid", "same", "local password"});
    EXPECT_THROW(target.import_file(exported, master), std::runtime_error);
    ASSERT_EQ(target.logins().size(), 1);
    EXPECT_EQ(target.import_file(exported, transfer), 1);
    EXPECT_EQ(target.logins()[0].password, "local password");
    EXPECT_EQ(target.logins()[1].password, "new password");
    EXPECT_EQ(target.import_file(exported, transfer), 0);
    EXPECT_EQ(contents, bytes(exported));
    size_t backups = 0;
    for (auto &f : std::filesystem::directory_iterator(dir))
        if (f.path().filename().string().starts_with("target.aura.backup-")) {
            ++backups;
            PasswordVault old(f.path());
            old.unlock("different target master");
            ASSERT_EQ(old.logins().size(), 1);
            EXPECT_EQ(old.logins()[0].password, "local password");
        }
    EXPECT_EQ(backups, 1);
}
TEST_F(Passwords, PasswordChangeRemoveAndFailedWritePreserveData) {
    PasswordVault vault(path);
    vault.create(master);
    vault.save({"https://test.invalid", "user", "fake password"});
    vault.change_password(transfer);
    vault.lock();
    EXPECT_THROW(vault.unlock(master), std::runtime_error);
    vault.unlock(transfer);
    auto original = bytes(path);
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    file(path / "block", "x");
    EXPECT_THROW(vault.save({"https://other.invalid", "new", "other password"}),
                 std::runtime_error);
    ASSERT_EQ(vault.logins().size(), 1);
    std::filesystem::remove_all(path);
    file(path, original);
    vault.remove(0);
    vault.lock();
    vault.unlock(transfer);
    EXPECT_TRUE(vault.logins().empty());
}
TEST_F(Passwords, RejectsInsecureOriginsOversizeInputAndUnsupportedFiles) {
    PasswordVault vault(path);
    EXPECT_THROW(vault.create("short"), std::runtime_error);
    EXPECT_FALSE(vault.exists());
    vault.create(master);
    for (const auto &o :
         {"http://test.invalid", "https://user@test.invalid", "https://test.invalid/path",
          "https://test.invalid?query", "https://test.invalid\n"})
        EXPECT_THROW(vault.save({o, "u", "p"}), std::runtime_error);
    EXPECT_THROW(vault.save({"https://test.invalid", "u", std::string(8193, 'x')}),
                 std::runtime_error);
    auto invalid = dir / "invalid.aura";
    file(invalid, "not a vault\nsecret never printed");
    EXPECT_THROW(vault.import_file(invalid, master), std::runtime_error);
    vault.lock();
    EXPECT_THROW(vault.save({"https://test.invalid", "u", "p"}), std::runtime_error);
}
TEST_F(Passwords, EditIsAtomicAndRefusesToOverwriteAnotherLogin) {
    PasswordVault vault(path);
    vault.create(master);
    vault.save({"https://first.invalid", "one", "first fictional password"});
    vault.save({"https://second.invalid", "two", "second fictional password"});
    auto before = bytes(path);
    EXPECT_THROW(vault.replace(0, {"https://second.invalid", "two", "replacement"}),
                 std::runtime_error);
    EXPECT_EQ(bytes(path), before);
    vault.replace(0, {"https://third.invalid", "three", "updated fictional password"});
    vault.lock();
    vault.unlock(master);
    ASSERT_EQ(vault.logins().size(), 2);
    EXPECT_EQ(vault.logins()[0].origin, "https://third.invalid");
    EXPECT_EQ(vault.logins()[0].password, "updated fictional password");
}
TEST_F(Passwords, WorkerCompletesBeforeIdleLockAndSecretsAreDiscarded) {
    PasswordManager manager(path);
    manager.run([&] {
        manager.vault.create(master);
        manager.vault.save({"https://test.invalid", "user", "fictional password"});
        return std::string("ready");
    });
    EXPECT_TRUE(manager.busy);
    manager.lock(); // An active worker is never raced by a UI lock.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (manager.busy && std::chrono::steady_clock::now() < deadline) {
        manager.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_FALSE(manager.busy);
    ASSERT_TRUE(manager.vault.unlocked());
    EXPECT_TRUE(manager.tick(std::chrono::steady_clock::now() + std::chrono::minutes(16)));
    EXPECT_FALSE(manager.vault.unlocked());
    EXPECT_TRUE(manager.vault.logins().empty());
}
} // namespace
} // namespace aura
