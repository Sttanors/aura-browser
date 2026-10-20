#pragma once
#include "core/cef/diagnostic_client.hpp"
#include "include/core/SkFont.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include <filesystem>

class SkCanvas;
namespace aura {
class DiagnosticScene {
public:
    explicit DiagnosticScene(const std::filesystem::path& font);
    void draw(SkCanvas& canvas, int width, int height, const DiagnosticState& state,
              unsigned theme_index, double elapsed_seconds) const;
    bool snapshot(const std::filesystem::path& file, int width, int height,
                  const DiagnosticState& state, unsigned theme_index, double elapsed_seconds) const;
    static Rect web_bounds(int width, int height);
private:
    sk_sp<SkTypeface> regular_;
    sk_sp<SkTypeface> medium_;
};
}
