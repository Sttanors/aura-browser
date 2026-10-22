#include "common/platform.hpp"
#include "common/config.hpp"
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
#endif
namespace aura {
uint64_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<uint64_t>(getpid());
#endif
}
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
bool launch_process(const std::filesystem::path &executable,
                    const std::vector<std::string> &arguments) {
#ifdef _WIN32
    auto quote = [](const std::wstring &value) {
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t c : value) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, L'\\');
        out += L'"';
        return out;
    };
    std::wstring command = quote(executable.native());
    for (const auto &arg : arguments)
        command += L" " + quote(path_from_utf8(arg).native());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, executable.parent_path().c_str(),
                        &startup, &child))
        return false;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return true;
#else
    std::vector<std::string> owned{executable.string()};
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    for (auto &arg : owned)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        return false;
    // Named profiles can outlive their parent window. Reap only this owned child.
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return true;
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
