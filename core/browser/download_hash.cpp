#include "core/browser/download_hash.hpp"
#include <array>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>
namespace aura {
std::string normalize_sha256(std::string value) {
    if (value.size() != 64)
        return {};
    for (auto &c : value) {
        if (c >= 'A' && c <= 'F')
            c += 32;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return {};
    }
    return value;
}
std::string download_sha256(const std::filesystem::path &path) {
    auto fail = [] {
        throw std::runtime_error(
            "Could not hash this file. Check that it exists and is unchanged.");
    };
    if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path))
        fail();
    const auto size = std::filesystem::file_size(path);
    const auto modified = std::filesystem::last_write_time(path);
    std::ifstream in(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!in || !ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        fail();
    std::array<char, 65536> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount())
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), size_t(in.gcount())) != 1)
            fail();
    if (!in.eof() || std::filesystem::file_size(path) != size ||
        std::filesystem::last_write_time(path) != modified)
        fail();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &length) != 1 || length != 32)
        fail();
    std::string hex;
    for (unsigned i = 0; i < length; ++i) {
        hex += "0123456789abcdef"[digest[i] >> 4];
        hex += "0123456789abcdef"[digest[i] & 15];
    }
    return hex;
}
} // namespace aura
