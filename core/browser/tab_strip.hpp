#pragma once
#include "common/geometry.hpp"
#include <vector>
namespace aura {
struct TabStripSlot {
    int position = 0;
    Rect bounds;
};
struct TabStripLayout {
    int first = 0, capacity = 0;
    bool overflow = false;
    Rect previous, next, add, caption;
    std::vector<TabStripSlot> slots;
};
TabStripLayout tab_strip_layout(int width, int count, int first);
bool tab_strip_caption(const TabStripLayout &, int x, int y);
} // namespace aura
