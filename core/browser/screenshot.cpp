#include "core/browser/screenshot.hpp"
#include <algorithm>
#include <cmath>
namespace aura {
std::optional<CapturePlan> screenshot_plan(CaptureRect css, double zoom, double device_scale) {
    if (!std::isfinite(css.x) || !std::isfinite(css.y) || !std::isfinite(css.width) ||
        !std::isfinite(css.height) || !std::isfinite(zoom) || !std::isfinite(device_scale) ||
        css.width <= 0 || css.height <= 0 || zoom < .1 || zoom > 10 || device_scale < .25 ||
        device_scale > 8)
        return {};
    CapturePlan plan;
    plan.clip = {css.x * zoom, css.y * zoom, css.width * zoom, css.height * zoom};
    if (std::abs(plan.clip.x) > 1e8 || std::abs(plan.clip.y) > 1e8 || plan.clip.width < 1 ||
        plan.clip.height < 1 || plan.clip.width > 1e8 || plan.clip.height > 1e8)
        return {};
    const double native_width = css.width * zoom * device_scale,
                 native_height = css.height * zoom * device_scale;
    const double factor =
        std::min({1., 32766 / native_width, 32766 / native_height,
                  std::sqrt((32. * 1024 * 1024 - 65536) / (native_width * native_height))});
    plan.scale = factor;
    plan.reduced = factor < .999999;
    // Chromium rounds its DIP request to integer dimensions before applying this scale.
    plan.width = int(std::lround(std::floor(plan.clip.width) * device_scale * factor));
    plan.height = int(std::lround(std::floor(plan.clip.height) * device_scale * factor));
    if (plan.width < 1 || plan.height < 1 || plan.width > 32767 || plan.height > 32767 ||
        uint64_t(plan.width) * plan.height > 32ULL * 1024 * 1024)
        return {};
    return plan;
}
std::optional<std::pair<int, int>> screenshot_png_size(std::string_view bytes) {
    constexpr unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || bytes.size() > 128ULL * 1024 * 1024)
        return {};
    for (int i = 0; i < 8; ++i)
        if (static_cast<unsigned char>(bytes[i]) != signature[i])
            return {};
    auto number = [&](int offset) {
        uint32_t n = 0;
        for (int i = 0; i < 4; ++i)
            n = n * 256 + static_cast<unsigned char>(bytes[offset + i]);
        return n;
    };
    if (number(8) != 13 || bytes.substr(12, 4) != "IHDR")
        return {};
    const auto w = number(16), h = number(20);
    if (!w || !h || w > 32767 || h > 32767 || uint64_t(w) * h > 32ULL * 1024 * 1024)
        return {};
    return std::pair<int, int>{int(w), int(h)};
}
} // namespace aura
