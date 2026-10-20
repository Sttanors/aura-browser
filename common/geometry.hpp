#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace aura {
struct Rect { int x{}, y{}, width{}, height{}; };
inline Rect intersect(Rect a, Rect b) {
    const auto x = std::max<long long>(a.x, b.x);
    const auto y = std::max<long long>(a.y, b.y);
    const auto right = std::min(static_cast<long long>(a.x) + a.width,
                                static_cast<long long>(b.x) + b.width);
    const auto bottom = std::min(static_cast<long long>(a.y) + a.height,
                                 static_cast<long long>(b.y) + b.height);
    return {static_cast<int>(x), static_cast<int>(y),
            static_cast<int>(std::max(0LL, right - x)), static_cast<int>(std::max(0LL, bottom - y))};
}
inline int pixels_from_dip(int value, double scale) {
    if (!std::isfinite(scale) || scale <= 0 || scale > 8 || value < 0 || value > 32768)
        throw std::invalid_argument("Invalid DIP conversion");
    return static_cast<int>(std::lround(value * scale));
}
}
