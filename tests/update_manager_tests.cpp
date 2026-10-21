#include "common/config.hpp"
#include "common/update_key.hpp"
#include "core/browser/updates.hpp"
#include "include/cef_api_hash.h"
#include <ctime>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <thread>
namespace aura {
struct UpdateManagerTestAccess {
    static void
    transport(UpdateManager &manager, std::string key,
              std::function<std::shared_ptr<UpdateTransfer>(const std::string &, long long,
                                                            const std::filesystem::path &)>
                  fetch) {
        manager.public_key_ = std::move(key);
        manager.fetch_override_ = std::move(fetch);
    }
};
namespace {
const auto *cef_api = cef_api_hash(CEF_API_VERSION, 0);
class UpdateManagerFlow : public testing::Test {
  protected:
    std::filesystem::path directory;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    std::string public_key, manifest, signature, payload = "abc";
    bool network_failure = false, hold_download = false;
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
                    ("aura-update-flow-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"));
        ASSERT_TRUE(key);
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
        ASSERT_EQ(PEM_write_bio_PUBKEY(bio.get(), key.get()), 1);
        char *bytes = nullptr;
        auto length = BIO_get_mem_data(bio.get(), &bytes);
        public_key.assign(bytes, length);
        metadata(browser_release_sequence + 1);
    }
    void TearDown() override {
        std::filesystem::remove_all(directory);
    }
    void metadata(int sequence) {
#ifdef _WIN32
        const std::string platform = "windows-x64";
#else
        const std::string platform = "linux-x64";
#endif
        manifest = "schema=1\nversion='0.3.0'\ntag='v0.3.0'\nsequence=" + std::to_string(sequence) +
                   "\nexpires=" + std::to_string(std::time(nullptr) + 86400) +
                   "\n[[artifacts]]\nplatform='" + platform +
                   "'\nkind='portable'\nfilename='aura.zip'\nsize=3\nsha256="
                   "'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'\n";
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
        ASSERT_EQ(EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()), 1);
        signature.assign(64, '\0');
        size_t size = signature.size();
        ASSERT_EQ(EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char *>(signature.data()),
                                 &size, reinterpret_cast<const unsigned char *>(manifest.data()),
                                 manifest.size()),
                  1);
    }
    void connect(UpdateManager &manager) {
        UpdateManagerTestAccess::transport(
            manager, public_key,
            [this](const std::string &url, long long limit, const std::filesystem::path &file) {
                EXPECT_TRUE(url.starts_with("https://github.com/Sttanors/aura-browser/releases/") ||
                            url.starts_with(
                                "https://api.github.com/repos/Sttanors/aura-browser/releases?"));
                auto s = std::make_shared<UpdateTransfer>();
                s->complete = true;
                s->ok = !network_failure;
                auto bytes =
                    url.find("api.github.com") != std::string::npos
                        ? std::string(R"([{"tag_name":"v0.3.0","draft":false,"prerelease":true}])")
                    : url.ends_with(".sig")  ? signature
                    : url.ends_with(".toml") ? manifest
                                             : payload;
                EXPECT_LE(bytes.size(), static_cast<std::size_t>(limit));
                if (file.empty())
                    s->bytes = bytes;
                else {
                    std::ofstream out(file, std::ios::binary);
                    out << bytes;
                    s->received = bytes.size();
                    s->complete = !hold_download;
                }
                return s;
            });
    }
    void settle(UpdateManager &manager) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (manager.busy() && std::chrono::steady_clock::now() < deadline) {
            manager.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_FALSE(manager.busy());
    }
};
TEST_F(UpdateManagerFlow, ChecksAuthenticatesDownloadsAndVerifiesWithoutReplacingInstallation) {
    UpdateManager manager(directory);
    connect(manager);
    EXPECT_TRUE(manager.due());
    manager.check();
    settle(manager);
    ASSERT_TRUE(manager.release);
    EXPECT_EQ(manager.release->sequence, browser_release_sequence + 1);
    manager.download();
    settle(manager);
    ASSERT_FALSE(manager.ready_file.empty());
    std::ifstream input(manager.ready_file, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "abc");
    auto ready = manager.ready_file;
    manager.shutdown();
    EXPECT_EQ(manager.ready_file, ready);
    EXPECT_TRUE(std::filesystem::is_regular_file(ready));
}
TEST_F(UpdateManagerFlow, RejectsOlderSignedChannelAfterRestart) {
    {
        UpdateManager manager(directory);
        connect(manager);
        metadata(browser_release_sequence + 2);
        manager.check();
        settle(manager);
        ASSERT_TRUE(manager.release);
    }
    UpdateManager manager(directory);
    connect(manager);
    metadata(browser_release_sequence + 1);
    EXPECT_FALSE(manager.due());
    manager.check();
    settle(manager);
    EXPECT_FALSE(manager.release);
    EXPECT_EQ(manager.status, "An outdated update manifest was rejected.");
}
TEST_F(UpdateManagerFlow, HandlesNetworkFailureAndRejectsTamperedManifest) {
    UpdateManager manager(directory);
    connect(manager);
    network_failure = true;
    manager.check();
    settle(manager);
    EXPECT_FALSE(manager.release);
    network_failure = false;
    manifest += "\n";
    manager.check();
    settle(manager);
    EXPECT_FALSE(manager.release);
    EXPECT_TRUE(manager.ready_file.empty());
}
TEST_F(UpdateManagerFlow, RejectsBadPayloadAndDeletesPartialDownloadsOnCancellation) {
    UpdateManager manager(directory);
    connect(manager);
    manager.check();
    settle(manager);
    payload = "abd";
    manager.download();
    settle(manager);
    EXPECT_TRUE(manager.ready_file.empty());
    EXPECT_TRUE(std::filesystem::is_empty(directory / "updates"));
    payload = "abc";
    hold_download = true;
    manager.download();
    EXPECT_TRUE(manager.busy());
    manager.cancel();
    EXPECT_FALSE(manager.busy());
    EXPECT_TRUE(std::filesystem::is_empty(directory / "updates"));
}
TEST_F(UpdateManagerFlow, RequiresFreshMetadataAgainBeforeDownloadingAnExpiredOffer) {
    UpdateManager manager(directory);
    connect(manager);
    manager.check();
    settle(manager);
    ASSERT_TRUE(manager.release);
    manager.release->expires = std::time(nullptr) - 1;
    manager.download();
    EXPECT_FALSE(manager.busy());
    EXPECT_FALSE(manager.release);
    EXPECT_TRUE(manager.ready_file.empty());
    EXPECT_EQ(manager.status, "The update metadata has expired. Check again.");
}
} // namespace
} // namespace aura
