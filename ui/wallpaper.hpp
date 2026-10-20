#pragma once
#include "include/core/SkImage.h"
#include <array>
#include <filesystem>
#include <string>
#include <vector>
namespace aura {
struct Wallpaper {
    sk_sp<SkImage> image;
    std::array<std::array<uint32_t, 5>, 2> palette{};
    std::string error, extension;
    std::vector<unsigned char> encoded;
};
Wallpaper read_wallpaper(const std::filesystem::path &path);
} // namespace aura
