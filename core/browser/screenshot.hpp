#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
namespace aura {
struct CaptureRect {
    double x = 0, y = 0, width = 0, height = 0;
};
struct CapturePlan {
    CaptureRect clip;
    double scale = 1;
    int width = 0, height = 0;
    bool reduced = false;
};
std::optional<CapturePlan> screenshot_plan(CaptureRect css, double zoom, double device_scale);
std::optional<std::pair<int, int>> screenshot_png_size(std::string_view bytes);
} // namespace aura
