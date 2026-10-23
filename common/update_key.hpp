#pragma once
#include <string_view>
namespace aura {
// Public channel identity. The private key must never be packaged or committed.
inline constexpr std::string_view update_public_key = R"(-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAqpaIzIQrmOwrSJZVIFQI6u7OF5oHNVlSvo2iSc+p2Yk=
-----END PUBLIC KEY-----
)";
inline constexpr std::string_view browser_version = "0.4.0-preview.1";
inline constexpr long long browser_release_sequence = 6;
} // namespace aura
