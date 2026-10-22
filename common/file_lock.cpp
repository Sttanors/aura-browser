#include "common/file_lock.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace aura {
ExclusiveFileLock::ExclusiveFileLock(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
        handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
    const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor >= 0) {
        if (flock(descriptor, LOCK_EX | LOCK_NB) == 0)
            handle_ = descriptor;
        else
            close(descriptor);
    }
#endif
}
ExclusiveFileLock::~ExclusiveFileLock() {
    if (!owns_lock())
        return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(static_cast<int>(handle_));
#endif
}
} // namespace aura
