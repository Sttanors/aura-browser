#include "core/browser/tab_strip.hpp"
#include <algorithm>
namespace aura {
TabStripLayout tab_strip_layout(int width, int count, int first) {
    TabStripLayout result;
    count = std::max(0, count);
    const int normal = std::max(1, (width - 252) / 116);
    result.overflow = count > normal;
    const int available = std::max(112, width - (result.overflow ? 332 : 256));
    result.capacity = std::max(1, (available + 4) / 116);
    result.first = std::clamp(first, 0, std::max(0, count - result.capacity));
    const int shown = std::min(count, result.capacity);
    const int item_width =
        std::min(220, (available - std::max(0, shown - 1) * 4) / std::max(1, shown));
    int right = 12;
    for (int i = 0; i < shown; ++i) {
        result.slots.push_back({result.first + i, {right, 2, item_width, 28}});
        right += item_width + 4;
    }
    if (result.overflow) {
        result.previous = {width - 304, 2, 28, 28};
        result.next = {width - 268, 2, 28, 28};
        result.add = {width - 232, 2, 32, 28};
        right = width - 196;
    } else {
        result.add = {right + 4, 2, 32, 28};
        right += 44;
    }
    result.caption = {right, 0, std::max(0, width - 144 - right), 30};
    return result;
}
bool tab_strip_caption(const TabStripLayout &layout, int x, int y) {
    const auto r = layout.caption;
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
} // namespace aura
