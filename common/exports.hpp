#pragma once
#include <filesystem>
#include <string_view>
namespace aura {
// Write a chosen export without truncating an existing file on failure.
void write_export(const std::filesystem::path &, std::string_view data);
} // namespace aura
