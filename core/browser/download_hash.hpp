#pragma once
#include <filesystem>
#include <string>
namespace aura {
std::string download_sha256(const std::filesystem::path &);
std::string normalize_sha256(std::string);
} // namespace aura
