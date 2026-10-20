#pragma once
#include <ctime>
#include <filesystem>
#include <system_error>
namespace aura {
std::filesystem::path executable_directory();
std::tm local_time(std::time_t value);
// Atomically replace an existing file, including Windows (where rename cannot).
void replace_file(const std::filesystem::path &source, const std::filesystem::path &target,
                  std::error_code &error);
void replace_file(const std::filesystem::path &source, const std::filesystem::path &target);
}
