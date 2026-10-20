#include "render/diagnostic_scene.hpp"
#include "aura/design_tokens.hpp"
#include "common/config.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkStream.h"
#include "include/effects/SkGradient.h"
#include "include/encode/SkPngEncoder.h"
#ifdef _WIN32
#include "include/ports/SkTypeface_win.h"
#else
#include "include/ports/SkFontMgr_empty.h"
#endif
#include <cmath>
#include <stdexcept>

namespace aura {
namespace {
void label(SkCanvas& canvas, sk_sp<SkTypeface> typeface, const std::string& text,
           float x, float y, float size, SkColor color) {
    SkFont font(std::move(typeface), size);
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setSubpixel(true);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);
    canvas.drawString(text.c_str(), x, y, font, paint);
}
void round_rect(SkCanvas& canvas, SkRect rect, float radius, SkColor color) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);
    canvas.drawRoundRect(rect, radius, radius, paint);
}
void glow(SkCanvas& canvas, SkPoint center, float radius, SkColor color) {
    const SkColor4f colors[] = {SkColor4f::FromColor(SkColorSetA(color, 65)),
                               SkColor4f::FromColor(SkColorSetA(color, 0))};
    SkPaint paint;
    paint.setShader(SkShaders::RadialGradient(center, radius,
        SkGradient(SkGradient::Colors(colors, SkTileMode::kClamp), {})));
    canvas.drawCircle(center, radius, paint);
}
void web_image(SkCanvas& canvas, const PaintBuffer& buffer, SkRect rect) {
    if (!buffer.generation()) return;
    const auto info = SkImageInfo::Make(buffer.width(), buffer.height(), kBGRA_8888_SkColorType,
                                       kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto image = SkImages::RasterFromData(info,
        SkData::MakeWithCopy(buffer.bytes().data(), buffer.bytes().size()), buffer.width() * 4);
    if (image) canvas.drawImageRect(image, rect, SkSamplingOptions(SkFilterMode::kLinear));
}
}

DiagnosticScene::DiagnosticScene(const std::filesystem::path& file) {
#ifdef _WIN32
    const auto manager = SkFontMgr_New_DirectWrite();
#else
    const auto manager = SkFontMgr_New_Custom_Empty();
#endif
    regular_ = manager->makeFromFile(utf8_path(file).c_str());
    if (!regular_) throw std::runtime_error("Unable to load Inter Variable font");
    const SkFontArguments::VariationPosition::Coordinate coordinate{SkSetFourByteTag('w','g','h','t'), 620};
    SkFontArguments args;
    args.setVariationDesignPosition({&coordinate, 1});
    medium_ = regular_->makeClone(args);
    if (!medium_) medium_ = regular_;
}
Rect DiagnosticScene::web_bounds(int width, int height) {
    return {340, 158, std::max(100, width - 372), std::max(100, height - 194)};
}
void DiagnosticScene::draw(SkCanvas& canvas, int width, int height, const DiagnosticState& state,
                           unsigned theme_index, double elapsed_seconds) const {
    const auto& t = design::themes[theme_index % design::themes.size()];
    canvas.clear(t.background);
    glow(canvas, {100, 80}, 650, t.mesh[0]);
    glow(canvas, {static_cast<float>(width - 80), static_cast<float>(height)}, 600, t.mesh[1]);
    glow(canvas, {static_cast<float>(width / 2), static_cast<float>(height + 150)}, 500, t.mesh[2]);
    // Subtle deterministic grain; static geometry creates no background animation loop.
    SkPaint grain;
    grain.setColor(SkColorSetA(t.text, 6));
    std::uint32_t random = 0xA02;
    for (int i = 0; i < width * height / 100; ++i) {
        random = random * 1664525u + 1013904223u;
        const float x = static_cast<float>(random % static_cast<unsigned>(width));
        random = random * 1664525u + 1013904223u;
        canvas.drawPoint(x, static_cast<float>(random % static_cast<unsigned>(height)), grain);
    }
    round_rect(canvas, SkRect::MakeXYWH(24, 23, 28, 28), 10, t.accent);
    SkPaint cutout;
    cutout.setAntiAlias(true);
    cutout.setColor(t.background);
    canvas.drawCircle(38, 37, 7, cutout);
    label(canvas, medium_, "aura", 65, 44, 24, t.text);
    label(canvas, regular_, "SYSTEMS / DESIGN", 145, 41, 10, t.muted);
    label(canvas, regular_, "MILESTONE 00", 34, 134, 11, t.accent);
    label(canvas, medium_, "A new", 30, 202, 48, t.text);
    label(canvas, medium_, "perspective.", 30, 258, 42, t.text);
    label(canvas, regular_, "Chromium at the core.", 34, 300, 14, t.muted);
    label(canvas, regular_, "Every pixel, our own.", 34, 323, 14, t.muted);
    const auto metric = [&](float y, const std::string& name, const std::string& value) {
        round_rect(canvas, SkRect::MakeXYWH(30, y, 274, 73), 16, SkColorSetA(t.surface, 190));
        label(canvas, regular_, name, 47, y + 25, 11, t.muted);
        label(canvas, medium_, value, 47, y + 51, 15, t.text);
    };
    metric(365, "COMPOSITION", state.compositor);
    metric(450, "WEB SURFACE", "CEF / " + std::to_string(state.view.generation()) + " frames");
    metric(535, "TEXTURE PROBE", state.accelerated_frames
        ? std::to_string(state.accelerated_frames) + " GPU copies + readback" : "CPU OSR / owned BGRA");
    const float bottom = static_cast<float>(height - 64);
    label(canvas, regular_, "F6  Change aura    F7  Light / dark", 34, bottom, 11, t.muted);
    label(canvas, regular_, "C++20  /  CEF  /  SKIA", 34, bottom + 25, 10, t.muted);
    label(canvas, medium_, "The web, rendered for real.", 342, 111, 22, t.text);
    auto url = state.url;
    if (url.size() > 76) url = url.substr(0, 73) + "...";
    label(canvas, regular_, url, 342, 139, 12, t.muted);
    const auto bounds = web_bounds(width, height);
    auto rect = SkRect::MakeXYWH(static_cast<float>(bounds.x), static_cast<float>(bounds.y),
                               static_cast<float>(bounds.width), static_cast<float>(bounds.height));
    round_rect(canvas, rect.makeOutset(1, 1), 19, SkColorSetA(t.text, 28));
    canvas.save();
    canvas.clipRRect(SkRRect::MakeRectXY(rect, 18, 18), true);
    canvas.drawColor(t.surface);
    web_image(canvas, state.view, rect);
    if (state.popup_visible && state.popup.generation()) {
        const auto& p = state.popup_bounds;
        web_image(canvas, state.popup, SkRect::MakeXYWH(rect.x() + p.x, rect.y() + p.y,
                                                       static_cast<float>(p.width), static_cast<float>(p.height)));
    }
    if (!state.view.generation())
        label(canvas, regular_, state.failed ? state.error : "Waiting for the first Chromium frame...",
              rect.x() + 28, rect.y() + 48, 14, t.muted);
    canvas.restore();
    const auto status = state.failed ? "LOAD FAILED" : state.loaded ? "LIVE SURFACE" : "CONNECTING";
    const auto status_x = static_cast<float>(width - 173);
    round_rect(canvas, SkRect::MakeXYWH(status_x, 83, 140, 29), 14, SkColorSetA(t.surface, 220));
    SkPaint dot;
    dot.setAntiAlias(true);
    dot.setColor(state.failed ? 0xFFEF8E96 : 0xFF68C7A0);
    if (!state.loaded) dot.setAlphaf(static_cast<float>(0.6 + 0.3 * std::sin(elapsed_seconds * 4)));
    canvas.drawCircle(status_x + 15, 98, 3, dot);
    label(canvas, medium_, status, status_x + 26, 101, 9, t.text);
}
bool DiagnosticScene::snapshot(const std::filesystem::path& file, int width, int height,
    const DiagnosticState& state, unsigned theme_index, double elapsed_seconds) const {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface) return false;
    draw(*surface->getCanvas(), width, height, state, theme_index, elapsed_seconds);
    SkPixmap pixels;
    if (!surface->peekPixels(&pixels)) return false;
    const auto data = SkPngEncoder::Encode(pixels, {});
    if (!data) return false;
    std::filesystem::create_directories(file.parent_path().empty() ? "." : file.parent_path());
    SkFILEWStream stream(utf8_path(file).c_str());
    return stream.isValid() && stream.write(data->data(), data->size());
}
}
