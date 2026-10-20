#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace aura {
class PumpSchedule {
  public:
    using Clock = std::chrono::steady_clock;
    explicit PumpSchedule(std::function<void()> wake = {}) : wake_(std::move(wake)) {}
    void schedule(std::int64_t delay_ms, Clock::time_point now = Clock::now());
    bool take_due(Clock::time_point now = Clock::now());
    int wait_ms(Clock::time_point now = Clock::now()) const;

  private:
    const std::function<void()> wake_;
    mutable std::mutex mutex_;
    std::optional<Clock::time_point> deadline_;
};
} // namespace aura
