#include "common/platform.hpp"
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace aura {
std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
        throw std::runtime_error("Cannot resolve executable path");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}
std::tm local_time(std::time_t value) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}
void replace_file(const std::filesystem::path &source, const std::filesystem::path &target,
                  std::error_code &error) {
#ifdef _WIN32
    DWORD failure = ERROR_SUCCESS;
    // Readers and scanners can briefly hold a file without FILE_SHARE_DELETE.
    // Preserve the old file and retry bounded transient failures before reporting them.
    for (unsigned attempt = 0; attempt < 12; ++attempt) {
        if (MoveFileExW(source.c_str(), target.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error.clear();
            return;
        }
        failure = GetLastError();
        if ((failure != ERROR_SHARING_VIOLATION && failure != ERROR_LOCK_VIOLATION &&
             failure != ERROR_ACCESS_DENIED) ||
            attempt == 11)
            break;
        Sleep(10);
    }
    error = std::error_code(failure, std::system_category());
#else
    std::filesystem::rename(source, target, error);
#endif
}
void replace_file(const std::filesystem::path &source, const std::filesystem::path &target) {
    std::error_code error;
    replace_file(source, target, error);
    if (error)
        throw std::filesystem::filesystem_error("Could not replace file", source, target, error);
}
} // namespace aura
