#include "common/pump_schedule.hpp"
#include <algorithm>
#include <cstdint>
namespace aura {
void PumpSchedule::schedule(std::int64_t delay_ms, Clock::time_point now) {
    const auto requested =
        now + std::chrono::milliseconds(std::clamp<std::int64_t>(delay_ms, 0, 60000));
    bool earlier = false;
    {
        std::lock_guard lock(mutex_);
        if (!deadline_ || requested < *deadline_) {
            deadline_ = requested;
            earlier = true;
        }
    }
    // CEF schedules from other threads too. Wake the native poll immediately,
    // without holding the scheduler lock while calling back into the host.
    if (earlier && wake_)
        wake_();
}
bool PumpSchedule::take_due(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (!deadline_ || *deadline_ > now)
        return false;
    deadline_.reset();
    return true;
}
int PumpSchedule::wait_ms(Clock::time_point now) const {
    std::lock_guard lock(mutex_);
    if (!deadline_)
        return 16;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline_ - now).count();
    return static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 16));
}
} // namespace aura
