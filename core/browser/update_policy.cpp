#include "core/browser/update_policy.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdexcept>
#include <toml++/toml.hpp>
namespace aura {
namespace {
bool token(std::string_view value) {
    return !value.empty() && value.size() <= 160 && value.front() != '.' &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_';
           });
}
[[noreturn]] void invalid() {
    throw std::runtime_error("Update verification failed. Nothing was installed.");
}
} // namespace
std::string UpdateRelease::page_url() const {
    return "https://github.com/Sttanors/aura-browser/releases/tag/" + tag;
}
std::string UpdateRelease::download_url(const UpdateArtifact &a) const {
    return "https://github.com/Sttanors/aura-browser/releases/download/" + tag + "/" + a.filename;
}
UpdateRelease verify_update_manifest(std::string_view bytes, std::string_view signature,
                                     std::string_view public_key, long long now) {
    if (bytes.empty() || bytes.size() > 65536 || signature.size() != 64 || public_key.size() > 4096)
        invalid();
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(public_key.data(), int(public_key.size())), BIO_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr, EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!key || !ctx || EVP_PKEY_id(key.get()) != EVP_PKEY_ED25519 ||
        EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char *>(signature.data()),
                         signature.size(), reinterpret_cast<const unsigned char *>(bytes.data()),
                         bytes.size()) != 1)
        invalid();
    const auto root = toml::parse(bytes);
    UpdateRelease release;
    release.version = root["version"].value_or(std::string{});
    release.tag = root["tag"].value_or(std::string{});
    release.sequence = root["sequence"].value_or(0LL);
    release.expires = root["expires"].value_or(0LL);
    if (root["schema"].value_or(0) != 1 || !token(release.version) ||
        release.tag != "v" + release.version || release.sequence < 1 || release.expires <= now ||
        release.expires - now > 180LL * 86400)
        invalid();
    const auto list = root["artifacts"].as_array();
    if (!list || list->empty() || list->size() > 12)
        invalid();
    for (const auto &node : *list) {
        auto t = node.as_table();
        if (!t)
            invalid();
        UpdateArtifact a{(*t)["platform"].value_or(std::string{}),
                         (*t)["kind"].value_or(std::string{}),
                         (*t)["filename"].value_or(std::string{}),
                         (*t)["sha256"].value_or(std::string{}), (*t)["size"].value_or(0LL)};
        if ((a.platform != "linux-x64" && a.platform != "windows-x64") ||
            (a.kind != "run" && a.kind != "setup" && a.kind != "deb" && a.kind != "rpm" &&
             a.kind != "arch" && a.kind != "portable") ||
            !token(a.filename) || a.sha256.size() != 64 || a.size <= 0 ||
            a.size > 2LL * 1024 * 1024 * 1024 ||
            !std::all_of(a.sha256.begin(), a.sha256.end(),
                         [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
            std::any_of(release.artifacts.begin(), release.artifacts.end(), [&](const auto &other) {
                return other.filename == a.filename ||
                       (other.platform == a.platform && other.kind == a.kind);
            }))
            invalid();
        release.artifacts.push_back(std::move(a));
    }
    return release;
}
void verify_update_artifact(const std::filesystem::path &path, const UpdateArtifact &a) {
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) != static_cast<std::uintmax_t>(a.size))
        invalid();
    std::ifstream in(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!in || !ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        invalid();
    std::array<char, 65536> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount())
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), std::size_t(in.gcount())) != 1)
            invalid();
    if (!in.eof())
        invalid();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &length) != 1 || length != 32)
        invalid();
    std::string hex;
    for (unsigned i = 0; i < length; ++i) {
        hex += "0123456789abcdef"[digest[i] >> 4];
        hex += "0123456789abcdef"[digest[i] & 15];
    }
    if (hex != a.sha256)
        invalid();
}
} // namespace aura
