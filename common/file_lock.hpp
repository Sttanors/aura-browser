#pragma once
#include <cstdint>
#include <filesystem>
namespace aura {
// Nonblocking process-wide lock; destroying it releases the OS handle.
class ExclusiveFileLock {
  public:
    explicit ExclusiveFileLock(const std::filesystem::path &path);
    ~ExclusiveFileLock();
    ExclusiveFileLock(const ExclusiveFileLock &) = delete;
    ExclusiveFileLock &operator=(const ExclusiveFileLock &) = delete;
    bool owns_lock() const { return handle_ != -1; }

  private:
    std::intptr_t handle_ = -1;
};
} // namespace aura
