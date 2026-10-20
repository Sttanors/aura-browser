#include "ui/wallpaper.hpp"
#include "include/cef_image.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>
namespace aura {
namespace {
SkColor mix(SkColor a, SkColor b, float weight) {
    auto channel = [&](int x, int y) { return int(x * (1 - weight) + y * weight); };
    return SkColorSetRGB(channel(SkColorGetR(a), SkColorGetR(b)),
                         channel(SkColorGetG(a), SkColorGetG(b)),
                         channel(SkColorGetB(a), SkColorGetB(b)));
}
float luminance(SkColor color) {
    auto linear = [](float c) {
        c /= 255;
        return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
    };
    return .2126 * linear(SkColorGetR(color)) + .7152 * linear(SkColorGetG(color)) +
           .0722 * linear(SkColorGetB(color));
}
float contrast(SkColor a, SkColor b) {
    auto x = luminance(a), y = luminance(b);
    return (std::max(x, y) + .05f) / (std::min(x, y) + .05f);
}
} // namespace
Wallpaper read_wallpaper(const std::filesystem::path &path) {
    Wallpaper result;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || !std::filesystem::is_regular_file(path, ec) || size > 20 * 1024 * 1024 || size < 24) {
        result.error = "Choose a PNG or JPEG file smaller than 20 MB.";
        return result;
    }
    std::vector<unsigned char> data(size);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(data.data()), data.size())) {
        result.error = "Could not read the image.";
        return result;
    }
    bool png = std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    uint32_t width = 0, height = 0;
    if (png) {
        auto value = [&](size_t n) {
            return (uint32_t(data[n]) << 24) | (uint32_t(data[n + 1]) << 16) |
                   (uint32_t(data[n + 2]) << 8) | data[n + 3];
        };
        width = value(16);
        height = value(20);
        result.extension = ".png";
    } else if (data[0] == 0xff && data[1] == 0xd8) {
        for (size_t i = 2; i + 4 < data.size();) {
            if (data[i++] != 0xff)
                break;
            while (i < data.size() && data[i] == 0xff)
                ++i;
            if (i >= data.size())
                break;
            auto marker = data[i++];
            if (marker == 0xda || marker == 0xd9)
                break;
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
                continue;
            if (i + 2 > data.size())
                break;
            size_t length = (data[i] << 8) | data[i + 1];
            if (length < 2 || i + length > data.size())
                break;
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 &&
                marker != 0xcc && length >= 8) {
                height = (data[i + 3] << 8) | data[i + 4];
                width = (data[i + 5] << 8) | data[i + 6];
                break;
            }
            i += length;
        }
        result.extension = ".jpg";
    }
    // Check encoded dimensions before Chromium allocates any decoded bitmap.
    if (!width || !height || width > 8192 || height > 8192) {
        result.error = "Use a PNG or JPEG image up to 8192 pixels per side.";
        return result;
    }
    auto decoded = CefImage::CreateImage();
    bool ok = png ? decoded->AddPNG(1, data.data(), data.size())
                  : decoded->AddJPEG(1, data.data(), data.size());
    if (!ok) {
        result.error = "Could not decode the image. Choose another PNG or JPEG.";
        return result;
    }
    int w = 0, h = 0;
    auto bitmap =
        decoded->GetAsBitmap(1, CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_PREMULTIPLIED, w, h);
    if (!bitmap || w != int(width) || h != int(height)) {
        result.error = "Could not decode the image. Choose another PNG or JPEG.";
        return result;
    }
    std::vector<unsigned char> source(bitmap->GetSize());
    bitmap->GetData(source.data(), source.size(), 0);
    float factor = std::min(1.f, 1920.f / std::max(w, h));
    int sw = std::max(1, int(w * factor)), sh = std::max(1, int(h * factor));
    std::vector<unsigned char> pixels(size_t(sw) * sh * 4);
    std::array<uint32_t, 4096> histogram{};
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const auto src =
                (size_t(std::min(h - 1, int(y / factor))) * w + std::min(w - 1, int(x / factor))) *
                4;
            auto dst = (size_t(y) * sw + x) * 4;
            std::memcpy(pixels.data() + dst, source.data() + src, 4);
            if (!(x % 8) && !(y % 8) && source[src + 3] > 200) {
                auto b = source[src], g = source[src + 1], r = source[src + 2];
                auto high = std::max({r, g, b}), low = std::min({r, g, b});
                if (high > 30 && low < 230)
                    histogram[(r >> 4) * 256 + (g >> 4) * 16 + (b >> 4)] += 1 + (high - low) / 32;
            }
        }
    auto dominant = std::max_element(histogram.begin(), histogram.end()) - histogram.begin();
    SkColor color = SkColorSetRGB(((dominant >> 8) & 15) * 16 + 8, ((dominant >> 4) & 15) * 16 + 8,
                                  (dominant & 15) * 16 + 8);
    if (*std::max_element(histogram.begin(), histogram.end()) == 0)
        color = 0xff808080;
    float hsv[3];
    SkColorToHSV(color, hsv);
    hsv[1] = hsv[1] < .08f ? 0 : std::clamp(hsv[1], .25f, .7f);
    for (int light = 0; light < 2; ++light) {
        auto background = mix(color, light ? SK_ColorWHITE : SK_ColorBLACK, light ? .94f : .88f);
        auto surface = mix(color, light ? SK_ColorWHITE : SK_ColorBLACK, light ? .88f : .78f);
        auto text = light ? 0xff17212b : 0xfff4f6fa, muted = light ? 0xff404b58 : 0xffb5c0cc;
        auto accent_hsv = std::array<float, 3>{hsv[0], hsv[1], light ? .62f : .96f};
        SkColor accent = SkHSVToColor(accent_hsv.data());
        for (int i = 0; i < 24 && contrast(accent, surface) < 4.5f; ++i) {
            accent_hsv[2] = light ? accent_hsv[2] * .9f : std::min(1.f, accent_hsv[2] + .02f);
            if (!light)
                accent_hsv[1] *= .9f;
            accent = SkHSVToColor(accent_hsv.data());
        }
        result.palette[light] = {background, surface, text, muted, accent};
    }
    result.image = SkImages::RasterFromData(
        SkImageInfo::Make(sw, sh, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
        SkData::MakeWithCopy(pixels.data(), pixels.size()), sw * 4);
    result.encoded = std::move(data);
    return result;
}
} // namespace aura
