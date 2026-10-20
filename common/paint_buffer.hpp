#pragma once
#include "common/geometry.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace aura {
class PaintBuffer {
public:
    void update(int width, int height, std::span<const std::uint8_t> source,
                std::span<const Rect> dirty);
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return pixels_; }
private:
    int width_{}, height_{};
    std::uint64_t generation_{};
    std::vector<std::uint8_t> pixels_;
};
}
