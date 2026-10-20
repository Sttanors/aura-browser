#include "common/paint_buffer.hpp"
#include <cstring>
#include <stdexcept>

namespace aura {
void PaintBuffer::update(int width, int height, std::span<const std::uint8_t> source,
                         std::span<const Rect> dirty) {
    constexpr std::size_t budget = 256 * 1024 * 1024;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        throw std::invalid_argument("Invalid paint dimensions");
    const auto stride = static_cast<std::size_t>(width) * 4;
    const auto size = stride * static_cast<std::size_t>(height);
    if (size > budget || source.size() != size)
        throw std::invalid_argument("Invalid paint byte count");
    if (width != width_ || height != height_ || dirty.empty()) {
        pixels_.assign(source.begin(), source.end());
        width_ = width;
        height_ = height;
    } else {
        for (const auto rectangle : dirty) {
            const auto clipped = intersect(rectangle, {0, 0, width, height});
            for (int row = 0; row < clipped.height; ++row) {
                const auto offset = (static_cast<std::size_t>(clipped.y + row) * width + clipped.x) * 4;
                std::memcpy(pixels_.data() + offset, source.data() + offset,
                            static_cast<std::size_t>(clipped.width) * 4);
            }
        }
    }
    ++generation_;
}
}
