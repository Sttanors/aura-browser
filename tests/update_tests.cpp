#include "core/browser/update_policy.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
namespace aura {
namespace {
class UpdatePolicy : public testing::Test {
  protected:
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    std::string public_key;
    std::string manifest = R"(schema = 1
version = "0.3.0"
tag = "v0.3.0"
sequence = 5
expires = 2000000100
[[artifacts]]
platform = "linux-x64"
kind = "run"
filename = "aura-linux.run"
sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
size = 3
)";
    void SetUp() override {
        key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"));
        ASSERT_TRUE(key);
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
        ASSERT_EQ(PEM_write_bio_PUBKEY(bio.get(), key.get()), 1);
        char *bytes = nullptr;
        const auto size = BIO_get_mem_data(bio.get(), &bytes);
        public_key.assign(bytes, size);
    }
    std::string signature() {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
        EXPECT_EQ(EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()), 1);
        std::string result(64, '\0');
        size_t size = result.size();
        EXPECT_EQ(EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char *>(result.data()), &size,
                                 reinterpret_cast<const unsigned char *>(manifest.data()),
                                 manifest.size()),
                  1);
        return result;
    }
    UpdateRelease verify(const std::string &sig) {
        return verify_update_manifest(manifest, sig, public_key, 2000000000);
    }
};
TEST_F(UpdatePolicy, AuthenticatesReleaseAndConstructsOnlyRepositoryUrls) {
    auto release = verify(signature());
    EXPECT_EQ(release.sequence, 5);
    EXPECT_EQ(release.page_url(), "https://github.com/Sttanors/aura-browser/releases/tag/v0.3.0");
    EXPECT_EQ(release.download_url(release.artifacts[0]),
              "https://github.com/Sttanors/aura-browser/releases/download/v0.3.0/aura-linux.run");
}
TEST_F(UpdatePolicy, RejectsTamperingWrongKeyAndTruncatedSignature) {
    auto sig = signature();
    manifest.back() = ' ';
    EXPECT_THROW(verify(sig), std::runtime_error);
    EXPECT_THROW(verify(sig.substr(1)), std::runtime_error);
    auto other = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
    key.swap(other);
    EXPECT_THROW(verify(signature()), std::runtime_error);
}
TEST_F(UpdatePolicy, RejectsExpiredAndExcessivelyLongLivedMetadata) {
    auto sig = signature();
    EXPECT_THROW(verify_update_manifest(manifest, sig, public_key, 2000000100), std::runtime_error);
    EXPECT_THROW(verify_update_manifest(manifest, sig, public_key, 1900000000), std::runtime_error);
}
TEST_F(UpdatePolicy, RejectsPathTraversalAndDuplicateArtifactsEvenWhenSigned) {
    auto pos = manifest.find("aura-linux.run");
    manifest.replace(pos, 14, "../aura.run");
    EXPECT_THROW(verify(signature()), std::runtime_error);
    manifest.replace(pos, 11, "aura.run");
    manifest += manifest.substr(manifest.find("[[artifacts]]"));
    EXPECT_THROW(verify(signature()), std::runtime_error);
}
TEST_F(UpdatePolicy, VerifiesDownloadedBytesAndRejectsWrongSizeAndHash) {
    auto release = verify(signature());
    auto path = std::filesystem::temp_directory_path() / "aura-update-policy-test.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "abc";
    }
    EXPECT_NO_THROW(verify_update_artifact(path, release.artifacts[0]));
    {
        std::ofstream out(path, std::ios::binary);
        out << "abd";
    }
    EXPECT_THROW(verify_update_artifact(path, release.artifacts[0]), std::runtime_error);
    {
        std::ofstream out(path, std::ios::binary);
        out << "abcd";
    }
    EXPECT_THROW(verify_update_artifact(path, release.artifacts[0]), std::runtime_error);
    std::filesystem::remove(path);
}
} // namespace
} // namespace aura
