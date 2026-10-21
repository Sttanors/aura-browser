#pragma once
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
namespace aura {
std::filesystem::path executable_directory();
uint64_t process_id();
bool launch_process(const std::filesystem::path &executable,
                    const std::vector<std::string> &arguments);
std::tm local_time(std::time_t value);
// Atomically replace an existing file, including Windows (where rename cannot).
void replace_file(const std::filesystem::path &source, const std::filesystem::path &target,
                  std::error_code &error);
void replace_file(const std::filesystem::path &source, const std::filesystem::path &target);
} // namespace aura
