#include "common/exports.hpp"
#include "common/platform.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <openssl/rand.h>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif
namespace aura {
void write_export(const std::filesystem::path &target, std::string_view data) {
    if (!target.is_absolute() || target.filename().empty() ||
        !std::filesystem::is_directory(target.parent_path()))
        throw std::runtime_error("Invalid export path");
    std::array<unsigned char, 12> random{};
    if (RAND_bytes(random.data(), random.size()) != 1)
        throw std::runtime_error("Could not create export file");
    const char hex[] = "0123456789abcdef";
    std::string name = ".aura-export-";
    for (auto c : random) {
        name += hex[c >> 4];
        name += hex[c & 15];
    }
    name += ".tmp";
    const auto temporary = target.parent_path() / name;
#ifdef _WIN32
    int fd = _wopen(temporary.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                    _S_IREAD | _S_IWRITE);
#else
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    if (fd < 0)
        throw std::runtime_error("Could not create export file");
    try {
        size_t position = 0;
        while (position < data.size()) {
            const auto count = std::min<size_t>(data.size() - position, 1024 * 1024);
#ifdef _WIN32
            const auto n = _write(fd, data.data() + position, static_cast<unsigned>(count));
#else
            const auto n = write(fd, data.data() + position, count);
#endif
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("Could not write export file");
            position += size_t(n);
        }
#ifdef _WIN32
        if (_commit(fd))
            throw std::runtime_error("Could not flush export file");
        _close(fd);
#else
        if (fsync(fd))
            throw std::runtime_error("Could not flush export file");
        close(fd);
#endif
        fd = -1;
        replace_file(temporary, target);
    } catch (...) {
        if (fd >= 0) {
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
        }
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        throw;
    }
}
} // namespace aura
