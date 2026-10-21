#include "ui/browser_ui.hpp"
#include "aura/design_tokens.hpp"
#include "common/branding_data.hpp"
#include "common/config.hpp"
#include "common/platform.hpp"
#include "common/update_key.hpp"
#include "include/cef_frame.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkGradient.h"
#ifdef _WIN32
#include "include/ports/SkTypeface_win.h"
#else
#include "include/ports/SkFontMgr_empty.h"
#endif
#include "include/utils/SkParsePath.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#else
#include <commdlg.h>
#include <windows.h>
#endif
namespace aura {
namespace {
design::Theme interface_theme(const Profile &p) {
    auto theme = design::themes[p.theme];
    if (p.ntp_background == "image" && p.ntp_image_theme && p.image_palette) {
        const auto &colors = (*p.image_palette)[p.theme % 2];
        theme.background = colors[0];
        theme.surface = colors[1];
        theme.text = colors[2];
        theme.muted = colors[3];
        theme.accent = colors[4];
        theme.mesh = {colors[4], colors[1], colors[4]};
        return theme;
    }
    static const std::map<std::string, std::pair<SkColor, SkColor>> accents = {
        {"blue", {0xFF8DBEFF, 0xFF245CC6}},
        {"violet", {0xFFC2ABFF, 0xFF7040B8}},
        {"green", {0xFF82D6B0, 0xFF216747}},
        {"rose", {0xFFF3A6C7, 0xFFA12F62}},
        {"amber", {0xFFEAC17B, 0xFF825400}}};
    if (auto it = accents.find(p.accent); it != accents.end())
        theme.accent = p.theme % 2 ? it->second.second : it->second.first;
    return theme;
}
bool inside(Rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}
SkRect box(Rect r) {
    return SkRect::MakeXYWH(r.x, r.y, r.width, r.height);
}
void fill(SkCanvas &c, Rect r, float radius, SkColor color) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    c.drawRoundRect(box(r), radius, radius, p);
}
void border(SkCanvas &c, Rect r, float radius, SkColor color) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1);
    p.setColor(color);
    c.drawRoundRect(box(r).makeInset(.5f, .5f), radius, radius, p);
}
void shadow(SkCanvas &c, Rect r, float radius) {
    SkPaint p;
    p.setColor(0x28000000);
    p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 12));
    c.drawRoundRect(box(r).makeOffset(0, 4), radius, radius, p);
}
SkFont font(sk_sp<SkTypeface> face, float size) {
    SkFont f(std::move(face), size);
    f.setSubpixel(true);
    f.setEdging(SkFont::Edging::kAntiAlias);
    return f;
}
float measure(const SkFont &f, const std::string &text) {
    return f.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
}
std::string elide(const SkFont &f, std::string text, float width) {
    if (measure(f, text) <= width)
        return text;
    while (!text.empty() && measure(f, text + "…") > width) {
        auto end = text.size() - 1;
        while (end && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end;
        text.resize(end);
    }
    return text + "…";
}
std::vector<std::string> wrap_text(const SkFont &f, const std::string &text, int width, int limit) {
    std::vector<std::string> lines;
    std::string remaining = text;
    while (!remaining.empty() && int(lines.size()) < limit) {
        if (measure(f, remaining) <= width || int(lines.size()) == limit - 1) {
            lines.push_back(elide(f, remaining, width));
            break;
        }
        size_t best = 0;
        for (size_t end = 0; end < remaining.size();) {
            ++end;
            while (end < remaining.size() &&
                   (static_cast<unsigned char>(remaining[end]) & 0xc0) == 0x80)
                ++end;
            if (measure(f, remaining.substr(0, end)) > width)
                break;
            best = end;
        }
        if (!best)
            best = remaining.size();
        const auto space = remaining.rfind(' ', best);
        if (space != std::string::npos && space > 0)
            best = space;
        lines.push_back(remaining.substr(0, best));
        remaining.erase(0, best);
        while (!remaining.empty() && remaining.front() == ' ')
            remaining.erase(0, 1);
    }
    return lines;
}
void label(SkCanvas &c, sk_sp<SkTypeface> face, const std::string &value, float x, float y,
           float size, SkColor color, float width = 10000) {
    const auto f = font(std::move(face), size);
    const auto s = elide(f, value, width);
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    c.drawString(s.c_str(), x, y, f, p);
}
// A shared text box keeps translated captions on the same baseline at every button height.
void centered_label(SkCanvas &c, sk_sp<SkTypeface> face, const std::string &value, Rect r,
                    float size, SkColor color, int left = 12, int right = 12) {
    const auto f = font(face, size);
    SkFontMetrics metrics;
    f.getMetrics(&metrics);
    const int available = std::max(0, r.width - left - right);
    if (!available)
        return;
    const auto text = elide(f, value, available);
    const float x = r.x + left + (available - measure(f, text)) * .5f;
    const float baseline = r.y + (r.height - metrics.fAscent - metrics.fDescent) * .5f;
    c.save();
    c.clipRect(box(r));
    label(c, face, text, x, baseline, size, color, available);
    c.restore();
}
void glow(SkCanvas &c, SkPoint center, float radius, SkColor color) {
    const SkColor4f colors[] = {SkColor4f::FromColor(SkColorSetA(color, 70)),
                                SkColor4f::FromColor(SkColorSetA(color, 0))};
    SkPaint p;
    p.setShader(SkShaders::RadialGradient(
        center, radius, SkGradient(SkGradient::Colors(colors, SkTileMode::kClamp), {})));
    c.drawCircle(center, radius, p);
}
void image(SkCanvas &c, const PaintBuffer &buffer, Rect r) {
    if (!buffer.generation())
        return;
    const auto info = SkImageInfo::Make(buffer.width(), buffer.height(), kBGRA_8888_SkColorType,
                                        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto im = SkImages::RasterFromData(
        info,
        // CPU canvases consume pixels synchronously on the CEF UI thread. The
        // buffer cannot change during draw; deferred GPU drawing keeps a copy.
        c.recordingContext()
            ? SkData::MakeWithCopy(buffer.bytes().data(), buffer.bytes().size())
            : SkData::MakeWithoutCopy(buffer.bytes().data(), buffer.bytes().size()),
        buffer.width() * 4);
    if (im)
        c.drawImageRect(im, box(r), SkSamplingOptions(SkFilterMode::kLinear));
}
void brand_logo(SkCanvas &c, Rect r) {
    c.save();
    c.translate(r.x, r.y);
    c.scale(r.width / 64.f, r.height / 64.f);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(0xff11182b);
    c.drawRoundRect(SkRect::MakeXYWH(1, 1, 62, 62), 18, 18, paint);
    for (const auto &item : brand::paths) {
        const auto path = SkParsePath::FromSVGString(item.data);
        if (!path)
            continue;
        paint.setColor(item.color);
        paint.setStyle(item.stroke ? SkPaint::kStroke_Style : SkPaint::kFill_Style);
        paint.setStrokeWidth(item.stroke);
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setStrokeJoin(SkPaint::kRound_Join);
        c.drawPath(*path, paint);
    }
    c.restore();
}
void icon(SkCanvas &c, const std::string &name, float x, float y, float size, SkColor color) {
    static const std::map<std::string, const char *> paths = {
        {"shield", "M12 2 L21 6 L20 14 Q18 20 12 23 Q6 20 4 14 L3 6 Z M8 12 L11 15 L17 9"},
        {"extension", "M3 8 L8 8 L8 5 A3 3 0 1 1 14 5 L14 8 L21 8 L21 13 L18 13 A3 3 0 1 0 18 19 "
                      "L21 19 L21 22 L3 22 Z"},
        {"back", "M15 5 L8 12 L15 19 M8 12 L21 12"},
        {"forward", "M9 5 L16 12 L9 19 M16 12 L3 12"},
        {"reload", "M20 7 L20 2 M20 7 L15 7 M20 7 A9 9 0 1 0 21 15"},
        {"close", "M6 6 L18 18 M18 6 L6 18"},
        {"plus", "M12 5 L12 19 M5 12 L19 12"},
        {"search", "M10 3 A7 7 0 1 1 9.99 3 M15 15 L21 21"},
        {"star", "M12 3 L14.8 8.7 L21 9.6 L16.5 14 L17.6 20.2 L12 17.3 L6.4 20.2 L7.5 14 L3 9.6 "
                 "L9.2 8.7 Z"},
        {"history", "M4 4 L4 10 L10 10 M4 10 A9 9 0 1 1 3.5 16 M12 7 L12 12 L16 14"},
        {"download", "M12 3 L12 15 M7 10 L12 15 L17 10 M4 16 L4 21 L20 21 L20 16"},
        {"settings", "M12 8 A4 4 0 1 1 11.99 8 M12 2 L12 5 M12 19 L12 22 M2 12 L5 12 M19 12 L22 12 "
                     "M5 5 L7 7 M17 17 L19 19 M5 19 L7 17 M17 7 L19 5"},
        {"lock", "M7 11 L7 7 A5 5 0 0 1 17 7 L17 11 M5 11 L19 11 L19 21 L5 21 Z M12 15 L12 17"},
        {"globe", "M12 2 A10 10 0 1 1 11.99 2 M2 12 L22 12 M12 2 C5 8 5 16 12 22 C19 16 19 8 12 2"},
        {"moon", "M20 15 A9 9 0 1 1 9 3 A7 7 0 0 0 20 15 Z"},
        {"pin", "M9 3 L18 3 L16 8 L19 11 L13 17 L10 14 L5 16 L3 14 L9 9 Z M8 16 L3 21"},
        {"volume", "M3 9 L7 9 L12 5 L12 19 L7 15 L3 15 Z M16 8 Q20 12 16 16 M19 5 Q25 12 19 19"},
        {"folder", "M3 5 L9 5 L11 8 L21 8 L21 20 L3 20 Z"},
        {"check", "M4 12 L9 17 L20 6"},
        {"private", "M4 9 Q12 4 20 9 L18 18 L14 18 L12 15 L10 18 L6 18 Z M2 9 L22 9"},
        {"arrow", "M5 12 L19 12 M13 6 L19 12 L13 18"},
        {"pause", "M8 5 L8 19 M16 5 L16 19"},
        {"play", "M7 4 L20 12 L7 20 Z"}};
    auto it = paths.find(name);
    if (it == paths.end())
        return;
    auto path = SkParsePath::FromSVGString(it->second);
    if (!path)
        return;
    c.save();
    c.translate(x, y);
    c.scale(size / 24, size / 24);
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.65f);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setStrokeJoin(SkPaint::kRound_Join);
    c.drawPath(*path, p);
    c.restore();
}
std::string bytes(long long size) {
    if (size < 1024)
        return std::to_string(size) + " B";
    if (size < 1024 * 1024)
        return std::to_string(size / 1024) + " KB";
    return std::to_string(size / (1024 * 1024)) + " MB";
}
} // namespace
BrowserUi::BrowserUi(BrowserController &app, const std::filesystem::path &file) : app_(app) {
#ifdef _WIN32
    auto manager = SkFontMgr_New_DirectWrite();
#else
    auto manager = SkFontMgr_New_Custom_Empty();
#endif
    regular_ = manager->makeFromFile(utf8_path(file).c_str());
    if (!regular_)
        throw std::runtime_error("Inter font is missing");
    const SkFontArguments::VariationPosition::Coordinate coord{SkSetFourByteTag('w', 'g', 'h', 't'),
                                                               600};
    SkFontArguments args;
    args.setVariationDesignPosition({&coord, 1});
    medium_ = regular_->makeClone(args);
    if (!medium_)
        medium_ = regular_;
}
Rect BrowserUi::web_bounds() const {
    return {252, 88, std::max(100, width_ - 264), std::max(100, height_ - 100)};
}
Rect BrowserUi::address_bounds() const {
    return {384, 36, std::max(150, width_ - 628 - int(extension_buttons().size()) * 36), 38};
}
Rect BrowserUi::panel_bounds() const {
    const auto r = web_bounds();
    const int w = std::min(panel_ == Settings     ? 1000
                           : panel_ == Extensions ? 900
                           : panel_ == Shortcuts  ? 900
                           : panel_ == Passwords  ? 900
                           : panel_ == Import     ? 900
                           : panel_ == Filters    ? 900
                                                  : 660,
                           r.width - 24);
    const int h = std::min(panel_ == Settings     ? 760
                           : panel_ == Extensions ? 700
                           : panel_ == Shortcuts  ? 700
                           : panel_ == Passwords  ? 700
                           : panel_ == Import     ? 700
                           : panel_ == Filters    ? 700
                                                  : 580,
                           height_ - 124);
    return {r.x + (r.width - w) / 2, std::max(100, (height_ - h) / 2), w, h};
}
Rect BrowserUi::dialog_bounds() const {
    return {std::max(260, (width_ - 490) / 2), std::max(110, (height_ - 230) / 2), 490, 230};
}
void BrowserUi::focus_target(Rect r) {
    if (r.x >= 0 && r.y >= 0 && r.x + r.width <= width_ && r.y + r.height <= height_)
        focus_targets_.push_back(r);
}
void BrowserUi::resize(int width, int height) {
    visible_active_id_ = -1;
    keyboard_focus_ = -1;
    width_ = width;
    height_ = height;
}
void BrowserUi::draw_editor(SkCanvas &c, const TextEditor &e, Rect r, float size, bool active) {
    const auto &t = interface_theme(app_.profile);
    auto f = font(regular_, size);
    const float caret = measure(f, e.value.substr(0, e.caret));
    float offset = std::max(0.f, caret - r.width + 18);
    c.save();
    c.clipRect(box(r));
    if (active && e.caret != e.anchor) {
        const float a = measure(f, e.value.substr(0, e.begin()));
        const float b = measure(f, e.value.substr(0, e.end()));
        fill(c,
             {static_cast<int>(r.x + a - offset), r.y + 3, static_cast<int>(std::ceil(b - a)),
              r.height - 6},
             4, SkColorSetA(t.accent, 65));
    }
    label(c, regular_, e.value, r.x - offset, r.y + r.height / 2 + size * .35f, size, t.text);
    if (active) {
        SkPaint p;
        p.setColor(t.accent);
        p.setStrokeWidth(1.4f);
        c.drawLine(r.x + caret - offset, r.y + 5, r.x + caret - offset, r.y + r.height - 5, p);
    }
    c.restore();
}
bool BrowserUi::draw_web_frame(SkCanvas &c) {
    const auto current = app_.current();
    // A CEF repaint changes only the page. Preserve native chrome and its focus
    // targets; overlays use the complete painter so they remain above the page.
    if (!current || current->ntp || current->loading || current->page.failed ||
        !current->page.view.generation() || !web_backdrop_ || panel_ != None || editing ||
        find_open || app_.extension_popup || !app_.dialogs.empty() || !app_.menu.empty() ||
        app_.menu_action || app_.menu_callback || keyboard_focus_ >= 0 ||
        (app_.profile.show_status && !status.empty()) ||
        (!toast_.empty() && std::chrono::steady_clock::now() < toast_until_))
        return false;
    const auto web = web_bounds();
    c.save();
    c.resetMatrix();
    c.drawImage(web_backdrop_, backdrop_x_, backdrop_y_);
    c.restore();
    // Partition in device pixels so fractional UI scales never cut an
    // antialiased pixel in half. Only the narrow perimeter needs curved clips.
    const auto matrix = c.getTotalMatrix();
    const auto pixels = matrix.mapRect(box(web));
    const auto outer = pixels.roundOut();
    const float left = std::ceil(pixels.left()) + 1, right = std::floor(pixels.right()) - 1;
    const float top = std::ceil(pixels.top()) + 1, bottom = std::floor(pixels.bottom()) - 1;
    const float rx = std::ceil(15 * matrix.getScaleX()) + 1;
    const float ry = std::ceil(15 * matrix.getScaleY()) + 1;
    const SkRect interiors[] = {
        SkRect::MakeLTRB(left, top + ry, right, bottom - ry),
        SkRect::MakeLTRB(left + rx, top, right - rx, top + ry),
        SkRect::MakeLTRB(left + rx, bottom - ry, right - rx, bottom)};
    const SkRect perimeter[] = {
        SkRect::MakeLTRB(outer.left(), outer.top(), left + rx, top + ry),
        SkRect::MakeLTRB(right - rx, outer.top(), outer.right(), top + ry),
        SkRect::MakeLTRB(outer.left(), bottom - ry, left + rx, outer.bottom()),
        SkRect::MakeLTRB(right - rx, bottom - ry, outer.right(), outer.bottom()),
        SkRect::MakeLTRB(outer.left(), top + ry, left, bottom - ry),
        SkRect::MakeLTRB(right, top + ry, outer.right(), bottom - ry),
        SkRect::MakeLTRB(left + rx, outer.top(), right - rx, top),
        SkRect::MakeLTRB(left + rx, bottom, right - rx, outer.bottom())};
    const auto background = app_.profile.theme % 2 ? 0xFFFFFFFF : 0xFF161B25;
    const auto page_part = [&](const SkRect &rect, bool curved) {
        c.save();
        c.resetMatrix();
        c.clipRect(rect);
        c.setMatrix(matrix);
        fill(c, web, curved ? 15 : 0, background);
        if (curved)
            c.clipRRect(SkRRect::MakeRectXY(box(web), 15, 15), true);
        image(c, current->page.view, web);
        c.restore();
    };
    for (const auto &rect : interiors)
        page_part(rect, false);
    for (const auto &rect : perimeter)
        page_part(rect, true);
    c.save();
    c.clipRRect(SkRRect::MakeRectXY(box(web), 15, 15), true);
    if (current->page.popup_visible) {
        const auto &p = current->page.popup_bounds;
        image(c, current->page.popup,
              {web.x + int(p.x / app_.ui_scale), web.y + int(p.y / app_.ui_scale),
               int(p.width / app_.ui_scale), int(p.height / app_.ui_scale)});
    }
    c.restore();
    border(c, web, 15, SkColorSetA(interface_theme(app_.profile).text, 22));
    return true;
}
void BrowserUi::draw(SkCanvas &c) {
    ensure_wallpaper();
    const auto &t = interface_theme(app_.profile);
    const bool light = app_.profile.theme % 2;
    const auto ink = t.text, muted = t.muted;
    auto current = app_.current();
    const auto web = web_bounds();
    const auto addr = address_bounds();
    const int scope = !app_.dialogs.empty()                               ? 100
                      : app_.menu_action || app_.menu_callback            ? 101
                      : editing                                           ? 102
                      : panel_ == Extensions && app_.extensions.candidate ? 103
                                                                          : int(panel_);
    if (scope != focus_scope_)
        keyboard_focus_ = -1;
    focus_scope_ = scope;
    focus_targets_.clear();
    c.clear(t.background);
    if (!(current && current->ntp && app_.profile.ntp_background == "plain")) {
        glow(c, {30, 0}, 480, t.mesh[0]);
        glow(c, {static_cast<float>(width_), static_cast<float>(height_)}, 800, t.mesh[1]);
    }
    fill(c, {240, 0, width_ - 240, height_}, 0, SkColorSetA(t.background, 80));
    brand_logo(c, {20, 17, 30, 30});
    label(c, medium_, "aura", 58, 40, 23, ink);
    label(c, regular_, app_.tr("Personal space"), 24, 65, 11, muted);
    auto button = [&](Rect rect, const std::string &name, bool enabled = true,
                      bool selected = false) {
        if (enabled && !name.empty())
            focus_target(rect);
        const bool hover = enabled && inside(rect, hover_x_, hover_y_);
        if (hover || selected)
            fill(c, rect, 9, SkColorSetA(t.accent, selected ? 36 : 22));
        icon(c, name, rect.x + (rect.width - 19) / 2.f, rect.y + (rect.height - 19) / 2.f, 19,
             enabled ? ink : SkColorSetA(muted, 90));
    };
    focus_target({16, 88, 208, 40});
    fill(c, {16, 88, 208, 40}, 11, SkColorSetA(t.surface, light ? 205 : 150));
    border(c, {16, 88, 208, 40}, 11, SkColorSetA(ink, 18));
    icon(c, "plus", 29, 99, 18, t.accent);
    focus_target({16, 12, 208, 62});
    label(c, medium_, app_.tr("New tab"), 59, 113, 13, ink);
    label(c, regular_, "Ctrl T", 170, 112, 10, muted);
    label(c, regular_, app_.tr("Tabs"), 24, 161, 12, muted);
    label(c, regular_, std::to_string(app_.tabs.size()), 207, 161, 9, muted);
    const int list_bottom = height_ - 220;
    int tab_capacity = std::max(1, (list_bottom - 178) / 52);
    if (current && (current->id != visible_active_id_ || app_.active != visible_active_index_)) {
        if (app_.active < tab_scroll_)
            tab_scroll_ = app_.active;
        else if (app_.active >= tab_scroll_ + tab_capacity)
            tab_scroll_ = app_.active - tab_capacity + 1;
        visible_active_id_ = current->id;
        visible_active_index_ = app_.active;
    }
    tab_scroll_ =
        std::clamp(tab_scroll_, 0, std::max(0, static_cast<int>(app_.tabs.size()) - tab_capacity));
    c.save();
    c.clipRect(SkRect::MakeLTRB(12, 176, 232, list_bottom));
    for (int i = tab_scroll_; i < static_cast<int>(app_.tabs.size()); ++i) {
        auto &tab = *app_.tabs[i];
        const int y = 178 + (i - tab_scroll_) * 52;
        if (y + 46 > list_bottom)
            break;
        const bool selected = i == app_.active;
        Rect r{14, y, 212, 46};
        focus_target({r.x, r.y, 180, r.height});
        if (!tab.pinned)
            focus_target({200, y + 8, 24, 30});
        const bool hover = inside(r, hover_x_, hover_y_);
        if (selected || hover) {
            fill(c, r, 12, SkColorSetA(t.surface, selected ? (light ? 235 : 245) : 125));
            if (selected) {
                border(c, r, 12, SkColorSetA(ink, 22));
                fill(c, {14, y + 13, 3, 20}, 2, t.accent);
            }
        }
        if (tab.loading) {
            const double time =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            fill(c, {29, y + 17, 12, 12}, 6,
                 SkColorSetA(t.accent, static_cast<U8CPU>(130 + 100 * (app_.profile.reduce_motion
                                                                           ? 0.0
                                                                           : std::sin(time * 4)))));
        } else if (tab.private_tab)
            icon(c, "private", 26, y + 14, 19, t.accent);
        else if (app_.profile.show_favicons && tab.favicon.generation())
            image(c, tab.favicon, {27, y + 15, 17, 17});
        else
            icon(c, tab.ntp ? "plus" : "globe", 26, y + 14, 19, selected ? t.accent : muted);
        label(c, selected ? medium_ : regular_,
              tab.page.title.empty() ? url_host(tab.page.url) : tab.page.title, 57,
              y + (app_.profile.show_tab_host ? 21 : 28), 12.5f, ink, 138);
        if (app_.profile.show_tab_host)
            label(c, regular_,
                  tab.private_tab ? app_.tr("Private browsing")
                  : tab.ntp       ? app_.tr("A fresh perspective")
                                  : url_host(tab.page.url),
                  57, y + 36, 10, muted, 139);
        if (hover || selected)
            icon(c, tab.pinned ? "pin" : "close", 202, y + 16, 14, muted);
    }
    c.restore();
    const std::string labels[] = {app_.tr("Bookmarks"), app_.tr("History"), app_.tr("Downloads"),
                                  app_.tr("Settings")};
    const char *icons[] = {"star", "history", "download", "settings"};
    for (int i = 0; i < 4; ++i) {
        Rect r{16, height_ - 204 + i * 35, 208, 32};
        focus_target(r);
        if (inside(r, hover_x_, hover_y_) || panel_ == i + 1)
            fill(c, r, 9, SkColorSetA(t.surface, 180));
        icon(c, icons[i], 28, r.y + 7, 18, muted);
        label(c, regular_, labels[i], 58, r.y + 21, 12, ink);
        if (i == 2 && !app_.downloads.empty())
            label(c, medium_, std::to_string(app_.downloads.size()), 204, r.y + 20, 10, t.accent);
    }
    fill(c, {20, height_ - 48, 28, 28}, 10, SkColorSetA(t.accent, 35));
    label(c, medium_, current && current->private_tab ? "P" : "A", 29, height_ - 29, 12, t.accent);
    label(c, medium_,
          current && current->private_tab ? app_.tr("Private space") : app_.tr("Personal space"),
          58, height_ - 31, 11, ink);
    label(c, regular_, app_.tr("Local profile"), 58, height_ - 17, 9, muted);
    const std::string title = current ? current->page.title : "Aura";
    label(c, regular_, title, 265, 21, 10, muted, width_ - 435);
    label(c, regular_, current && current->private_tab ? app_.tr("PRIVATE") : "", width_ - 240, 21,
          9, t.accent);
    button({width_ - 144, 0, 48, 30}, "", true);
    SkPaint line;
    line.setAntiAlias(true);
    line.setColor(muted);
    line.setStrokeWidth(1.2f);
    c.drawLine(width_ - 125, 16, width_ - 114, 16, line);
    line.setStyle(SkPaint::kStroke_Style);
    c.drawRect(SkRect::MakeXYWH(width_ - 77, 10, 10, 10), line);
    icon(c, "close", width_ - 39, 7, 17, muted);
    button({260, 37, 34, 36}, "back", current && current->back);
    button({298, 37, 34, 36}, "forward", current && current->forward);
    button({336, 37, 34, 36}, current && current->loading ? "close" : "reload");
    focus_target(addr);
    fill(c, addr, 12, SkColorSetA(t.surface, light ? 230 : 185));
    border(c, addr, 12, SkColorSetA(muted, 190));
    icon(c,
         current && current->private_tab                        ? "private"
         : current && current->page.url.starts_with("https://") ? "lock"
                                                                : "search",
         addr.x + 12, addr.y + 10, 17, muted);
    label(c, regular_,
          current && !current->ntp ? current->page.url : app_.tr("Search or enter an address"),
          addr.x + 40, addr.y + 24, 12, current && !current->ntp ? ink : muted, addr.width - 110);
    label(c, regular_, "Ctrl L", addr.x + addr.width - 48, addr.y + 24, 9, muted);
    button({width_ - 150, 37, 34, 36}, "star", true,
           current && app_.profile.bookmarked(current->page.url));
    button({width_ - 110, 37, 34, 36}, "download", true, panel_ == Downloads);
    button({width_ - 230, 37, 34, 36}, "shield", true,
           app_.request_policy->protection->active(current ? current->page.url : ""));
    button({width_ - 190, 37, 34, 36}, "extension", true, panel_ == Extensions);
    button({width_ - 70, 37, 34, 36}, "settings", true, panel_ == Settings);
    for (const auto &[index, b] : extension_buttons()) {
        focus_target(b);
        const auto &e = app_.extensions.items[index];
        const bool usable = e.load_requested && !(current && current->private_tab);
        if (inside(b, hover_x_, hover_y_) ||
            (app_.extension_popup && app_.extension_popup->extension_id == e.id))
            fill(c, b, 9, SkColorSetA(t.accent, 30));
        const auto &bitmap = app_.extension_icon(index);
        if (bitmap.generation()) {
            c.save();
            if (!usable)
                c.saveLayerAlpha(nullptr, 95);
            image(c, bitmap, {b.x + 7, b.y + 8, 20, 20});
            if (!usable)
                c.restore();
            c.restore();
        } else
            icon(c, "extension", b.x + 7, b.y + 8, 20, usable ? ink : muted);
    }
    if (current && !current->ntp) {
        shadow(c, web, 15);
        // Cache only the native backdrop, before drawing CEF pixels. Restoring
        // it prevents antialiased corners and borders from darkening over time.
        web_backdrop_.reset();
        if (!c.recordingContext()) {
            const auto pixels = c.getTotalMatrix().mapRect(box(web)).roundOut();
            SkBitmap backing;
            if (backing.tryAllocPixels(c.imageInfo().makeWH(pixels.width(), pixels.height())) &&
                c.readPixels(backing, pixels.left(), pixels.top())) {
                backing.setImmutable();
                web_backdrop_ = SkImages::RasterFromBitmap(backing);
                backdrop_x_ = pixels.left();
                backdrop_y_ = pixels.top();
            }
        }
        fill(c, web, 15, light ? 0xFFFFFFFF : 0xFF161B25);
        c.save();
        c.clipRRect(SkRRect::MakeRectXY(box(web), 15, 15), true);
        image(c, current->page.view, web);
        if (current->page.popup_visible) {
            auto p = current->page.popup_bounds;
            image(c, current->page.popup,
                  {web.x + int(p.x / app_.ui_scale), web.y + int(p.y / app_.ui_scale),
                   int(p.width / app_.ui_scale), int(p.height / app_.ui_scale)});
        }
        if (current->page.failed) {
            fill(c, web, 0, t.background);
            icon(c, "globe", web.x + 64, web.y + 70, 38, t.accent);
            label(c, medium_, app_.tr("This page couldn't be reached"), web.x + 64, web.y + 158, 29,
                  ink, web.width - 100);
            label(c, regular_, current->page.error, web.x + 64, web.y + 195, 13, muted,
                  web.width - 100);
            fill(c, {web.x + 64, web.y + 225, 125, 38}, 10, t.accent);
            centered_label(c, medium_, app_.tr("Try again"), {web.x + 64, web.y + 225, 125, 38}, 12,
                           t.background);
        } else if (!current->page.view.generation()) {
            label(c, regular_, app_.tr("Opening ") + url_host(current->page.url) + "…", web.x + 40,
                  web.y + 65, 14, muted);
        }
        c.restore();
        border(c, web, 15, SkColorSetA(ink, 22));
        if (current->loading)
            fill(c,
                 {web.x + 12, web.y, std::max(20, static_cast<int>((web.width - 24) * progress_)),
                  2},
                 2, t.accent);
    } else {
        draw_start_page(c);
    }
    if (app_.profile.show_status && !status.empty() && !editing && panel_ == None) {
        Rect r{web.x + 9, height_ - 43,
               std::min(web.width - 18, static_cast<int>(status.size() * 7 + 22)), 25};
        fill(c, r, 7, t.surface);
        label(c, regular_, status, r.x + 10, r.y + 17, 10, muted, r.width - 20);
    }
    if (find_open) {
        Rect r{width_ - 390, 94, 360, 49};
        shadow(c, r, 12);
        fill(c, r, 12, t.surface);
        border(c, r, 12, SkColorSetA(ink, 28));
        draw_editor(c, find, {r.x + 14, r.y + 9, 190, 32}, 13, !editing);
        label(c, regular_,
              std::to_string(app_.find_active) + " / " + std::to_string(app_.find_matches),
              r.x + 216, r.y + 29, 10, muted);
        icon(c, "back", r.x + 280, r.y + 16, 17, muted);
        icon(c, "forward", r.x + 304, r.y + 16, 17, muted);
        icon(c, "close", r.x + 333, r.y + 16, 17, muted);
    }
    if (current && !chrome_store_id(current->page.url).empty()) {
        Rect store{web.x + 16, web.y + 14, web.width - 32, 49};
        shadow(c, store, 12);
        fill(c, store, 12, t.surface);
        border(c, store, 12, SkColorSetA(t.accent, 90));
        label(c, medium_, app_.tr("Chrome Web Store"), store.x + 16, store.y + 30, 13, ink,
              store.width - 200);
        fill(c, {store.x + store.width - 171, store.y + 8, 158, 33}, 8, t.accent);
        centered_label(c, medium_, app_.tr("Install in Aura"),
                       {store.x + store.width - 171, store.y + 8, 158, 33}, 12, t.background);
    }
    if (panel_ != None) {
        fill(c, {240, 30, width_ - 240, height_ - 30}, 0, SkColorSetA(0xFF050712, light ? 40 : 90));
        focus_targets_.clear();
        const auto r = panel_bounds();
        focus_target({r.x + r.width - 52, r.y + 16, 36, 36});
        shadow(c, r, design::radii::overlay);
        fill(c, r, design::radii::overlay, t.surface);
        border(c, r, design::radii::overlay, SkColorSetA(ink, 30));
        const std::string titles[] = {"",
                                      app_.tr("Bookmarks"),
                                      app_.tr("History"),
                                      app_.tr("Downloads"),
                                      app_.tr("Settings"),
                                      app_.tr("Extensions"),
                                      app_.tr("Quick links"),
                                      app_.tr("About Aura and updates"),
                                      app_.tr("Import browser data"),
                                      app_.tr("Passwords"),
                                      app_.tr("Filter lists")};
        label(c, medium_, titles[panel_], r.x + 28, r.y + 47, 23, ink);
        icon(c, "close", r.x + r.width - 43, r.y + 26, 19, muted);
        if (panel_ == History || panel_ == Bookmarks) {
            auto &list = panel_ == History ? app_.profile.history : app_.profile.bookmarks;
            std::string info = panel_ == History
                                   ? app_.tr("Your recent places, stored on this device.")
                                   : app_.tr("Good places, kept close.");
            label(c, regular_, info, r.x + 28, r.y + 73, 12, muted);
            if (list.empty())
                label(c, regular_,
                      panel_ == History ? app_.tr("Your history starts with your first page.")
                                        : app_.tr("Press Ctrl D on a page to save it here."),
                      r.x + 28, r.y + 140, 14, muted);
            c.save();
            c.clipRect(
                SkRect::MakeLTRB(r.x + 12, r.y + 97, r.x + r.width - 12, r.y + r.height - 18));
            for (int i = panel_scroll_; i < static_cast<int>(list.size()); ++i) {
                int y = r.y + 103 + (i - panel_scroll_) * 61;
                if (y > r.y + r.height - 30)
                    break;
                Rect row{r.x + 16, y, r.width - 32, 55};
                if (y + 55 <= r.y + r.height - 18) {
                    focus_target({row.x, y, row.width - 50, 55});
                    focus_target({r.x + r.width - 58, y + 8, 36, 36});
                }
                if (inside(row, hover_x_, hover_y_))
                    fill(c, row, 10, SkColorSetA(t.accent, 22));
                icon(c, panel_ == Bookmarks ? "star" : "globe", row.x + 12, y + 16, 19, t.accent);
                label(c, medium_, list[i].title, row.x + 45, y + 23, 13, ink, row.width - 100);
                label(c, regular_, list[i].url, row.x + 45, y + 42, 11, muted, row.width - 100);
                icon(c, "close", r.x + r.width - 48, y + 19, 16, muted);
            }
            c.restore();
        }
        if (panel_ == Downloads) {
            label(c, regular_, app_.tr("Files are saved in your local profile's downloads folder."),
                  r.x + 28, r.y + 75, 12, muted);
            fill(c, {r.x + 25, r.y + 91, 132, 31}, 8, SkColorSetA(t.accent, 25));
            icon(c, "folder", r.x + 35, r.y + 98, 17, t.accent);
            centered_label(c, medium_, app_.tr("Show folder"), {r.x + 25, r.y + 91, 132, 31}, 11,
                           ink, 34, 8);
            focus_target({r.x + 25, r.y + 91, 132, 31});
            if (app_.downloads.empty())
                label(c, regular_, app_.tr("Your downloads will appear here."), r.x + 28, r.y + 169,
                      14, muted);
            c.save();
            c.clipRect(
                SkRect::MakeLTRB(r.x + 12, r.y + 132, r.x + r.width - 12, r.y + r.height - 14));
            for (std::size_t i = panel_scroll_; i < app_.downloads.size(); ++i) {
                auto &d = app_.downloads[i];
                int y = r.y + 141 + (static_cast<int>(i) - panel_scroll_) * 75;
                if (y > r.y + r.height - 20)
                    break;
                icon(c, d.complete ? "check" : "download", r.x + 29, y + 7, 20, t.accent);
                label(c, medium_, d.name.empty() ? app_.tr("Downloading…") : d.name, r.x + 62,
                      y + 21, 13, ink, r.width - 170);
                label(c, regular_,
                      d.cancelled  ? app_.tr("Cancelled")
                      : d.complete ? app_.tr("Complete · ") + bytes(d.received)
                      : d.interrupted
                          ? app_.tr("Interrupted · reason ") + std::to_string(d.interrupt_reason)
                      : d.paused ? app_.tr("Paused")
                                 : bytes(d.received) + " / " + bytes(d.total),
                      r.x + 62, y + 42, 11, muted);
                if (!d.complete && !d.cancelled) {
                    if (y + 50 <= r.y + r.height - 14) {
                        focus_target({r.x + r.width - 98, y + 5, 36, 36});
                        focus_target({r.x + r.width - 57, y + 5, 36, 36});
                    }
                    icon(c, d.paused || d.interrupted ? "play" : "pause", r.x + r.width - 89,
                         y + 13, 18, muted);
                    icon(c, "close", r.x + r.width - 48, y + 13, 18, muted);
                    fill(c, {r.x + 62, y + 52, r.width - 94, 3}, 2, SkColorSetA(t.accent, 30));
                    fill(c,
                         {r.x + 62, y + 52,
                          static_cast<int>((r.width - 94) * std::max(0, d.percent) / 100.f), 3},
                         2, t.accent);
                }
            }
            c.restore();
        }
        if (panel_ == Settings)
            draw_settings(c, r);
        if (panel_ == Extensions)
            draw_extensions(c, r);
        if (panel_ == Shortcuts)
            draw_shortcuts(c, r);
        if (panel_ == Updates)
            draw_updates(c, r);
        if (panel_ == Import)
            draw_import(c, r);
        if (panel_ == Passwords)
            draw_passwords(c, r);
        if (panel_ == Filters)
            draw_filters(c, r);
    }

    if (editing) {
        focus_targets_.clear();
        fill(c, {240, 30, width_ - 240, height_ - 30}, 0,
             SkColorSetA(0xFF050712, static_cast<U8CPU>((light ? 65 : 115) * focus_animation_)));
        Rect r{std::max(264, web.x + (web.width - 760) / 2), 96, std::min(760, web.width - 28), 85};
        auto list = suggestions();
        r.height = 85 + static_cast<int>(list.size()) * 54;
        shadow(c, r, 18);
        fill(c, r, 18, t.surface);
        border(c, r, 18, SkColorSetA(t.accent, 65));
        icon(c, "search", r.x + 20, r.y + 25, 23, t.accent);
        draw_editor(c, address, {r.x + 59, r.y + 15, r.width - 87, 43}, 18, true);
        label(c, regular_,
              app_.tr("Enter to navigate or search with ") +
                  search_engine_name(app_.profile.search_engine),
              r.x + 59, r.y + 69, 10, muted);
        for (std::size_t i = 0; i < list.size(); ++i) {
            Rect row{r.x + 9, r.y + 83 + static_cast<int>(i) * 54, r.width - 18, 50};
            if (static_cast<int>(i) == suggestion_ || inside(row, hover_x_, hover_y_))
                fill(c, row, 10, SkColorSetA(t.accent, 25));
            icon(c, app_.profile.bookmarked(list[i].url) ? "star" : "history", row.x + 13,
                 row.y + 15, 18, t.accent);
            label(c, medium_, list[i].title, row.x + 46, row.y + 21, 12, ink, row.width - 66);
            label(c, regular_, list[i].url, row.x + 46, row.y + 39, 10, muted, row.width - 66);
        }
    }
    draw_extension_popup(c);
    if (!app_.extension_popup && panel_ == None && !editing) {
        for (const auto &[index, b] : extension_buttons())
            if (inside(b, hover_x_, hover_y_)) {
                auto text = app_.extensions.items[index].name;
                int w = std::min(320, int(measure(font(regular_, 12), text)) + 24);
                Rect tip{std::clamp(b.x + b.width - w, web.x + 8, width_ - w - 12), 82, w, 34};
                shadow(c, tip, 8);
                fill(c, tip, 8, t.surface);
                label(c, regular_, text, tip.x + 12, tip.y + 22, 12, ink, w - 24);
            }
    }
    if (!app_.extension_popup && panel_ == None && !editing && app_.dialogs.empty() &&
        !app_.menu_callback && !app_.menu_action) {
        const std::pair<Rect, std::string> tips[] = {
            {{260, 37, 34, 36}, app_.tr("Back") + " (Alt+←)"},
            {{298, 37, 34, 36}, app_.tr("Forward") + " (Alt+→)"},
            {{336, 37, 34, 36}, app_.tr("Reload") + " (Ctrl+R)"},
            {{width_ - 230, 37, 34, 36}, app_.tr("Protection")},
            {{width_ - 190, 37, 34, 36}, app_.tr("Extensions")},
            {{width_ - 150, 37, 34, 36}, app_.tr("Bookmarks") + " (Ctrl+D)"},
            {{width_ - 110, 37, 34, 36}, app_.tr("Downloads") + " (Ctrl+J)"},
            {{width_ - 70, 37, 34, 36}, app_.tr("Settings")}};
        for (const auto &[b, text] : tips)
            if (inside(b, hover_x_, hover_y_)) {
                const int w = int(measure(font(regular_, 12), text)) + 24;
                Rect tip{std::clamp(b.x, web.x + 8, width_ - w - 12), 82, w, 34};
                shadow(c, tip, 8);
                fill(c, tip, 8, t.surface);
                border(c, tip, 8, SkColorSetA(t.text, 60));
                label(c, regular_, text, tip.x + 12, tip.y + 22, 12, ink);
            }
    }
    if (app_.menu_callback || app_.menu_action) {
        focus_targets_.clear();
        Rect r{app_.menu_x, app_.menu_y, app_.menu_width,
               static_cast<int>(app_.menu.size()) * 32 + 12};
        shadow(c, r, 12);
        fill(c, r, 12, t.surface);
        border(c, r, 12, SkColorSetA(ink, 30));
        for (std::size_t i = 0; i < app_.menu.size(); ++i) {
            auto &item = app_.menu[i];
            Rect row{r.x + 5, r.y + 6 + static_cast<int>(i) * 32, r.width - 10, 31};
            if (item.label.empty())
                fill(c, {row.x + 9, row.y + 15, row.width - 18, 1}, 0, SkColorSetA(ink, 20));
            else {
                if (item.enabled)
                    focus_target(row);
                if (inside(row, hover_x_, hover_y_) && item.enabled)
                    fill(c, row, 7, SkColorSetA(t.accent, 28));
                label(c, regular_, item.label, row.x + 13, row.y + 21, 12,
                      item.enabled ? ink : muted,
                      row.width - 26 - (item.shortcut.empty() ? 0 : 110));
                if (!item.shortcut.empty())
                    label(c, regular_, item.shortcut, row.x + row.width - 110, row.y + 21, 11,
                          muted, 100);
            }
        }
    }
    if (!app_.dialogs.empty()) {
        focus_targets_.clear();
        auto &dialog = app_.dialogs.front();
        const auto r = dialog_bounds();
        focus_target({r.x + r.width - 217, r.y + r.height - 60, 90, 35});
        focus_target({r.x + r.width - 114, r.y + r.height - 60, 90, 35});
        fill(c, {240, 30, width_ - 240, height_ - 30}, 0, 0x99050712);
        shadow(c, r, 18);
        fill(c, r, 18, t.surface);
        label(c, medium_,
              dialog.before_unload ? app_.tr("Leave this page?")
              : dialog.owned       ? app_.tr(dialog.origin)
                                   : dialog.origin,
              r.x + 25, r.y + 38, 16, ink, r.width - 50);
        const auto lines =
            wrap_text(font(regular_, 13),
                      dialog.before_unload ? app_.tr("This page may contain unsaved changes.")
                      : dialog.owned       ? app_.tr(dialog.message)
                                           : dialog.message,
                      r.width - 50, dialog.prompt ? 2 : 4);
        for (size_t i = 0; i < lines.size(); ++i)
            label(c, regular_, lines[i], r.x + 25, r.y + 78 + int(i) * 18, 13, muted, r.width - 50);
        if (dialog.prompt) {
            fill(c, {r.x + 25, r.y + 101, r.width - 50, 37}, 8, t.background);
            draw_editor(c, dialog.input, {r.x + 37, r.y + 104, r.width - 74, 31}, 13, true);
        }
        fill(c, {r.x + r.width - 217, r.y + r.height - 60, 90, 35}, 9, SkColorSetA(ink, 14));
        const auto cancel = app_.tr(dialog.permission ? "Deny" : "Cancel");
        centered_label(c, medium_, cancel, {r.x + r.width - 217, r.y + r.height - 60, 90, 35}, 12,
                       ink);
        fill(c, {r.x + r.width - 114, r.y + r.height - 60, 90, 35}, 9, t.accent);
        const auto accept = app_.tr(!dialog.accept_label.empty() ? dialog.accept_label.c_str()
                                    : dialog.permission          ? "Allow"
                                    : dialog.before_unload       ? "Leave"
                                                                 : "OK");
        centered_label(c, medium_, accept, {r.x + r.width - 114, r.y + r.height - 60, 90, 35}, 12,
                       t.background);
    }
    if (keyboard_focus_ >= 0 && keyboard_focus_ < int(focus_targets_.size()) &&
        !app_.extension_popup) {
        auto r = focus_targets_[keyboard_focus_];
        r = {r.x - 2, r.y - 2, r.width + 4, r.height + 4};
        border(c, r, 8, t.accent);
        border(c, {r.x - 1, r.y - 1, r.width + 2, r.height + 2}, 9, t.accent);
    }
    if (!toast_.empty() && std::chrono::steady_clock::now() < toast_until_) {
        const int w =
            std::min(web.width - 36, static_cast<int>(measure(font(regular_, 12), toast_)) + 50);
        Rect r{web.x + (web.width - w) / 2, height_ - 67, w, 39};
        shadow(c, r, 12);
        fill(c, r, 12, t.surface);
        border(c, r, 12, SkColorSetA(t.accent, 45));
        label(c, regular_, toast_, r.x + 24, r.y + 25, 12, ink, w - 45);
    }
}
namespace {
constexpr const char *settings_sections[] = {"General", "Tabs",        "Appearance", "Webpages",
                                             "Privacy", "Permissions", "Downloads",  "Protection",
                                             "Data",    "Start page"};
std::string fold_settings(std::string text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xD0 && i + 1 < text.size()) {
            auto next = static_cast<unsigned char>(text[++i]);
            if (next == 0x81) {
                out += '\xD1';
                out += '\x91';
            } else if (next >= 0x90 && next <= 0x9F) {
                out += '\xD0';
                out += static_cast<char>(next + 32);
            } else if (next >= 0xA0 && next <= 0xAF) {
                out += '\xD1';
                out += static_cast<char>(next - 32);
            } else {
                out += static_cast<char>(c);
                out += static_cast<char>(next);
            }
        } else
            out += c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    }
    return out;
}
} // namespace
int BrowserUi::choice_columns(const SettingSpec &spec, Rect r) const {
    float minimum = 116;
    for (const auto &choice : spec.choices)
        minimum = std::max(minimum, measure(font(medium_, 12), app_.tr(choice.label)) + 56);
    return std::min(int(spec.choices.size()), std::max(1, int((r.width + 8) / (minimum + 8))));
}
Rect BrowserUi::setting_choice(const SettingSpec &spec, Rect r, int index) const {
    const int columns = choice_columns(spec, r), w = (r.width - (columns - 1) * 8) / columns;
    return {r.x + (index % columns) * (w + 8), r.y + 66 + (index / columns) * 38, w, 32};
}
Rect BrowserUi::settings_category_bounds(Rect r, int index) const {
    const int height = std::min(34, std::max(24, (r.height - 188) / 10));
    return {r.x + 18, r.y + 128 + index * height, 150, height};
}
BrowserUi::StartPageLayout BrowserUi::start_page_layout() const {
    StartPageLayout l;
    const auto &p = app_.profile;
    const auto web = web_bounds();
    l.scale = p.ntp_zoom / 100.f;
    const float z = l.scale;
    const int gap = std::max(8, int(24 * z));
    l.viewport = {web.x + 16, web.y + 12, web.width - 32, web.height - 72};
    const int preferred = p.ntp_width == "compact" ? 480 : p.ntp_width == "wide" ? 820 : 620;
    const int width = std::min(l.viewport.width - 16, int(preferred * z));
    const int left = web.x + (web.width - width) / 2;
    int y = 0;
    auto block = [&](int height) {
        Rect r{left, y, width, height};
        y += height + gap;
        return r;
    };
    if (p.ntp_clock)
        l.clock = block(int(32 * z));
    if (p.ntp_logo) {
        l.logo = block(int(46 * z));
        l.logo.x = web.x + (web.width - int(46 * z)) / 2;
        l.logo.width = int(46 * z);
    }
    const bool private_tab = app_.current() && app_.current()->private_tab;
    if (p.ntp_greeting) {
        const auto title = private_tab           ? app_.tr("A little room for privacy.")
                           : p.ntp_title.empty() ? app_.tr("Space for your next idea.")
                                                 : p.ntp_title;
        l.heading_lines = wrap_text(font(medium_, 32 * z), title, width, 128);
        l.heading = block(int(l.heading_lines.size() * 40 * z));
        if (p.ntp_subtitle_visible) {
            const auto text =
                private_tab ? app_.tr("This tab stays out of your history and restored session.")
                : p.ntp_subtitle.empty()
                    ? app_.tr("A quieter browser. An open world. Make it yours.")
                    : p.ntp_subtitle;
            l.subtitle_lines = wrap_text(font(regular_, 13 * z), text, width, 128);
            l.subtitle = block(int(l.subtitle_lines.size() * 19 * z));
        }
    }
    l.search = block(int(58 * z));
    if (p.quick_links) {
        int columns = p.ntp_link_columns == "auto" ? std::clamp(int(width / (160 * z)), 1, 6)
                                                   : std::stoi(p.ntp_link_columns);
        const int space = int(16 * z), tile_width = (width - (columns - 1) * space) / columns;
        const int tile_height = int((p.ntp_link_style == "rows" ? 48 : 80) * z);
        int count = 0;
        for (size_t i = 0; i < p.shortcuts.size(); ++i) {
            l.links.push_back(
                {int(i),
                 {left + (count % columns) * (tile_width + space),
                  y + (count / columns) * (tile_height + space), tile_width, tile_height}});
            ++count;
        }
        if (count)
            y += ((count + columns - 1) / columns) * (tile_height + space) - space;
    } else
        y -= gap;
    l.maximum = std::max(0, y - l.viewport.height);
    l.scroll = std::clamp(start_scroll_, 0, l.maximum);
    const int offset = l.viewport.y + std::max(0, (l.viewport.height - y) / 2) - l.scroll;
    for (auto r : {&l.clock, &l.logo, &l.heading, &l.subtitle, &l.search})
        if (r->height)
            r->y += offset;
    for (auto &[index, r] : l.links)
        r.y += offset;
    l.zoom_out = {web.x + 16, web.y + web.height - 44, 34, 32};
    l.zoom_reset = {web.x + 54, l.zoom_out.y, 64, 32};
    l.zoom_in = {web.x + 122, l.zoom_out.y, 34, 32};
    l.customize = {web.x + web.width - 204, l.zoom_out.y, 188, 32};
    l.manage = {web.x + 168, l.zoom_out.y, 160, 32};
    return l;
}
void BrowserUi::zoom_start_page(int delta, bool reset) {
    app_.profile.ntp_zoom = reset ? 100 : std::clamp(app_.profile.ntp_zoom + delta * 10, 50, 200);
    start_scroll_ = 0;
    keyboard_focus_ = -1;
    app_.changed(true);
}
void BrowserUi::draw_start_page(SkCanvas &c) {
    const auto &p = app_.profile;
    const auto t = interface_theme(p);
    const auto web = web_bounds();
    const auto current = app_.current();
    if ((current && current->id != start_tab_) || start_zoom_ != p.ntp_zoom) {
        start_scroll_ = 0;
        start_tab_ = current ? current->id : -1;
        start_zoom_ = p.ntp_zoom;
    }
    const auto l = start_page_layout();
    start_scroll_ = l.scroll;
    const float z = l.scale;
    if (p.ntp_background == "image" && wallpaper_.image) {
        const auto &im = wallpaper_.image;
        float factor =
            p.ntp_image_fit == "cover"
                ? std::max(float(web.width) / im->width(), float(web.height) / im->height())
                : std::min(float(web.width) / im->width(), float(web.height) / im->height());
        float w = im->width() * factor, h = im->height() * factor;
        c.save();
        c.clipRect(box(web));
        fill(c, web, 0, t.background);
        c.drawImageRect(
            im, SkRect::MakeXYWH(web.x + (web.width - w) / 2, web.y + (web.height - h) / 2, w, h),
            SkSamplingOptions(SkFilterMode::kLinear));
        fill(c, web, 0, SkColorSetA(SK_ColorBLACK, p.ntp_image_dim * 255 / 100));
        c.restore();
    }
    if (p.ntp_background == "aura")
        glow(c, {float(web.x + web.width / 2), float(web.y + web.height / 2)}, 500, t.mesh[2]);
    auto visible_target = [&](Rect r) {
        if (r.y >= l.viewport.y && r.y + r.height <= l.viewport.y + l.viewport.height)
            focus_target(r);
    };
    c.save();
    c.clipRect(box(l.viewport));
    if (p.ntp_background == "image" && wallpaper_.image) {
        int top = l.search.y, bottom = l.search.y + l.search.height;
        for (auto r : {l.clock, l.logo, l.heading, l.subtitle})
            if (r.height)
                top = std::min(top, r.y);
        for (const auto &[index, r] : l.links)
            bottom = std::max(bottom, r.y + r.height);
        fill(c, {l.search.x - 8, top - 12, l.search.width + 16, bottom - top + 24}, 16,
             SkColorSetA(t.background, 210));
    }

    if (l.clock.height) {
        const auto now = std::time(nullptr);
        const auto local = local_time(now);
        char value[32];
        std::strftime(value, sizeof(value), p.clock_24 ? "%H:%M" : "%I:%M %p", &local);
        centered_label(c, medium_, value, l.clock, 26 * z, t.accent, 0, 0);
    }
    if (l.logo.height) {
        const auto r = l.logo;
        brand_logo(c, r);
    }
    for (size_t i = 0; i < l.heading_lines.size(); ++i)
        centered_label(c, medium_, l.heading_lines[i],
                       {l.heading.x, l.heading.y + int(i * 40 * z), l.heading.width, int(40 * z)},
                       32 * z, t.text, 0, 0);
    for (size_t i = 0; i < l.subtitle_lines.size(); ++i)
        centered_label(
            c, regular_, l.subtitle_lines[i],
            {l.subtitle.x, l.subtitle.y + int(i * 19 * z), l.subtitle.width, int(19 * z)}, 13 * z,
            t.muted, 0, 0);
    const auto search = l.search;
    visible_target(search);
    fill(c, search, 17 * z, t.surface);
    border(c, search, 17 * z, SkColorSetA(t.muted, 190));
    icon(c, "search", search.x + 20 * z, search.y + (search.height - 21 * z) / 2, 21 * z, t.accent);
    centered_label(c, regular_, app_.tr("Search the web or enter a URL"), search, 14 * z, t.muted,
                   int(56 * z), int(58 * z));
    Rect arrow{search.x + search.width - int(50 * z), search.y + int(12 * z), int(34 * z),
               int(34 * z)};
    fill(c, arrow, 10 * z, SkColorSetA(t.accent, 30));
    icon(c, "arrow", arrow.x + 8 * z, arrow.y + 8 * z, 18 * z, t.accent);
    for (const auto &[index, r] : l.links) {
        visible_target(r);
        const auto &link = p.shortcuts[index];
        const auto host = url_host(link.url);
        const std::string name = link.title.empty() ? host : link.title;
        fill(c, r, 12 * z, SkColorSetA(t.surface, inside(r, hover_x_, hover_y_) ? 255 : 185));
        border(c, r, 12 * z, SkColorSetA(t.text, 25));
        if (p.ntp_link_style == "rows") {
            if (p.ntp_link_icons)
                icon(c, "globe", r.x + 14 * z, r.y + (r.height - 19 * z) / 2, 19 * z, t.accent);
            centered_label(c, medium_, name, r, 12 * z, t.text,
                           int((p.ntp_link_icons ? 40 : 12) * z), int(12 * z));
        } else {
            if (p.ntp_link_icons)
                icon(c, "globe", r.x + 16 * z, r.y + 12 * z, 19 * z, t.accent);
            label(c, medium_, name, r.x + 16 * z, r.y + (p.ntp_link_icons ? 53 : 34) * z, 12 * z,
                  t.text, r.width - 24 * z);
            label(c, regular_, host, r.x + 16 * z, r.y + (p.ntp_link_icons ? 69 : 54) * z, 10 * z,
                  t.muted, r.width - 24 * z);
        }
    }
    c.restore();
    auto button = [&](Rect r, const std::string &text, bool enabled = true) {
        if (enabled)
            focus_target(r);
        fill(c, r, 8, SkColorSetA(t.surface, 230));
        border(c, r, 8, SkColorSetA(t.muted, inside(r, hover_x_, hover_y_) && enabled ? 220 : 90));
        centered_label(c, medium_, text, r, 12, enabled ? t.text : t.muted, 4, 4);
    };
    button(l.zoom_out, "−", p.ntp_zoom > 50);
    button(l.zoom_reset, std::to_string(p.ntp_zoom) + "%");
    button(l.zoom_in, "+", p.ntp_zoom < 200);
    button(l.customize, app_.tr("Customize start page"));
    button(l.manage, app_.tr("Quick links"));
    if (l.maximum) {
        const int track = l.viewport.height,
                  thumb = std::max(24, track * track / (track + l.maximum));
        fill(c,
             {web.x + web.width - 7, l.viewport.y + l.scroll * (track - thumb) / l.maximum, 4,
              thumb},
             2, t.muted);
    }
}
std::string BrowserUi::start_page_report() const {
    if (!app_.current() || !app_.current()->ntp)
        return "null";
    const auto l = start_page_layout();
    auto rect = [](Rect r) {
        return "[" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
               std::to_string(r.width) + "," + std::to_string(r.height) + "]";
    };
    std::string out =
        "{\"zoom\":" + std::to_string(app_.profile.ntp_zoom) +
        ",\"scroll\":" + std::to_string(l.scroll) + ",\"maximum\":" + std::to_string(l.maximum) +
        ",\"viewport\":" + rect(l.viewport) + ",\"search\":" + rect(l.search) +
        ",\"customize\":" + rect(l.customize) + ",\"manage\":" + rect(l.manage) +
        ",\"zoom_out\":" + rect(l.zoom_out) + ",\"zoom_reset\":" + rect(l.zoom_reset) +
        ",\"zoom_in\":" + rect(l.zoom_in) + ",\"links\":[";
    for (size_t i = 0; i < l.links.size(); ++i) {
        if (i)
            out += ',';
        out += "{\"url\":\"" + json_escape(app_.profile.shortcuts[l.links[i].first].url) +
               "\",\"bounds\":" + rect(l.links[i].second) + "}";
    }
    const auto t = interface_theme(app_.profile);
    out += "],\"image_loaded\":" + std::string(wallpaper_.image ? "true" : "false") +
           ",\"image_error\":\"" + json_escape(wallpaper_error_) +
           "\",\"picker_pid\":" + std::to_string(picker_pid_) + ",\"theme_colors\":[" +
           std::to_string(t.background) + "," + std::to_string(t.surface) + "," +
           std::to_string(t.text) + "," + std::to_string(t.muted) + "," + std::to_string(t.accent) +
           "]}";
    return out;
}
BrowserUi::~BrowserUi() {
    clear_password_fields();
#ifndef _WIN32
    if (picker_fd_ >= 0)
        close(picker_fd_);
    if (picker_pid_ > 0) {
        kill(picker_pid_, SIGTERM);
        waitpid(picker_pid_, nullptr, WNOHANG);
    }
#endif
}
void BrowserUi::ensure_wallpaper() {
    const auto &path = app_.profile.ntp_image_path;
    if (path == wallpaper_key_)
        return;
    wallpaper_key_ = path;
    wallpaper_ = {};
    app_.profile.image_palette.reset();
    if (path.empty())
        return;
    wallpaper_ = read_wallpaper(path_from_utf8(path));
    wallpaper_error_ = wallpaper_.error;
    wallpaper_.encoded.clear();
    wallpaper_.encoded.shrink_to_fit();
    if (wallpaper_.image)
        app_.profile.image_palette = wallpaper_.palette;
}
bool BrowserUi::import_wallpaper(const std::string &value) {
    if (value.empty()) {
        std::error_code ignored;
        for (const auto name : {"wallpaper.png", "wallpaper.jpg", "wallpaper.tmp"})
            std::filesystem::remove(app_.profile.directory / name, ignored);
        app_.profile.ntp_image_path.clear();
        app_.profile.ntp_background = "aura";
        wallpaper_key_.clear();
        wallpaper_ = {};
        wallpaper_error_.clear();
        app_.profile.image_palette.reset();
        app_.changed(true);
        return true;
    }
    auto candidate = read_wallpaper(path_from_utf8(value));
    if (!candidate.image) {
        wallpaper_error_ = candidate.error;
        app_.changed();
        return false;
    }
    const auto target = app_.profile.directory / ("wallpaper" + candidate.extension);
    const auto temporary = app_.profile.directory / "wallpaper.tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(candidate.encoded.data()), candidate.encoded.size());
    file.close();
    std::error_code ec;
    if (!file) {
        wallpaper_error_ = "Could not save the image in your profile.";
        return false;
    }
    replace_file(temporary, target, ec);
    if (ec) {
        wallpaper_error_ = "Could not save the image in your profile.";
        return false;
    }
    std::error_code ignored;
    std::filesystem::remove(app_.profile.directory /
                                (candidate.extension == ".png" ? "wallpaper.jpg" : "wallpaper.png"),
                            ignored);
    candidate.encoded.clear();
    candidate.encoded.shrink_to_fit();
    wallpaper_ = std::move(candidate);
    wallpaper_error_.clear();
    wallpaper_key_ = utf8_path(target);
    app_.profile.ntp_image_path = wallpaper_key_;
    app_.profile.ntp_background = "image";
    app_.profile.image_palette = wallpaper_.palette;
    app_.changed(true);
    return true;
}
void BrowserUi::choose_wallpaper() {
    choose_file(false);
}
void BrowserUi::choose_file(bool for_import, bool for_filters) {
    if (for_filters && app_.filter_lists().busy())
        return;
    if (for_import && app_.importer.busy)
        return;
    const auto error_text = for_filters ? "Could not open the file chooser. Add rules manually."
                            : for_import
                                ? "Could not open the file chooser. Paste a file path instead."
                                : "Could not open the image chooser. Paste a file path instead.";
#ifdef _WIN32
    std::wstring file(32768, L'\0');
    const auto title = CefString(app_.tr(for_filters  ? "Import custom filter rules"
                                         : for_import ? "Choose browser data"
                                                      : "Choose background image"))
                           .ToWString();
    const auto initial = app_.profile.directory.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = app_.window->handle();
    dialog.lpstrFilter =
        for_filters ? L"Filter rules\0*.txt;*.list\0All files\0*.*\0\0"
        : for_import
            ? (app_.profile.language == "ru"
                   ? L"Данные браузера\0*.html;*.htm;*.json;*.sqlite;History;Bookmarks\0Все "
                     L"файлы\0*.*\0\0"
                   : L"Browser data\0*.html;*.htm;*.json;*.sqlite;History;Bookmarks\0All "
                     L"files\0*.*\0\0")
            : L"PNG / JPEG\0*.png;*.jpg;*.jpeg\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrTitle = title.c_str();
    dialog.lpstrInitialDir = initial.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        file.resize(wcslen(file.c_str()));
        if (for_filters)
            app_.filter_lists().import_custom(std::filesystem::path(file));
        else if (for_import) {
            import_input_.set(utf8_path(std::filesystem::path(file)));
            import_input_focus_ = false;
            app_.importer.preview(import_input_.value);
        } else if (!import_wallpaper(utf8_path(std::filesystem::path(file))))
            toast(app_.tr(wallpaper_error_));
    } else if (CommDlgExtendedError())
        toast(app_.tr(error_text));
    app_.changed();
#else
    if (picker_pid_ > 0)
        return;
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK)) {
        toast(app_.tr(error_text));
        return;
    }
    std::vector<std::string> args = {
        "env",
        "GDK_BACKEND=x11",
        "GSK_RENDERER=cairo",
        "GTK_USE_PORTAL=0",
        "LIBGL_ALWAYS_SOFTWARE=1",
        "zenity",
        "--file-selection",
        "--modal",
        "--title=" + app_.tr(for_filters  ? "Import custom filter rules"
                             : for_import ? "Choose browser data"
                                          : "Choose background image"),
        for_filters  ? "--file-filter=Filter rules | *.txt *.list"
        : for_import ? "--file-filter=" + app_.tr("Browser data") +
                           " | *.html *.htm *.json *.sqlite History Bookmarks"
                     : "--file-filter=PNG / JPEG | *.png *.PNG *.jpg *.JPG *.jpeg *.JPEG",
        "--file-filter=" + app_.tr("All files") + " | *",
        "--filename=" + utf8_path(app_.profile.directory) + "/"};
    std::vector<char *> argv;
    for (auto &s : args)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    pid_t child;
    int status = posix_spawnp(&child, "env", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (status) {
        close(pipes[0]);
        toast(app_.tr(error_text));
        return;
    }
    picker_pid_ = child;
    picker_import_ = for_import;
    picker_filters_ = for_filters;
    picker_fd_ = pipes[0];
    picker_output_.clear();
    settings_edit_.clear();
    keyboard_focus_ = -1;
    app_.changed();
#endif
}
void BrowserUi::poll_wallpaper_picker() {
#ifndef _WIN32
    if (picker_pid_ < 0)
        return;
    char buffer[4096];
    ssize_t count;
    while ((count = read(picker_fd_, buffer, sizeof(buffer))) > 0)
        if (picker_output_.size() + count <= 8192)
            picker_output_.append(buffer, count);
    int status = 0;
    if (waitpid(picker_pid_, &status, WNOHANG) <= 0)
        return;
    while ((count = read(picker_fd_, buffer, sizeof(buffer))) > 0)
        if (picker_output_.size() + count <= 8192)
            picker_output_.append(buffer, count);
    close(picker_fd_);
    picker_fd_ = -1;
    picker_pid_ = -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        while (!picker_output_.empty() &&
               (picker_output_.back() == '\n' || picker_output_.back() == '\r'))
            picker_output_.pop_back();
        if (!picker_output_.empty()) {
            if (picker_filters_)
                app_.filter_lists().import_custom(path_from_utf8(picker_output_));
            else if (picker_import_) {
                import_input_.set(picker_output_);
                import_input_focus_ = false;
                app_.importer.preview(picker_output_);
            } else if (!import_wallpaper(picker_output_))
                toast(app_.tr(wallpaper_error_));
        }
        settings_error_.clear();
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 1)
        toast(app_.tr(picker_filters_ ? "Could not open the file chooser. Add rules manually."
                      : picker_import_
                          ? "Could not open the file chooser. Paste a file path instead."
                          : "Could not open the image chooser. Paste a file path instead."));
    picker_output_.clear();
    app_.changed();
#endif
}
void BrowserUi::open_shortcuts(int index) {
    panel(Shortcuts);
    shortcut_edit_ = index >= 0 && size_t(index) < app_.profile.shortcuts.size() ? index : -1;
    shortcut_title_.set(shortcut_edit_ >= 0 ? app_.profile.shortcuts[index].title : "");
    shortcut_url_.set(shortcut_edit_ >= 0 ? app_.profile.shortcuts[index].url : "");
    shortcut_error_.clear();
    shortcut_focus_ = 0;
}
void BrowserUi::save_shortcut() {
    auto &p = app_.profile;
    bool ok = shortcut_edit_ < 0
                  ? p.add_shortcut(shortcut_title_.value, shortcut_url_.value)
                  : p.edit_shortcut(shortcut_edit_, shortcut_title_.value, shortcut_url_.value);
    if (!ok) {
        shortcut_error_ = app_.tr("Enter a valid http:// or https:// address and a short name.");
        app_.changed();
        return;
    }
    if (shortcut_edit_ < 0)
        panel_scroll_ = std::max(0, int(p.shortcuts.size()) - 3);
    p.quick_links = true;
    shortcut_edit_ = -1;
    shortcut_focus_ = -1;
    shortcut_title_.set("");
    shortcut_url_.set("");
    shortcut_error_.clear();
    app_.changed(true);
}
void BrowserUi::remove_shortcut(size_t index) {
    if (index >= app_.profile.shortcuts.size())
        return;
    shortcut_undo_ = {{index, app_.profile.shortcuts[index]}};
    app_.profile.remove_shortcut(index);
    shortcut_edit_ = -1;
    shortcut_focus_ = -1;
    shortcut_title_.set("");
    shortcut_url_.set("");
    panel_scroll_ = std::min(panel_scroll_, std::max(0, int(app_.profile.shortcuts.size()) - 1));
    toast(app_.tr("Shortcut removed"));
    app_.changed(true);
}
void BrowserUi::draw_shortcuts(SkCanvas &c, Rect r) {
    const auto t = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &text, bool primary = false) {
        focus_target(b);
        fill(c, b, 8,
             primary ? t.accent : SkColorSetA(t.accent, inside(b, hover_x_, hover_y_) ? 32 : 18));
        centered_label(c, medium_, app_.tr(text), b, 12, primary ? t.background : t.text, 6, 6);
    };
    label(c, regular_, app_.tr("Name (optional)"), r.x + 28, r.y + 78, 12, t.muted);
    label(c, regular_, app_.tr("Website address"), r.x + 28, r.y + 143, 12, t.muted);
    const Rect fields[] = {{r.x + 28, r.y + 90, r.width - 56, 34},
                           {r.x + 28, r.y + 155, r.width - 56, 34}};
    for (int i = 0; i < 2; ++i) {
        const auto b = fields[i];
        focus_target(b);
        fill(c, b, 8, SkColorSetA(t.text, 7));
        border(c, b, 8, shortcut_focus_ == i ? t.accent : SkColorSetA(t.muted, 190));
        draw_editor(c, i == 0 ? shortcut_title_ : shortcut_url_,
                    {b.x + 10, b.y + 1, b.width - 20, b.height - 2}, 12, shortcut_focus_ == i);
    }
    button({r.x + 28, r.y + 202, 184, 34}, shortcut_edit_ < 0 ? "Add shortcut" : "Save changes",
           true);
    if (shortcut_edit_ >= 0)
        button({r.x + 224, r.y + 202, 104, 34}, "Cancel");
    label(c, regular_,
          shortcut_error_.empty() ? app_.tr("Add as many shortcuts as you need. Dragging is not "
                                            "required: use the arrows to reorder.")
                                  : shortcut_error_,
          r.x + 28, r.y + 260, 12, shortcut_error_.empty() ? t.muted : t.text, r.width - 56);
    c.save();
    c.clipRect(SkRect::MakeLTRB(r.x + 16, r.y + 270, r.x + r.width - 16, r.y + r.height - 60));
    for (size_t i = panel_scroll_; i < app_.profile.shortcuts.size(); ++i) {
        const auto &link = app_.profile.shortcuts[i];
        int y = r.y + 274 + int(i - panel_scroll_) * 64;
        if (y + 58 > r.y + r.height - 60)
            break;
        fill(c, {r.x + 20, y, r.width - 40, 58}, 8, SkColorSetA(t.text, 5));
        label(c, medium_, link.title.empty() ? url_host(link.url) : link.title, r.x + 32, y + 22,
              13, t.text, r.width - 352);
        label(c, regular_, link.url, r.x + 32, y + 43, 11, t.muted, r.width - 352);
        if (y + 58 <= r.y + r.height - 60) {
            button({r.x + r.width - 300, y + 12, 76, 34}, "Edit");
            button({r.x + r.width - 216, y + 12, 32, 34}, "↑");
            button({r.x + r.width - 176, y + 12, 32, 34}, "↓");
            button({r.x + r.width - 136, y + 12, 108, 34}, "Remove");
        }
    }
    c.restore();
    label(c, regular_, std::to_string(app_.profile.shortcuts.size()) + " " + app_.tr("quick links"),
          r.x + 28, r.y + r.height - 26, 12, t.muted);
    if (shortcut_undo_)
        button({r.x + r.width - 136, r.y + r.height - 47, 108, 32}, "Undo");
}
void BrowserUi::shortcuts_pointer(Rect r, int x, int y) {
    shortcut_focus_ = -1;
    for (int i = 0; i < 2; ++i)
        if (inside({r.x + 28, r.y + (i == 0 ? 90 : 155), r.width - 56, 34}, x, y)) {
            shortcut_focus_ = i;
            app_.window->caret(x, y + 22);
            return;
        }
    if (inside({r.x + 28, r.y + 202, 184, 34}, x, y)) {
        save_shortcut();
        return;
    }
    if (shortcut_edit_ >= 0 && inside({r.x + 224, r.y + 202, 104, 34}, x, y)) {
        open_shortcuts();
        return;
    }
    if (shortcut_undo_ && inside({r.x + r.width - 136, r.y + r.height - 47, 108, 32}, x, y)) {
        auto [index, link] = *shortcut_undo_;
        index = std::min(index, app_.profile.shortcuts.size());
        app_.profile.shortcuts.insert(app_.profile.shortcuts.begin() + index, std::move(link));
        shortcut_undo_.reset();
        toast(app_.tr("Shortcut restored"));
        app_.changed(true);
        return;
    }
    if (y < r.y + 274 || y >= r.y + r.height - 60)
        return;
    const size_t index = panel_scroll_ + (y - r.y - 274) / 64;
    if (index >= app_.profile.shortcuts.size())
        return;
    int top = r.y + 274 + int(index - panel_scroll_) * 64;
    if (top + 58 > r.y + r.height - 60)
        return;
    if (inside({r.x + r.width - 300, top + 12, 76, 34}, x, y)) {
        open_shortcuts(index);
        panel_scroll_ = int(index);
    } else if (inside({r.x + r.width - 216, top + 12, 32, 34}, x, y) && index > 0) {
        app_.profile.move_shortcut(index, index - 1);
        app_.changed(true);
    } else if (inside({r.x + r.width - 176, top + 12, 32, 34}, x, y) &&
               index + 1 < app_.profile.shortcuts.size()) {
        app_.profile.move_shortcut(index, index + 1);
        app_.changed(true);
    } else if (inside({r.x + r.width - 136, top + 12, 108, 34}, x, y))
        remove_shortcut(index);
}
std::string BrowserUi::shortcuts_report() const {
    if (panel_ != Shortcuts)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"title\":" + rect({r.x + 28, r.y + 90, r.width - 56, 34}) +
                      ",\"url\":" + rect({r.x + 28, r.y + 155, r.width - 56, 34}) +
                      ",\"save\":" + rect({r.x + 28, r.y + 202, 184, 34}) +
                      ",\"undo\":" + rect({r.x + r.width - 136, r.y + r.height - 47, 108, 32}) +
                      ",\"error\":\"" + json_escape(shortcut_error_) + "\",\"title_value\":\"" +
                      json_escape(shortcut_title_.value) + "\",\"url_value\":\"" +
                      json_escape(shortcut_url_.value) + "\",\"items\":[";
    for (size_t i = 0; i < app_.profile.shortcuts.size(); ++i) {
        if (i)
            out += ',';
        const auto &link = app_.profile.shortcuts[i];
        int y = r.y + 274 + (int(i) - panel_scroll_) * 64;
        out += "{\"title\":\"" + json_escape(link.title) + "\",\"url\":\"" + json_escape(link.url) +
               "\",\"visible\":" +
               (i >= size_t(panel_scroll_) && y + 58 <= r.y + r.height - 60 ? "true" : "false") +
               ",\"edit\":" + rect({r.x + r.width - 300, y + 12, 76, 34}) +
               ",\"up\":" + rect({r.x + r.width - 216, y + 12, 32, 34}) +
               ",\"down\":" + rect({r.x + r.width - 176, y + 12, 32, 34}) +
               ",\"remove\":" + rect({r.x + r.width - 136, y + 12, 108, 34}) + "}";
    }
    return out + "]}";
}
void BrowserUi::draw_updates(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    label(c, medium_, "Aura " + std::string(browser_version), r.x + 28, r.y + 94, 18, t.text,
          r.width - 56);
    auto &updates = app_.updates;
    label(c, regular_,
          app_.tr(updates.status) + (updates.status == "Downloading the update..."
                                         ? " " + std::to_string(updates.download_percent()) + "%"
                                         : ""),
          r.x + 28, r.y + 127, 12, t.muted, r.width - 56);
    if (updates.release)
        label(c, medium_, app_.tr("Available version: ") + updates.release->version, r.x + 28,
              r.y + 157, 13, t.accent, r.width - 56);
    const auto kind = updates.installation_kind();
    auto button = [&](int index, const std::string &text, bool enabled) {
        Rect b{r.x + 28, r.y + 185 + index * 46, r.width - 56, 36};
        if (enabled)
            focus_target(b);
        fill(c, b, 9, SkColorSetA(t.accent, enabled && inside(b, hover_x_, hover_y_) ? 30 : 12));
        border(c, b, 9, SkColorSetA(t.muted, enabled ? 180 : 60));
        centered_label(c, medium_, app_.tr(text), b, 12, enabled ? t.text : t.muted, 12, 12);
    };
    button(0, updates.busy() ? "Cancel" : "Check for updates", updates.ready_file.empty());
    button(1, "Download verified update",
           bool(updates.release) && !updates.busy() && updates.ready_file.empty() &&
               kind != "system");
    const bool installer = kind == "run" || kind == "setup";
    button(2, installer ? "Close Aura and install update" : "Show verified download",
           !updates.ready_file.empty());
    button(3, "Release notes and downloads", !updates.busy());
    const auto help = kind == "system" ? "Update this installation with your Linux package manager."
                      : kind == "portable"
                          ? "Unpack the verified ZIP separately. Keep your existing profile."
                          : "Your profile is kept. Installation starts after Aura closes.";
    label(c, regular_, app_.tr(help), r.x + 28, r.y + 405, 12, t.muted, r.width - 56);
}
void BrowserUi::updates_pointer(Rect r, int x, int y) {
    auto &updates = app_.updates;
    for (int index = 0; index < 4; ++index) {
        if (!inside({r.x + 28, r.y + 185 + index * 46, r.width - 56, 36}, x, y))
            continue;
        if (index == 0 && updates.ready_file.empty()) {
            if (updates.busy())
                updates.cancel();
            else
                updates.check();
        } else if (index == 1)
            updates.download();
        else if (index == 2 && !updates.ready_file.empty()) {
            const auto kind = updates.installation_kind();
            if (kind == "run" || kind == "setup")
                app_.confirm_action("Install Aura update",
                                    "Your profile is kept. Installation starts after Aura closes.",
                                    [this] {
                                        app_.install_update_on_exit = true;
                                        app_.request_exit(true);
                                    });
            else
                updates.open_download_folder();
        } else if (index == 3 && !updates.busy())
            app_.new_tab(updates.release
                             ? updates.release->page_url()
                             : "https://github.com/Sttanors/aura-browser/releases/latest");
        app_.changed();
        return;
    }
}
std::string BrowserUi::updates_report() const {
    if (panel_ != Updates)
        return "null";
    const auto r = panel_bounds();
    std::string out = "{\"buttons\":[";
    for (int index = 0; index < 4; ++index) {
        if (index)
            out += ',';
        out += "[" + std::to_string(r.x + 28) + "," + std::to_string(r.y + 185 + index * 46) + "," +
               std::to_string(r.width - 56) + ",36]";
    }
    return out + "]}";
}
void BrowserUi::draw_import(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    auto &importer = app_.importer;
    label(c, regular_, "HTML / JSON · Firefox places.sqlite · Chromium History", r.x + 28, r.y + 88,
          12, t.muted, r.width - 56);
    Rect input{r.x + 28, r.y + 108, r.width - 172, 36};
    if (!importer.busy)
        focus_target(input);
    fill(c, input, 9, SkColorSetA(t.text, 7));
    border(c, input, 9, import_input_focus_ ? t.accent : SkColorSetA(t.muted, 190));
    if (import_input_.value.empty() && !import_input_focus_)
        label(c, regular_, app_.tr("Paste a file path or choose Browse"), input.x + 12,
              input.y + 24, 12, t.muted, input.width - 24);
    else
        draw_editor(c, import_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 12,
                    import_input_focus_);
    auto button = [&](Rect b, const std::string &text, bool enabled) {
        if (enabled)
            focus_target(b);
        fill(c, b, 9, SkColorSetA(t.accent, enabled && inside(b, hover_x_, hover_y_) ? 30 : 12));
        border(c, b, 9, SkColorSetA(t.muted, enabled ? 180 : 60));
        centered_label(c, medium_, app_.tr(text), b, 12, enabled ? t.text : t.muted, 10, 10);
    };
    button({r.x + r.width - 132, r.y + 108, 104, 36}, "Browse", !importer.busy);
    button({r.x + 28, r.y + 162, 200, 36}, importer.busy ? "Cancel" : "Preview import",
           importer.busy || !import_input_.value.empty());
    button({r.x + 244, r.y + 162, 220, 36}, "Detected profiles", !importer.busy);
    label(c, regular_, app_.tr(importer.status), r.x + 28, r.y + 225, 12, t.muted, r.width - 56);
    if (auto data = importer.candidate.get()) {
        label(c, medium_, app_.tr(data->source), r.x + 28, r.y + 257, 15, t.text, r.width - 56);
        label(c, regular_,
              data->limited ? app_.tr("This is a partial preview of a large data set.")
                            : app_.tr("Skipped or duplicate records: ") +
                                  std::to_string(data->skipped + data->duplicates),
              r.x + 28, r.y + 283, 12, t.muted, r.width - 56);
        auto check = [&](int row, const std::string &title, size_t count, bool selected) {
            Rect b{r.x + 28, r.y + 302 + row * 40, r.width - 56, 32};
            if (count)
                focus_target(b);
            fill(c, {b.x, b.y + 6, 20, 20}, 5, SkColorSetA(t.accent, selected && count ? 45 : 10));
            border(c, {b.x, b.y + 6, 20, 20}, 5, SkColorSetA(t.muted, count ? 190 : 70));
            if (selected && count)
                icon(c, "check", b.x + 4, b.y + 10, 12, t.accent);
            label(c, regular_, app_.tr(title) + " · " + std::to_string(count), b.x + 32, b.y + 23,
                  13, count ? t.text : t.muted, b.width - 32);
        };
        check(0, "Bookmarks", data->bookmarks.size(), import_bookmarks_);
        check(1, "History", data->history.size(), import_history_);
        label(c, regular_, app_.tr("History limit: ") + std::to_string(app_.profile.history_limit),
              r.x + 28, r.y + 393, 12, t.muted, r.width - 56);
        if (r.height >= 620) {
            label(c, medium_, app_.tr("Sample addresses"), r.x + 28, r.y + 431, 12, t.text);
            const auto &values =
                import_bookmarks_ && !data->bookmarks.empty() ? data->bookmarks : data->history;
            for (size_t i = 0; i < std::min<size_t>(3, values.size()); ++i)
                label(c, regular_, values[i].title + " · " + values[i].url, r.x + 28,
                      r.y + 458 + i * 25, 12, t.muted, r.width - 56);
        }
    } else if (importer.result) {
        const auto &result = *importer.result;
        const std::pair<std::string, size_t> counts[] = {
            {"Bookmarks added: ", result.bookmarks_added},
            {"History entries added: ", result.history_added},
            {"Duplicate records: ", result.duplicates},
            {"Excluded by history limits: ", result.history_limited}};
        for (size_t i = 0; i < std::size(counts); ++i)
            label(c, regular_, app_.tr(counts[i].first) + std::to_string(counts[i].second),
                  r.x + 28, r.y + 265 + int(i) * 28, 13, t.text, r.width - 56);
    }
    const bool selected =
        importer.candidate && ((import_bookmarks_ && !importer.candidate->bookmarks.empty()) ||
                               (import_history_ && !importer.candidate->history.empty()));
    button({r.x + 28, r.y + r.height - 82, 240, 36}, "Import selected data",
           selected && !importer.busy);
    if (importer.result && !importer.result->backup.empty())
        button({r.x + r.width - 256, r.y + r.height - 82, 228, 36}, "Show backup folder", true);
    label(c, regular_, app_.tr("Bookmarks use a flat list. History keeps your current limits."),
          r.x + 28, r.y + r.height - 22, 11, t.muted, r.width - 56);
}
void BrowserUi::import_pointer(Rect r, int x, int y) {
    auto &importer = app_.importer;
    import_input_focus_ = false;
    if (!importer.busy && inside({r.x + 28, r.y + 108, r.width - 172, 36}, x, y)) {
        import_input_focus_ = true;
        app_.window->caret(x, y + 24);
    } else if (!importer.busy && inside({r.x + r.width - 132, r.y + 108, 104, 36}, x, y))
        choose_file(true);
    else if (!importer.busy && inside({r.x + 244, r.y + 162, 220, 36}, x, y)) {
        auto sources = discover_browser_imports(browser_import_roots());
        if (sources.empty()) {
            toast(app_.tr("No browser profiles found. Choose a file instead."));
        } else {
            app_.close_menu();
            app_.menu_x = r.x + 244;
            app_.menu_y = r.y + 204;
            const int limit = std::clamp((height_ - app_.menu_y - 22) / 32, 1, 12);
            for (size_t i = 0; i < std::min<size_t>(sources.size(), limit); ++i)
                app_.menu.push_back({int(i + 1), sources[i].label});
            app_.menu_action = [this, sources](int id) {
                if (id < 1 || static_cast<size_t>(id) > sources.size())
                    return;
                import_input_.set(utf8_path(sources[id - 1].path));
                app_.importer.preview(import_input_.value);
                app_.changed();
            };
        }
    } else if (inside({r.x + 28, r.y + 162, 200, 36}, x, y)) {
        if (importer.busy)
            importer.cancel();
        else if (!import_input_.value.empty())
            importer.preview(import_input_.value);
    } else if (importer.candidate && !importer.busy) {
        if (!importer.candidate->bookmarks.empty() &&
            inside({r.x + 28, r.y + 302, r.width - 56, 32}, x, y))
            import_bookmarks_ = !import_bookmarks_;
        else if (!importer.candidate->history.empty() &&
                 inside({r.x + 28, r.y + 342, r.width - 56, 32}, x, y))
            import_history_ = !import_history_;
        else if (inside({r.x + 28, r.y + r.height - 82, 240, 36}, x, y)) {
            importer.apply(app_.profile, import_bookmarks_, import_history_);
            app_.changed();
        }
    } else if (importer.result && !importer.result->backup.empty() &&
               inside({r.x + r.width - 256, r.y + r.height - 82, 228, 36}, x, y))
        app_.open_directory(importer.result->backup);
    app_.changed();
}
std::string BrowserUi::import_report() const {
    if (panel_ != Import)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    auto &importer = app_.importer;
    std::string out =
        "{\"input\":" + rect({r.x + 28, r.y + 108, r.width - 172, 36}) +
        ",\"browse\":" + rect({r.x + r.width - 132, r.y + 108, 104, 36}) +
        ",\"preview\":" + rect({r.x + 28, r.y + 162, 200, 36}) +
        ",\"profiles\":" + rect({r.x + 244, r.y + 162, 220, 36}) +
        ",\"bookmarks\":" + rect({r.x + 28, r.y + 302, r.width - 56, 32}) +
        ",\"history\":" + rect({r.x + 28, r.y + 342, r.width - 56, 32}) +
        ",\"apply\":" + rect({r.x + 28, r.y + r.height - 82, 240, 36}) +
        ",\"path\":" + json_quote(import_input_.value) +
        ",\"status\":" + json_quote(importer.status) +
        ",\"busy\":" + (importer.busy ? "true" : "false") + ",\"preview_bookmarks\":" +
        std::to_string(importer.candidate ? importer.candidate->bookmarks.size() : 0) +
        ",\"preview_history\":" +
        std::to_string(importer.candidate ? importer.candidate->history.size() : 0) +
        ",\"ready\":" + (importer.candidate ? "true" : "false");
    if (importer.result)
        out += ",\"result\":{\"bookmarks\":" + std::to_string(importer.result->bookmarks_added) +
               ",\"history\":" + std::to_string(importer.result->history_added) +
               ",\"backup\":" + json_quote(utf8_path(importer.result->backup)) + "}";
    return out + "}";
}
std::vector<BrowserUi::SettingsRow> BrowserUi::settings_rows(Rect panel) const {
    std::vector<SettingsRow> rows;
    const auto query = fold_settings(settings_search_.value);
    const auto exact = find_setting(settings_search_.value);
    const int left = panel.x + 188, w = panel.width - 216;
    int y = panel.y + 128;
    for (const auto &spec : settings_catalog()) {
        if (!query.empty()) {
            if (exact && exact->id != spec.id)
                continue;
            const auto haystack =
                fold_settings(spec.id + " " + spec.title + " " + spec.description + " " +
                              app_.tr(spec.title) + " " + app_.tr(spec.description));
            if (haystack.find(query) == std::string::npos)
                continue;
        } else if (spec.section != settings_sections[settings_section_])
            continue;
        int h = 90;
        Rect r{left, y, w, h};
        if (spec.kind == SettingKind::Choice)
            h = 76 + 38 * ((spec.choices.size() + choice_columns(spec, r) - 1) /
                           choice_columns(spec, r));
        else if (spec.kind == SettingKind::Text)
            h = settings_error_ == spec.id ? 142 : 118;
        else if (spec.kind == SettingKind::Themes)
            h = 248;
        r.height = h;
        rows.push_back({&spec, "", spec.title, spec.description, r});
        y += h;
    }
    const struct {
        const char *id, *section, *title, *description;
    } actions[] = {
        {"passwords", "Data", "Passwords", "Manage saved logins and encrypted transfer files."},
        {"import", "Data", "Import browser data",
         "Preview bookmarks and history from Firefox, Chrome or an HTML export."},
        {"updates", "General", "About Aura and updates",
         "View your version and check for verified browser updates."},
        {"shortcuts", "Start page", "Manage quick links",
         "Add, edit, remove and reorder your shortcuts."},
        {"wallpaper_clear", "Start page", "Remove background image",
         "Return to the Aura background. Your original image is kept."},
        {"filters", "Protection", "Filter lists and custom rules",
         "Update EasyList and EasyPrivacy, choose lists and edit your own rules."},
        {"extensions", "Protection", "Manage extensions",
         "Install extensions from Chrome Web Store or a signed CRX3 file."},
        {"history", "Data", "Clear Aura history",
         "Remove the Aura history list. Keep cookies and bookmarks."},
        {"cookies", "Data", "Clear cookies",
         "Sign out of websites. Bookmarks and downloads are kept."},
        {"cache", "Data", "Clear cached files",
         "Remove cached network files. Websites may load more slowly once."},
        {"reset", "Data", "Reset settings",
         "Restore preferences. Keep history, bookmarks and downloaded files."},
        {"folder", "Downloads", "Show folder",
         "Open the current download folder in the file manager."},
        {"downloads", "Downloads", "Clear completed downloads",
         "Remove finished entries from the panel. Keep the files."}};
    for (const auto &a : actions) {
        if (!query.empty()) {
            if (exact)
                continue;
            if (fold_settings(std::string(a.title) + " " + a.description + " " + app_.tr(a.title) +
                              " " + app_.tr(a.description))
                    .find(query) == std::string::npos)
                continue;
        } else if (std::string(a.section) != settings_sections[settings_section_])
            continue;
        rows.push_back({nullptr, a.id, a.title, a.description, {left, y, w, 118}});
        y += 118;
    }
    return rows;
}
int BrowserUi::settings_max_scroll(Rect r) const {
    const auto rows = settings_rows(r);
    return rows.empty() ? 0
                        : std::max(0, rows.back().bounds.y + rows.back().bounds.height -
                                          (r.y + r.height - 54));
}
void BrowserUi::draw_settings(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    const auto ink = t.text, muted = t.muted;
    Rect search{r.x + 28, r.y + 66, r.width - 56, 38};
    focus_target(search);
    fill(c, search, 10, SkColorSetA(ink, 7));
    border(
        c, search, 10,
        SkColorSetA(settings_search_focus_ ? t.accent : muted, settings_search_focus_ ? 255 : 190));
    icon(c, "search", search.x + 12, search.y + 10, 18, muted);
    if (settings_search_.value.empty() && !settings_search_focus_)
        label(c, regular_, app_.tr("Search settings"), search.x + 40, search.y + 25, 12, muted);
    else
        draw_editor(c, settings_search_, {search.x + 40, search.y + 3, search.width - 86, 32}, 12,
                    settings_search_focus_);
    if (!settings_search_.value.empty())
        icon(c, "close", search.x + search.width - 28, search.y + 11, 16, muted);
    const auto rows = settings_rows(r);
    for (int i = 0; i < int(std::size(settings_sections)); ++i) {
        Rect tab = settings_category_bounds(r, i);
        focus_target(tab);
        const bool selected = settings_search_.value.empty() && i == settings_section_;
        if (selected || inside(tab, hover_x_, hover_y_))
            fill(c, tab, 9, SkColorSetA(t.accent, selected ? 30 : 12));
        label(c, selected ? medium_ : regular_, app_.tr(settings_sections[i]), tab.x + 13,
              tab.y + tab.height / 2 + 4, 12, selected ? t.accent : muted, tab.width - 22);
    }
    if (r.height >= 560)
        label(c, regular_, std::to_string(settings_catalog().size()) + " " + app_.tr("preferences"),
              r.x + 31, r.y + r.height - 68, 10, muted);
    settings_scroll_ = std::clamp(settings_scroll_, 0, settings_max_scroll(r));
    c.save();
    c.clipRect(SkRect::MakeLTRB(r.x + 183, r.y + 122, r.x + r.width - 24, r.y + r.height - 54));
    auto chip = [&](Rect b, const std::string &text, bool selected) {
        if (b.y >= r.y + 122 && b.y + b.height <= r.y + r.height - 54)
            focus_target(b);
        const bool hover = inside(b, hover_x_, hover_y_);
        fill(c, b, 9, SkColorSetA(t.accent, selected ? 35 : hover ? 24 : 9));
        border(c, b, 9, SkColorSetA(selected ? t.accent : muted, selected ? 255 : 190));
        if (selected)
            icon(c, "check", b.x + 8, b.y + (b.height - 12) / 2.f, 12, t.accent);
        centered_label(c, medium_, text, b, 12, selected ? t.accent : ink, selected ? 28 : 8,
                       selected ? 28 : 8);
    };
    auto description = [&](const std::string &text, Rect b, int reserved) {
        const auto value = app_.tr(text);
        const auto f = font(regular_, 12);
        const int w = std::max(90, b.width - reserved);
        if (measure(f, value) <= w)
            label(c, regular_, value, b.x, b.y + 41, 12, muted, w);
        else {
            std::size_t split = value.find(' '), best = 0;
            while (split != std::string::npos && measure(f, value.substr(0, split)) < w) {
                best = split;
                split = value.find(' ', split + 1);
            }
            if (!best)
                label(c, regular_, value, b.x, b.y + 41, 12, muted, w);
            else {
                label(c, regular_, value.substr(0, best), b.x, b.y + 41, 12, muted, w);
                label(c, regular_, value.substr(best + 1), b.x, b.y + 56, 12, muted, w);
            }
        }
    };
    if (rows.empty()) {
        label(c, medium_, app_.tr("No settings found"), r.x + 188, r.y + 161, 16, ink);
        label(c, regular_, app_.tr("Try another word or choose a category."), r.x + 188, r.y + 188,
              12, muted, r.width - 216);
    }
    for (auto row : rows) {
        auto b = row.bounds;
        b.y -= settings_scroll_;
        if (b.y + b.height < r.y + 122 || b.y > r.y + r.height - 54)
            continue;
        const auto spec = row.spec;
        const bool toggle = spec && spec->kind == SettingKind::Toggle;
        const bool number = spec && spec->kind == SettingKind::Number;
        const int reserve = toggle ? 65 : number ? 152 : 0;
        if (toggle && inside(b, hover_x_, hover_y_))
            fill(c, {b.x - 4, b.y, b.width + 8, b.height - 12}, 10, SkColorSetA(t.accent, 9));
        auto title = app_.tr(row.title);
        if (spec && spec->id == "ui_zoom")
            title += " · " + app_.tr("Applied") + " " +
                     std::to_string(int(std::lround(app_.ui_scale * 100))) + "%";
        label(c, medium_, title, b.x, b.y + 20, 14, ink, b.width - reserve);
        description(row.description, b, reserve);
        if (!spec) {
            chip({b.x, b.y + 66, std::min(300, b.width), 32}, app_.tr(row.title), false);
        } else if (toggle) {
            const bool value = setting_value(app_.profile, *spec) == "true";
            if (b.y >= r.y + 122 && b.y + 54 <= r.y + r.height - 54)
                focus_target({b.x, b.y, b.width, 60});
            fill(c, {b.x + b.width - 45, b.y + 13, 45, 25}, 13,
                 value ? t.accent : SkColorSetA(ink, 35));
            border(c, {b.x + b.width - 45, b.y + 13, 45, 25}, 13,
                   value ? t.accent : SkColorSetA(muted, 190));
            fill(c, {b.x + b.width - (value ? 23 : 41), b.y + 17, 17, 17}, 9, t.background);
        } else if (spec->kind == SettingKind::Choice) {
            const auto value = setting_value(app_.profile, *spec);
            for (int i = 0; i < static_cast<int>(spec->choices.size()); ++i)
                chip(setting_choice(*spec, b, i), app_.tr(spec->choices[i].label),
                     value == spec->choices[i].value);
        } else if (number) {
            Rect n{b.x + b.width - 140, b.y + 13, 140, 38};
            if (n.y >= r.y + 122 && n.y + n.height <= r.y + r.height - 54) {
                focus_target({n.x, n.y, 36, n.height});
                focus_target({n.x + n.width - 36, n.y, 36, n.height});
            }
            fill(c, n, 10, SkColorSetA(t.accent, 12));
            centered_label(c, medium_, "−", {n.x, n.y, 36, n.height}, 17, muted, 0, 0);
            centered_label(c, medium_, "+", {n.x + n.width - 36, n.y, 36, n.height}, 17, muted, 0,
                           0);
            const auto value = setting_value(app_.profile, *spec);
            centered_label(c, medium_, value, {n.x + 36, n.y, n.width - 72, n.height}, 13, t.accent,
                           0, 0);
        } else if (spec->kind == SettingKind::Text) {
            Rect field{b.x, b.y + 66, b.width - (spec->id == "ntp_image_path" ? 212 : 106), 34};
            if (field.y >= r.y + 122 && field.y + field.height <= r.y + r.height - 54)
                focus_target(field);
            fill(c, field, 9, SkColorSetA(ink, 7));
            border(c, field, 9,
                   SkColorSetA(settings_edit_ == spec->id ? t.accent : muted,
                               settings_edit_ == spec->id ? 255 : 190));
            if (settings_edit_ == spec->id)
                draw_editor(c, settings_text_, {field.x + 10, field.y + 1, field.width - 20, 32},
                            12, true);
            else
                label(c, regular_, setting_value(app_.profile, *spec), field.x + 10, field.y + 23,
                      12, muted, field.width - 20);
            chip({b.x + b.width - 98, b.y + 66, 98, 34}, app_.tr("Save"), false);
            if (spec->id == "ntp_image_path")
                chip({b.x + b.width - 204, b.y + 66, 98, 34},
                     app_.tr(picker_pid_ > 0 ? "Choosing..." : "Browse..."), false);
            if (settings_error_ == spec->id)
                label(c, medium_,
                      app_.tr(spec->id == "ntp_image_path" ? wallpaper_error_
                              : (spec->id == "home_page" || spec->id.starts_with("ntp_link_"))
                                  ? "Enter a valid http:// or https:// address"
                              : (spec->id == "ntp_title" || spec->id == "ntp_subtitle")
                                  ? "Use a shorter heading or introduction."
                                  : "Enter an absolute folder path"),
                      b.x, b.y + 123, 12, ink, b.width);
        } else if (spec->kind == SettingKind::Themes) {
            const char *names[] = {"Aurora",      "Aurora Light", "Ember",
                                   "Ember Light", "Monolith",     "Monolith Light"};
            for (int i = 0; i < 6; ++i) {
                Rect tile{b.x + (i % 3) * ((b.width - 16) / 3 + 8), b.y + 66 + (i / 3) * 84,
                          (b.width - 16) / 3, 74};
                if (tile.y >= r.y + 122 && tile.y + tile.height <= r.y + r.height - 54)
                    focus_target(tile);
                const auto &theme = design::themes[i];
                fill(c, tile, 12, theme.background);
                c.save();
                c.clipRRect(SkRRect::MakeRectXY(box(tile), 12, 12), true);
                glow(c, {static_cast<float>(tile.x), static_cast<float>(tile.y)}, 180,
                     theme.mesh[0]);
                c.restore();
                border(c, tile, 12, i == app_.profile.theme ? t.accent : SkColorSetA(ink, 35));
                label(c, medium_, app_.tr(names[i]), tile.x + 12, tile.y + 53, 11, theme.text,
                      tile.width - 20);
                if (i == app_.profile.theme)
                    icon(c, "check", tile.x + tile.width - 25, tile.y + 10, 14, theme.accent);
            }
        }
        fill(c, {b.x, b.y + b.height - 8, b.width, 1}, 0, SkColorSetA(ink, 10));
    }
    c.restore();
    fill(c, {r.x + 182, r.y + 118, r.width - 206, 6}, 0, t.surface);
    fill(c, {r.x + 182, r.y + r.height - 58, r.width - 206, 6}, 0, t.surface);
    if (const int maximum = settings_max_scroll(r); maximum > 0) {
        const int track = r.height - 182, thumb = std::max(25, track * track / (track + maximum));
        fill(c,
             {r.x + r.width - 13, r.y + 128 + settings_scroll_ * (track - thumb) / maximum, 5,
              thumb},
             3, SkColorSetA(muted, 160));
    }
    fill(c, {r.x + 28, r.y + r.height - 45, r.width - 56, 1}, 0, SkColorSetA(ink, 15));
    label(c, regular_, app_.tr("Saved automatically"), r.x + 28, r.y + r.height - 24, 10, muted,
          r.width - 165);
#ifdef _WIN32
    label(c, regular_, "Aura · Windows",
#else
    label(c, regular_, "Aura · Linux",
#endif
          r.x + r.width - 103, r.y + r.height - 24, 10, muted);
}
void BrowserUi::save_setting_text() {
    if (const auto spec = find_setting(settings_edit_)) {
        if (spec->id == "ntp_image_path") {
            if (!import_wallpaper(settings_text_.value)) {
                settings_error_ = spec->id;
                app_.changed();
                return;
            }
            settings_error_.clear();
            settings_edit_.clear();
            app_.changed(true);
            return;
        }
        if (!set_setting(app_.profile, *spec, settings_text_.value)) {
            settings_error_ = spec->id;
            toast(app_.tr((spec->id == "home_page" || spec->id.starts_with("ntp_link_"))
                              ? "Enter a valid http:// or https:// address"
                          : (spec->id == "ntp_title" || spec->id == "ntp_subtitle")
                              ? "Use a shorter heading or introduction."
                              : "Enter an absolute folder path"));
            return;
        }
        settings_error_.clear();
        settings_edit_.clear();
        app_.apply_settings();
    }
}
void BrowserUi::settings_pointer(Rect r, int x, int y) {
    if (inside({r.x + 28, r.y + 66, r.width - 56, 38}, x, y)) {
        if (x >= r.x + r.width - 66) {
            settings_search_.set("");
            settings_scroll_ = 0;
        }
        settings_search_focus_ = true;
        settings_edit_.clear();
        app_.window->caret(r.x + 80, r.y + 104);
        app_.changed();
        return;
    }
    settings_search_focus_ = false;
    for (int i = 0; i < int(std::size(settings_sections)); ++i)
        if (inside(settings_category_bounds(r, i), x, y)) {
            settings_section_ = i;
            settings_scroll_ = 0;
            settings_search_.set("");
            settings_edit_.clear();
            app_.changed();
            return;
        }
    if (y < r.y + 122 || y >= r.y + r.height - 54)
        return;
    if (x >= r.x + r.width - 20) {
        const int maximum = settings_max_scroll(r);
        settings_scroll_ =
            std::clamp((y - r.y - 128) * maximum / std::max(1, r.height - 182), 0, maximum);
        app_.changed();
        return;
    }
    for (const auto &row : settings_rows(r)) {
        auto b = row.bounds;
        b.y -= settings_scroll_;
        if (!inside(b, x, y))
            continue;
        const auto spec = row.spec;
        if (!spec) {
            if (inside({b.x, b.y + 66, std::min(300, b.width), 32}, x, y)) {
                if (row.action == "shortcuts")
                    open_shortcuts();
                else if (row.action == "updates")
                    panel(Updates);
                else if (row.action == "import")
                    panel(Import);
                else if (row.action == "passwords")
                    panel(Passwords);
                else if (row.action == "wallpaper_clear")
                    import_wallpaper("");
                else if (row.action == "extensions")
                    panel(Extensions);
                else if (row.action == "filters")
                    panel(Filters);
                else if (row.action == "folder")
                    app_.open_folder();
                else if (row.action == "downloads")
                    app_.clear_data(row.action);
                else
                    app_.confirm_action(row.title, row.description,
                                        [this, kind = row.action] { app_.clear_data(kind); });
            }
            return;
        }
        bool update = false;
        if (spec->kind == SettingKind::Toggle) {
            update = set_setting(app_.profile, *spec,
                                 setting_value(app_.profile, *spec) == "true" ? "false" : "true");
        } else if (spec->kind == SettingKind::Choice) {
            for (int i = 0; i < static_cast<int>(spec->choices.size()); ++i)
                if (inside(setting_choice(*spec, b, i), x, y))
                    update = set_setting(app_.profile, *spec, spec->choices[i].value);
        } else if (spec->kind == SettingKind::Number &&
                   inside({b.x + b.width - 140, b.y + 13, 140, 38}, x, y)) {
            const int value = std::stoi(setting_value(app_.profile, *spec));
            if (x < b.x + b.width - 100)
                update = set_setting(app_.profile, *spec,
                                     std::to_string(std::max(spec->minimum, value - spec->step)));
            else if (x > b.x + b.width - 40)
                update = set_setting(app_.profile, *spec,
                                     std::to_string(std::min(spec->maximum, value + spec->step)));
        } else if (spec->kind == SettingKind::Text) {
            if (spec->id == "ntp_image_path" &&
                inside({b.x + b.width - 204, b.y + 66, 98, 34}, x, y)) {
                choose_wallpaper();
                return;
            }
            if (inside({b.x, b.y + 66, b.width - (spec->id == "ntp_image_path" ? 212 : 106), 34}, x,
                       y)) {
                settings_edit_ = spec->id;
                settings_text_.set(setting_value(app_.profile, *spec), true);
                app_.window->caret(b.x + 12, b.y + 100);
                app_.changed();
            } else if (inside({b.x + b.width - 98, b.y + 66, 98, 34}, x, y) &&
                       settings_edit_ == spec->id)
                save_setting_text();
            return;
        } else if (spec->kind == SettingKind::Themes) {
            for (int i = 0; i < 6; ++i)
                if (inside({b.x + (i % 3) * ((b.width - 16) / 3 + 8), b.y + 66 + (i / 3) * 84,
                            (b.width - 16) / 3, 74},
                           x, y))
                    update = set_setting(app_.profile, *spec, std::to_string(i));
        }
        if (update) {
            if (spec->id == "language")
                toast_.clear();
            app_.apply_settings(spec->id == "default_zoom");
        }
        settings_edit_.clear();
        return;
    }
    settings_edit_.clear();
}
std::string BrowserUi::settings_report() const {
    if (panel_ != Settings)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out =
        "{\"search\":\"" + json_escape(settings_search_.value) + "\",\"validation_error\":\"" +
        json_escape(settings_error_) + "\",\"scroll\":" + std::to_string(settings_scroll_) +
        ",\"search_bounds\":" + rect({r.x + 28, r.y + 66, r.width - 56, 38}) + ",\"categories\":[";
    for (int i = 0; i < int(std::size(settings_sections)); ++i) {
        if (i)
            out += ',';
        out += "{\"name\":\"" + std::string(settings_sections[i]) +
               "\",\"bounds\":" + rect(settings_category_bounds(r, i)) + "}";
    }
    out += "],\"controls\":[";
    bool first = true;
    for (auto row : settings_rows(r)) {
        auto b = row.bounds;
        b.y -= settings_scroll_;
        if (!first)
            out += ',';
        first = false;
        out += "{\"id\":\"" + (row.spec ? row.spec->id : row.action) + "\",\"bounds\":" + rect(b);
        if (row.spec && row.spec->id == "ntp_image_path")
            out += ",\"browse\":" + rect({b.x + b.width - 204, b.y + 66, 98, 34});
        if (row.spec && row.spec->kind == SettingKind::Choice) {
            out += ",\"options\":[";
            for (int i = 0; i < static_cast<int>(row.spec->choices.size()); ++i) {
                if (i)
                    out += ',';
                out += "{\"value\":\"" + row.spec->choices[i].value +
                       "\",\"bounds\":" + rect(setting_choice(*row.spec, b, i)) + "}";
            }
            out += ']';
        }
        out += '}';
    }
    return out + "]}";
}

std::vector<Visit> BrowserUi::suggestions() const {
    return app_.profile.suggest(
        address.value, std::min(app_.profile.suggestion_limit, std::max(1, (height_ - 205) / 54)));
}
void BrowserUi::focus_address(bool select) {
    clear_password_fields();
    app_.close_extension_popup();
    editing = true;
    panel_ = None;
    address.set(app_.current() && !app_.current()->ntp ? app_.current()->page.url : "", select);
    suggestion_ = -1;
    app_.close_menu();
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.window->caret(address_bounds().x + 60, 135);
    app_.changed();
}
void BrowserUi::toast(std::string message) {
    toast_ = std::move(message);
    toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    app_.changed();
}
void BrowserUi::panel(Panel value) {
    app_.close_extension_popup();
    popup_buttons_ = 0;
    keyboard_focus_ = -1;
    clear_password_fields();
    password_mode_ = 0;
    panel_ = value;
    if (value == Extensions)
        app_.refresh_extension_state();
    shortcut_focus_ = -1;
    panel_scroll_ = 0;
    if (value != None) {
        editing = false;
        find_open = false;
    }
    settings_search_focus_ = false;
    settings_edit_.clear();
    import_input_focus_ = false;
    filter_input_focus_ = false;
    if (value == Filters)
        sync_filter_lines();
    app_.changed();
}
void BrowserUi::commit_address() {
    auto list = suggestions();
    auto value = suggestion_ >= 0 && suggestion_ < static_cast<int>(list.size())
                     ? list[suggestion_].url
                     : address.value;
    app_.navigate(value);
}
void BrowserUi::dismiss() {
    clear_password_fields();
    editing = false;
    panel_ = None;
    app_.close_menu();
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(true);
    app_.changed();
}
bool BrowserUi::editor_key(TextEditor &e, const CefKeyEvent &key) {
    const bool ctrl = key.modifiers & EVENTFLAG_CONTROL_DOWN,
               shift = key.modifiers & EVENTFLAG_SHIFT_DOWN;
    const auto code = key.windows_key_code;
    if (ctrl && code == 'A')
        e.select_all();
    else if (ctrl && (code == 'C' || code == 'X')) {
        bool secret =
            panel_ == Passwords && (&e == &password_fields_[0] || &e == &password_fields_[1] ||
                                    &e == &password_fields_[2]);
        if (secret)
            return true;
        app_.window->copy(e.selected());
        if (code == 'X')
            e.insert("");
    } else if (ctrl && code == 'V') {
        auto *target = &e;
        app_.window->paste([this, target](std::string text) {
            text.erase(
                std::remove_if(text.begin(), text.end(), [](unsigned char c) { return c < 32; }),
                text.end());
            TextEditor *live = nullptr;
            if (target == &address && editing)
                live = &address;
            else if (target == &find && find_open)
                live = &find;
            else if (panel_ == Settings && target == &settings_search_ && settings_search_focus_)
                live = &settings_search_;
            else if (panel_ == Settings && target == &settings_text_ && !settings_edit_.empty())
                live = &settings_text_;
            else if (panel_ == Shortcuts && target == &shortcut_title_ && shortcut_focus_ == 0)
                live = target;
            else if (panel_ == Shortcuts && target == &shortcut_url_ && shortcut_focus_ == 1)
                live = target;
            else if (panel_ == Filters && target == &filter_input_ && filter_input_focus_ &&
                     !app_.filter_lists().busy())
                live = &filter_input_;
            else if (panel_ == Extensions && target == &extension_input_ && extension_input_focus_)
                live = &extension_input_;
            else if (panel_ == Import && target == &import_input_ && import_input_focus_ &&
                     !app_.importer.busy)
                live = &import_input_;
            else if (panel_ == Passwords && password_focus_ >= 0 && !app_.passwords.busy &&
                     target == &password_fields_[password_focus_])
                live = target;
            else if (!app_.dialogs.empty() && target == &app_.dialogs.front().input)
                live = target;
            if (live && !text.empty() &&
                (panel_ != Passwords || live->value.size() + text.size() <= 8192) &&
                (live != &filter_input_ || live->value.size() + text.size() <= 4096))
                live->insert(text);
            if (live == &find)
                app_.find_text(find.value);
            if (live == &settings_search_)
                settings_scroll_ = 0;
            if (live == &import_input_) {
                app_.importer.candidate.reset();
                app_.importer.result.reset();
            }
            erase_secret(text);
            app_.changed();
        });
    } else if (code == 0x08)
        e.erase(true);
    else if (code == 0x2E)
        e.erase(false);
    else if (code == 0x25)
        e.move(-1, shift, ctrl);
    else if (code == 0x27)
        e.move(1, shift, ctrl);
    else if (code == 0x24)
        e.edge(false, shift);
    else if (code == 0x23)
        e.edge(true, shift);
    else
        return false;
    return true;
}
bool BrowserUi::key(const CefKeyEvent &key) {
    const bool down = key.type != KEYEVENT_KEYUP;
    const auto code = key.windows_key_code;
    const bool ctrl = key.modifiers & EVENTFLAG_CONTROL_DOWN,
               shift = key.modifiers & EVENTFLAG_SHIFT_DOWN,
               alt = key.modifiers & EVENTFLAG_ALT_DOWN;
    if (ctrl && shift && code == '0') {
        if (down) {
            app_.profile.ui_zoom = 100;
            app_.apply_settings();
        }
        return true;
    }
    if (down)
        last_key_ = key;
    const bool native_focus = panel_ != None || !app_.dialogs.empty() || app_.menu_action ||
                              app_.menu_callback || keyboard_focus_ >= 0 ||
                              (app_.current() && app_.current()->ntp);
    if (!app_.extension_popup && down && !ctrl && !alt &&
        (code == 0x79 || (code == 0x09 && native_focus && !editing))) {
        if (!settings_edit_.empty()) {
            save_setting_text();
            if (!settings_edit_.empty())
                return true;
        }
        settings_search_focus_ = false;
        extension_input_focus_ = false;
        import_input_focus_ = false;
        shortcut_focus_ = -1;
        password_focus_ = -1;
        filter_input_focus_ = false;
        if (auto tab = app_.current(); tab && tab->browser)
            tab->browser->GetHost()->SetFocus(false);
        if (!focus_targets_.empty()) {
            keyboard_focus_ =
                keyboard_focus_ < 0
                    ? (shift ? int(focus_targets_.size()) - 1 : 0)
                    : (keyboard_focus_ + (shift ? int(focus_targets_.size()) - 1 : 1)) %
                          int(focus_targets_.size());
        }
        app_.changed();
        return true;
    }
    if (!app_.extension_popup && down && keyboard_focus_ >= 0 &&
        keyboard_focus_ < int(focus_targets_.size()) && (code == 0x0D || code == 0x20) &&
        !settings_search_focus_ && settings_edit_.empty() && !extension_input_focus_ &&
        !import_input_focus_ && !filter_input_focus_ && shortcut_focus_ < 0 &&
        password_focus_ < 0 && !editing) {
        const auto r = focus_targets_[keyboard_focus_];
        const int previous = keyboard_focus_;
        const auto previous_panel = panel_;
        PointerInput click{};
        click.kind = PointerInput::Down;
        click.point.x = r.x + r.width / 2;
        click.point.y = r.y + r.height / 2;
        pointer(click);
        click.kind = PointerInput::Up;
        pointer(click);
        if (panel_ != None && panel_ == previous_panel)
            keyboard_focus_ = previous;
        return true;
    }
    if (auto p = app_.extension_popup;
        p && app_.dialogs.empty() && !app_.menu_callback && !app_.menu_action) {
        if (down && code == 0x1B) {
            app_.close_extension_popup();
            return true;
        }
        const bool browser_shortcut =
            (ctrl && (code == 'L' || code == 'T' || code == 'W' || code == 'Q' || code == 'R' ||
                      code == 'F' || code == 'J' || code == 'H' || code == 'B' || code == 'D' ||
                      code == 'P' || code == 0x09 || (code >= '1' && code <= '9'))) ||
            (alt && (code == 0x25 || code == 0x27 || code == 0x24)) || code == 0x74 || code == 0x7B;
        if (down && browser_shortcut)
            app_.close_extension_popup();
        else {
            if (p->browser)
                p->browser->GetHost()->SendKeyEvent(key);
            return false;
        }
    }
    if (down) {
        last_key_ = key;
        if (!app_.dialogs.empty()) {
            if (code == 0x0D)
                app_.answer_dialog(true);
            else if (code == 0x1B)
                app_.answer_dialog(false);
            else if (app_.dialogs.front().prompt) {
                editor_key(app_.dialogs.front().input, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
            return true;
        }
        if (panel_ == Shortcuts && shortcut_focus_ >= 0) {
            if (code == 0x1B) {
                shortcut_focus_ = -1;
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                save_shortcut();
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(shortcut_focus_ == 0 ? shortcut_title_ : shortcut_url_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Passwords && password_focus_ >= 0) {
            if (code == 0x1B || code == 0x0D) {
                password_focus_ = -1;
                app_.changed();
                return true;
            }
            if (!app_.passwords.busy) {
                editor_key(password_fields_[password_focus_], key);
                app_.passwords.touch();
                app_.changed();
            }
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
        if (panel_ == Import && import_input_focus_) {
            if (code == 0x1B) {
                import_input_focus_ = false;
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                app_.importer.preview(import_input_.value);
                import_input_focus_ = false;
                app_.changed();
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                const auto before = import_input_.value;
                editor_key(import_input_, key);
                if (before != import_input_.value) {
                    app_.importer.candidate.reset();
                    app_.importer.result.reset();
                }
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Filters && filter_input_focus_) {
            if (code == 0x1B) {
                filter_input_focus_ = false;
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                save_filter_rule();
                app_.changed();
                return true;
            }
            if (!ctrl ||
                (code != 'L' && code != 'T' && code != 'W' && code != 'Q' && code != 'N')) {
                if (!app_.filter_lists().busy())
                    editor_key(filter_input_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Extensions && extension_input_focus_) {
            if (code == 0x1B) {
                extension_input_focus_ = false;
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                app_.extensions.fetch(extension_input_.value);
                panel_scroll_ = 0;
                app_.changed();
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(extension_input_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Settings && ctrl && code == 'F') {
            settings_search_focus_ = true;
            settings_edit_.clear();
            settings_search_.select_all();
            app_.changed();
            return true;
        }
        if (panel_ == Settings && (settings_search_focus_ || !settings_edit_.empty())) {
            if (code == 0x1B) {
                settings_search_focus_ = false;
                settings_edit_.clear();
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                save_setting_text();
                settings_search_focus_ = false;
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(settings_search_focus_ ? settings_search_ : settings_text_, key);
                if (settings_search_focus_)
                    settings_scroll_ = 0;
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (alt && code == 0x24) {
            app_.navigate(app_.profile.home_page);
            return true;
        }
        if (ctrl && code == 'L') {
            focus_address();
            return true;
        }
        if (ctrl && code == 'T') {
            if (shift)
                app_.reopen();
            else
                app_.open_new_tab(app_.current() && app_.current()->private_tab);
            return true;
        }
        if (ctrl && code == 'N') {
            if (alt)
                app_.open_new_tab(true);
            else
                app_.new_window(shift);
            return true;
        }
        if (ctrl && code == 'W') {
            if (shift)
                app_.request_quit();
            else
                app_.close_tab(app_.active);
            return true;
        }
        if (ctrl && code == 'Q') {
            app_.request_exit();
            return true;
        }
        if (ctrl && code == 0x09) {
            if (!app_.tabs.empty())
                app_.activate((app_.active + (shift ? static_cast<int>(app_.tabs.size()) - 1 : 1)) %
                              app_.tabs.size());
            return true;
        }
        if (ctrl && code >= '1' && code <= '9') {
            app_.activate(code == '9' ? static_cast<int>(app_.tabs.size()) - 1 : code - '1');
            return true;
        }
        if (alt && (code == 0x25 || code == 0x27)) {
            if (code == 0x25)
                app_.back();
            else
                app_.forward();
            return true;
        }
        if ((ctrl && code == 'R') || code == 0x74) {
            app_.reload(shift);
            return true;
        }
        if (ctrl && (code == 0xBB || code == 0x6B || code == 0xBD || code == 0x6D || code == '0')) {
            if (app_.current() && app_.current()->ntp && panel_ == None && !editing)
                zoom_start_page(code == 0xBD || code == 0x6D ? -1 : 1, code == '0');
            else
                app_.zoom(code == 0xBD || code == 0x6D ? -1 : 1, code == '0');
            return true;
        }
        if (ctrl && code == 'D') {
            app_.bookmark();
            return true;
        }
        if (ctrl && code == 'H') {
            panel(History);
            return true;
        }
        if (ctrl && code == 'B') {
            panel(Bookmarks);
            return true;
        }
        if (ctrl && code == 'J') {
            panel(Downloads);
            return true;
        }
        if (ctrl && shift && code == 'Y') {
            panel(Passwords);
            return true;
        }
        if (ctrl && code == 'P') {
            if (auto t = app_.current(); t && t->browser)
                t->browser->GetHost()->Print();
            return true;
        }
        if (code == 0x7B) {
            app_.devtools();
            return true;
        }
        if (code == 0x75 || code == 0x76) {
            if (code == 0x75)
                app_.profile.theme = (app_.profile.theme + 2) % 6;
            else
                app_.profile.theme ^= 1;
            app_.changed(true);
            return true;
        }
        if (ctrl && code == 'F') {
            find_open = true;
            editing = false;
            find.select_all();
            app_.changed();
            return true;
        }
        if (code == 0x72) {
            app_.find_text(find.value, true, shift);
            return true;
        }
        if (code == 0x1B) {
            if (keyboard_focus_ >= 0 && panel_ == None && !editing && !find_open) {
                keyboard_focus_ = -1;
                if (auto tab = app_.current(); tab && tab->browser)
                    tab->browser->GetHost()->SetFocus(true);
                app_.changed();
                return true;
            }
            keyboard_focus_ = -1;
            if (find_open) {
                find_open = false;
                if (auto t = app_.current(); t && t->browser)
                    t->browser->GetHost()->StopFinding(true);
            } else if (editing || panel_ != None || app_.menu_callback || app_.menu_action)
                dismiss();
            else if (auto t = app_.current(); t && t->browser)
                t->browser->StopLoad();
            app_.changed();
            return true;
        }
        if (editing) {
            if (code == 0x0D) {
                commit_address();
                return true;
            }
            if (code == 0x26 || code == 0x28) {
                auto list = suggestions();
                suggestion_ = std::clamp(suggestion_ + (code == 0x28 ? 1 : -1), -1,
                                         static_cast<int>(list.size()) - 1);
            } else {
                editor_key(address, key);
                suggestion_ = -1;
            }
            app_.changed();
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
        if (find_open) {
            if (code == 0x0D)
                app_.find_text(find.value, true, shift);
            else {
                editor_key(find, key);
                app_.find_text(find.value);
            }
            app_.changed();
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
        if (app_.current() && app_.current()->ntp && panel_ == None && !editing && !find_open &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const auto l = start_page_layout();
            start_scroll_ =
                code == 0x24 ? 0
                : code == 0x23
                    ? l.maximum
                    : std::clamp(start_scroll_ + (code == 0x22 ? 1 : -1) * (l.viewport.height - 32),
                                 0, l.maximum);
            keyboard_focus_ = -1;
            app_.changed();
            return true;
        }
        if ((panel_ == History || panel_ == Bookmarks || panel_ == Downloads ||
             panel_ == Extensions || panel_ == Shortcuts || panel_ == Filters) &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int count = panel_ == History     ? int(app_.profile.history.size())
                              : panel_ == Bookmarks ? int(app_.profile.bookmarks.size())
                              : panel_ == Downloads ? int(app_.downloads.size())
                              : panel_ == Shortcuts ? int(app_.profile.shortcuts.size())
                              : panel_ == Filters   ? int(filter_lines_.size())
                              : app_.extensions.candidate
                                  ? int(app_.extensions.candidate->extension.permissions.size())
                                  : int(app_.extensions.items.size());
            panel_scroll_ = code == 0x24   ? 0
                            : code == 0x23 ? std::max(0, count - 1)
                                           : std::clamp(panel_scroll_ + (code == 0x22 ? 3 : -3), 0,
                                                        std::max(0, count - 1));
            keyboard_focus_ = -1;
            app_.changed();
            return true;
        }
        if (panel_ == Settings && (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            keyboard_focus_ = -1;
            const int maximum = settings_max_scroll(panel_bounds());
            if (code == 0x24 || code == 0x23)
                settings_scroll_ = code == 0x24 ? 0 : maximum;
            else
                settings_scroll_ =
                    std::clamp(settings_scroll_ + (code == 0x22 ? 180 : -180), 0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ != None)
            return true;
    }
    if (keyboard_focus_ >= 0 && !editing && !find_open && panel_ == None && app_.dialogs.empty())
        return true;
    if (editing || find_open || !app_.dialogs.empty() || panel_ != None)
        return false;
    if (auto t = app_.current(); t && t->browser && !t->ntp)
        t->browser->GetHost()->SendKeyEvent(key);
    return false;
}
void BrowserUi::text(const std::string &value) {
    if (auto p = app_.extension_popup; p && p->browser && app_.dialogs.empty()) {
        for (auto ch : CefString(value).ToString16()) {
            auto event = last_key_;
            event.type = KEYEVENT_CHAR;
#ifdef _WIN32
            // CEF's Windows backend maps this field to WM_CHAR's Unicode wParam.
            event.windows_key_code = ch;
#endif
            event.character = ch;
            event.unmodified_character = ch;
            p->browser->GetHost()->SendKeyEvent(event);
        }
        return;
    }
    if (app_.dialogs.empty() && !editing && !find_open && panel_ == None && app_.current() &&
        app_.current()->ntp) {
        std::string clean = value;
        clean.erase(
            std::remove_if(clean.begin(), clean.end(), [](unsigned char c) { return c < 32; }),
            clean.end());
        if (!clean.empty()) {
            focus_address(false);
            address.insert(clean);
            app_.changed();
        }
        return;
    }
    if (app_.dialogs.empty() && !editing && !find_open && panel_ == None) {
        if (auto t = app_.current(); t && t->browser && !t->ntp) {
            for (auto ch : CefString(value).ToString16()) {
                CefKeyEvent event = last_key_;
                event.type = KEYEVENT_CHAR;
#ifdef _WIN32
                // CEF's Windows backend maps this field to WM_CHAR's Unicode wParam.
                event.windows_key_code = ch;
#endif
                event.character = ch;
                event.unmodified_character = ch;
                t->browser->GetHost()->SendKeyEvent(event);
            }
        }
        return;
    }
    std::string clean = value;
    clean.erase(std::remove_if(clean.begin(), clean.end(), [](unsigned char c) { return c < 32; }),
                clean.end());
    if (clean.empty())
        return;
    if (!app_.dialogs.empty()) {
        if (app_.dialogs.front().prompt)
            app_.dialogs.front().input.insert(clean);
    } else if (panel_ == Shortcuts) {
        if (shortcut_focus_ >= 0)
            (shortcut_focus_ == 0 ? shortcut_title_ : shortcut_url_).insert(clean);
    } else if (panel_ == Passwords) {
        if (password_focus_ >= 0 && !app_.passwords.busy &&
            password_fields_[password_focus_].value.size() + clean.size() <= 8192) {
            password_fields_[password_focus_].insert(clean);
            app_.passwords.touch();
        }
        erase_secret(clean);
    } else if (panel_ == Filters) {
        if (filter_input_focus_ && !app_.filter_lists().busy() &&
            filter_input_.value.size() + clean.size() <= 4096)
            filter_input_.insert(clean);
    } else if (panel_ == Import) {
        if (import_input_focus_ && !app_.importer.busy) {
            import_input_.insert(clean);
            app_.importer.candidate.reset();
            app_.importer.result.reset();
        }
    } else if (panel_ == Extensions) {
        if (extension_input_focus_)
            extension_input_.insert(clean);
    } else if (panel_ == Settings) {
        if (settings_search_focus_) {
            settings_search_.insert(clean);
            settings_scroll_ = 0;
        } else if (!settings_edit_.empty())
            settings_text_.insert(clean);
    } else if (editing) {
        address.insert(clean);
        suggestion_ = -1;
    } else if (find_open) {
        find.insert(clean);
        app_.find_text(find.value);
    }
    app_.changed();
}
bool BrowserUi::tick() {
    poll_wallpaper_picker();
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(.1, std::chrono::duration<double>(now - last_tick_).count());
    last_tick_ = now;
    bool animate = false;
    float target = editing ? 1 : 0;
    if (app_.profile.reduce_motion)
        focus_animation_ = target;
    if (filter_pending_ && !app_.filter_lists().busy()) {
        if (app_.filter_lists().revision > filter_submit_revision_ &&
            filter_input_.value == filter_submitted_) {
            filter_input_.set("");
            filter_edit_ = -1;
        }
        filter_pending_ = false;
        animate = true;
    }
    if (std::abs(focus_animation_ - target) > .005f) {
        focus_animation_ += (target - focus_animation_) * std::min(1.f, dt * 18);
        animate = true;
    }
    if (app_.profile.ntp_clock && app_.current() && app_.current()->ntp) {
        static std::time_t last_minute = 0;
        const auto minute = std::time(nullptr) / 60;
        if (minute != last_minute) {
            last_minute = minute;
            animate = true;
        }
    }
    if (auto t = app_.current(); t && t->loading) {
        if (app_.profile.reduce_motion)
            progress_ = std::max(.12, t->progress);
        else {
            progress_ += (std::max(.12, t->progress) - progress_) * std::min(1.f, dt * 10);
            animate = true;
        }
    } else
        progress_ = 0;
    if (!toast_.empty() && now >= toast_until_) {
        toast_.clear();
        animate = true;
    }
    return animate;
}
void BrowserUi::pointer(const PointerInput &input) {
    const int x = input.point.x, y = input.point.y;
    const auto web = web_bounds();
    auto t = app_.current();
    const bool web_only = t && !t->ntp && inside(web, x, y) && inside(web, hover_x_, hover_y_) &&
                          panel_ == None && !editing && !find_open && drag_tab_ < 0 &&
                          keyboard_focus_ < 0 && !app_.extension_popup && app_.dialogs.empty() &&
                          !app_.menu_callback && !app_.menu_action;
    hover_x_ = x;
    hover_y_ = y;
    // CEF owns hover and wheel changes inside a page. Its paint callback will
    // invalidate content; the shell does not need a full redraw per input event.
    if (!web_only || (input.kind != PointerInput::Wheel && input.kind != PointerInput::Move))
        app_.changed();
    const bool down = input.kind == PointerInput::Down;
    if (down || input.kind == PointerInput::Wheel)
        keyboard_focus_ = -1;
    if (input.kind == PointerInput::Up)
        drag_tab_ = -1;
    if (app_.extension_popup && app_.dialogs.empty() && !app_.menu_callback && !app_.menu_action &&
        popup_pointer(input))
        return;
    if (input.kind == PointerInput::Move && drag_tab_ >= 0 && std::abs(y - drag_y_) > 18) {
        const int index =
            std::clamp((y - 178) / 52 + tab_scroll_, 0, static_cast<int>(app_.tabs.size()) - 1);
        app_.move_tab(drag_tab_, index);
        drag_tab_ = index;
        drag_y_ = y;
        return;
    }
    if (input.kind == PointerInput::Wheel && t && t->ntp && panel_ == None && !editing &&
        inside(web, x, y)) {
        if (input.point.modifiers & EVENTFLAG_CONTROL_DOWN)
            zoom_start_page(input.dy > 0 ? 1 : -1);
        else
            start_scroll_ = std::clamp(start_scroll_ + (input.dy < 0 ? 64 : -64), 0,
                                       start_page_layout().maximum);
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Filters) {
        panel_scroll_ = std::clamp(panel_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                   std::max(0, int(filter_lines_.size()) - 1));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Shortcuts) {
        panel_scroll_ = std::clamp(panel_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                   std::max(0, int(app_.profile.shortcuts.size()) - 1));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Passwords) {
        if (!app_.passwords.busy && inside(panel_bounds(), x, y))
            panel_scroll_ = std::clamp(panel_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                       std::max(0, int(app_.passwords.vault.logins().size()) - 1));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Extensions) {
        auto &m = app_.extensions;
        int count =
            m.candidate ? int(m.candidate->extension.permissions.size()) : int(m.items.size());
        panel_scroll_ =
            std::clamp(panel_scroll_ + (input.dy < 0 ? 1 : -1), 0, std::max(0, count - 1));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Settings) {
        const auto r = panel_bounds();
        if (inside(r, x, y))
            settings_scroll_ =
                std::clamp(settings_scroll_ + (input.dy < 0 ? 48 : -48), 0, settings_max_scroll(r));
        return;
    }
    if (input.kind == PointerInput::Wheel &&
        (panel_ == History || panel_ == Bookmarks || panel_ == Downloads)) {
        auto r = panel_bounds();
        if (inside(r, x, y)) {
            int count = panel_ == History     ? app_.profile.history.size()
                        : panel_ == Bookmarks ? app_.profile.bookmarks.size()
                                              : app_.downloads.size();
            panel_scroll_ =
                std::clamp(panel_scroll_ + (input.dy < 0 ? 2 : -2), 0, std::max(0, count - 4));
            return;
        }
    }
    if (input.kind == PointerInput::Wheel && x < 240) {
        if (app_.profile.wheel_switch && !app_.tabs.empty())
            app_.activate(
                (app_.active + (input.dy < 0 ? 1 : static_cast<int>(app_.tabs.size()) - 1)) %
                app_.tabs.size());
        else
            tab_scroll_ = std::max(0, tab_scroll_ + (input.dy < 0 ? 1 : -1));
        return;
    }
    if (!app_.dialogs.empty()) {
        if (down) {
            auto r = dialog_bounds();
            if (inside({r.x + r.width - 114, r.y + r.height - 60, 90, 35}, x, y))
                app_.answer_dialog(true);
            else if (inside({r.x + r.width - 217, r.y + r.height - 60, 90, 35}, x, y))
                app_.answer_dialog(false);
        }
        return;
    }
    if (app_.menu_callback || app_.menu_action) {
        if (down) {
            Rect r{app_.menu_x, app_.menu_y, app_.menu_width,
                   static_cast<int>(app_.menu.size()) * 32 + 12};
            if (inside(r, x, y)) {
                int i = (y - r.y - 6) / 32;
                if (i >= 0 && i < static_cast<int>(app_.menu.size()) && app_.menu[i].enabled &&
                    !app_.menu[i].label.empty()) {
                    auto cb = std::move(app_.menu_callback);
                    auto action = std::move(app_.menu_action);
                    int id = app_.menu[i].id;
                    app_.menu.clear();
                    if (action)
                        action(id);
                    else if (cb)
                        cb->Continue(id, static_cast<cef_event_flags_t>(input.point.modifiers));
                }
            } else
                app_.close_menu();
        }
        return;
    }
    if (editing) {
        if (down) {
            auto list = suggestions();
            Rect r{std::max(264, web.x + (web.width - 760) / 2), 96, std::min(760, web.width - 28),
                   85 + static_cast<int>(list.size()) * 54};
            if (!inside(r, x, y)) {
                dismiss();
                return;
            }
            if (y >= r.y + 83) {
                int i = (y - r.y - 83) / 54;
                if (i < static_cast<int>(list.size()))
                    app_.navigate(list[i].url);
            } else {
                const auto f = font(regular_, 18);
                const float pos = x - r.x - 59;
                std::size_t caret = 0;
                while (caret < address.value.size() &&
                       measure(f, address.value.substr(0, caret)) < pos) {
                    ++caret;
                    while (caret < address.value.size() &&
                           (static_cast<unsigned char>(address.value[caret]) & 0xC0) == 0x80)
                        ++caret;
                }
                address.caret = address.anchor = caret;
            }
        }
        return;
    }
    if (down && y >= 32 && y < 80) {
        for (const auto &[index, b] : extension_buttons())
            if (inside(b, x, y)) {
                if (input.button == 3) {
                    app_.close_menu();
                    app_.menu = {{1, app_.tr("Open popup")},
                                 {2, app_.tr("Unpin extension")},
                                 {3, app_.tr("Options")},
                                 {4, app_.tr("Manage extensions")}};
                    app_.menu_x = std::clamp(b.x - 180, 252, width_ - 250);
                    app_.menu_y = 82;
                    const auto id = app_.extensions.items[index].id;
                    app_.menu_action = [this, id](int action) {
                        auto &items = app_.extensions.items;
                        auto it = std::find_if(items.begin(), items.end(),
                                               [&](const auto &e) { return e.id == id; });
                        if (it == items.end())
                            return;
                        size_t i = it - items.begin();
                        if (action == 1)
                            app_.open_extension_popup(i);
                        else if (action == 2)
                            app_.pin_extension(i);
                        else if (action == 3)
                            app_.extension_page(i, true);
                        else
                            panel(Extensions);
                    };
                } else if (input.button == 1)
                    app_.open_extension_popup(index);
                return;
            }
    }
    if (panel_ != None) {
        if (down) {
            auto r = panel_bounds();
            if (!inside(r, x, y) || inside({r.x + r.width - 50, r.y + 18, 36, 36}, x, y)) {
                panel(None);
                return;
            }
            if (panel_ == Bookmarks || panel_ == History) {
                auto &list = panel_ == Bookmarks ? app_.profile.bookmarks : app_.profile.history;
                int i = (y - r.y - 103) / 61 + panel_scroll_;
                if (y >= r.y + 103 && i >= 0 && i < static_cast<int>(list.size())) {
                    if (x > r.x + r.width - 64) {
                        list.erase(list.begin() + i);
                        app_.changed(true);
                    } else {
                        auto url = list[i].url;
                        app_.navigate(url);
                    }
                }
            } else if (panel_ == Downloads) {
                if (inside({r.x + 25, r.y + 91, 132, 31}, x, y))
                    app_.open_folder();
                else if (y >= r.y + 141) {
                    int i = (y - r.y - 141) / 75 + panel_scroll_;
                    if (x > r.x + r.width - 110)
                        app_.download_action(i, x > r.x + r.width - 64);
                }
            } else if (panel_ == Settings)
                settings_pointer(r, x, y);
            else if (panel_ == Extensions)
                extensions_pointer(r, x, y);
            else if (panel_ == Shortcuts)
                shortcuts_pointer(r, x, y);
            else if (panel_ == Updates)
                updates_pointer(r, x, y);
            else if (panel_ == Import)
                import_pointer(r, x, y);
            else if (panel_ == Passwords)
                passwords_pointer(r, x, y);
            else if (panel_ == Filters)
                filters_pointer(r, x, y);
        }
        return;
    }
    if (find_open && inside({width_ - 390, 94, 360, 49}, x, y)) {
        if (down) {
            if (x > width_ - 59) {
                find_open = false;
                app_.find_text("");
            } else if (x > width_ - 86)
                app_.find_text(find.value, true);
            else if (x > width_ - 110)
                app_.find_text(find.value, true, true);
        }
        return;
    }
    if (down && x < 240) {
        if (inside({16, 12, 208, 62}, x, y)) {
            app_.close_menu();
            app_.menu = {
                {1, app_.tr("New window"), true, "Ctrl+N"},
                {2, app_.tr("New private window"), true, "Ctrl+Shift+N"},
                {3, app_.tr("Close window"), true, "Ctrl+Shift+W"},
                {5, app_.tr("Restore previous session"),
                 !app_.state()->previous_restored && !app_.state()->previous_windows.empty()},
                {4, app_.tr("Exit Aura"), true, "Ctrl+Q"}};
            app_.menu_width = 320;
            app_.menu_x = 16;
            app_.menu_y = 76;
            app_.menu_action = [this](int command) {
                if (command == 1 || command == 2)
                    app_.new_window(command == 2);
                else if (command == 3)
                    app_.request_quit();
                else if (command == 5)
                    app_.restore_previous_session();
                else
                    app_.request_exit();
            };
            return;
        }
        if (inside({16, 88, 208, 40}, x, y)) {
            app_.open_new_tab(t && t->private_tab);
            return;
        }
        if (y >= 178 && y < height_ - 220) {
            int index = (y - 178) / 52 + tab_scroll_;
            if (index - tab_scroll_ >= std::max(1, (height_ - 220 - 178) / 52))
                return;
            if (index < static_cast<int>(app_.tabs.size())) {
                if (input.button == 2 && !app_.profile.middle_close)
                    return;
                if (input.button == 2 || (x > 195 && !app_.tabs[index]->pinned))
                    app_.close_tab(index);
                else {
                    app_.activate(index);
                    drag_tab_ = index;
                    drag_y_ = y;
                    if (input.button == 3) {
                        const int id = app_.tabs[index]->id;
                        app_.menu = {{1, app_.tabs[index]->pinned ? app_.tr("Unpin tab")
                                                                  : app_.tr("Pin tab")},
                                     {2, app_.tabs[index]->muted ? app_.tr("Unmute tab")
                                                                 : app_.tr("Mute tab")},
                                     {3, app_.tr("Duplicate tab")},
                                     {4, app_.tr("Close tab")},
                                     {5, app_.tr("Reopen closed tab")}};
                        app_.menu_x = std::clamp(x + 8, 8, width_ - 250);
                        app_.menu_y = std::clamp(y, 32, height_ - 180);
                        app_.menu_action = [this, id](int command) {
                            auto tab = app_.tab(id);
                            if (!tab)
                                return;
                            int index = 0;
                            while (index < static_cast<int>(app_.tabs.size()) &&
                                   app_.tabs[index]->id != id)
                                ++index;
                            app_.activate(index);
                            if (command == 1)
                                app_.pin();
                            else if (command == 2)
                                app_.mute();
                            else if (command == 3)
                                app_.new_tab(tab->page.url, tab->private_tab);
                            else if (command == 4)
                                app_.close_tab(index);
                            else
                                app_.reopen();
                        };
                    }
                }
            }
            return;
        }
        for (int i = 0; i < 4; ++i)
            if (inside({16, height_ - 204 + i * 35, 208, 32}, x, y)) {
                panel(static_cast<Panel>(i + 1));
                return;
            }
    }
    if (down && y >= 32 && y < 80) {
        if (inside(address_bounds(), x, y)) {
            focus_address();
            return;
        }
        if (x >= 260 && x < 294) {
            app_.back();
            return;
        }
        if (x >= 298 && x < 332) {
            app_.forward();
            return;
        }
        if (x >= 336 && x < 370) {
            app_.reload();
            return;
        }
        if (x >= width_ - 230 && x < width_ - 196) {
            app_.protection_menu();
            return;
        }
        if (x >= width_ - 190 && x < width_ - 156) {
            panel(Extensions);
            return;
        }
        if (x >= width_ - 150 && x < width_ - 116) {
            app_.bookmark();
            return;
        }
        if (x >= width_ - 110 && x < width_ - 76) {
            panel(Downloads);
            return;
        }
        if (x >= width_ - 70 && x < width_ - 36) {
            panel(Settings);
            return;
        }
    }
    if (t && down && !chrome_store_id(t->page.url).empty() &&
        inside({web.x + web.width - 187, web.y + 22, 158, 33}, x, y)) {
        auto url = t->page.url;
        panel(Extensions);
        app_.extensions.fetch(url);
        return;
    }
    if (t && t->ntp && down && inside(web, x, y)) {
        const auto layout = start_page_layout();
        if (inside(layout.manage, x, y)) {
            open_shortcuts();
            return;
        }
        if (inside(layout.customize, x, y)) {
            panel(Settings);
            settings_section_ = 9;
            settings_scroll_ = 0;
            settings_search_.set("");
            return;
        }
        if (inside(layout.zoom_out, x, y)) {
            zoom_start_page(-1);
            return;
        }
        if (inside(layout.zoom_reset, x, y)) {
            zoom_start_page(0, true);
            return;
        }
        if (inside(layout.zoom_in, x, y)) {
            zoom_start_page(1);
            return;
        }
        if (inside(layout.viewport, x, y)) {
            if (inside(layout.search, x, y)) {
                focus_address();
                return;
            }
            for (const auto &[index, r] : layout.links)
                if (inside(r, x, y)) {
                    if (input.button == 3) {
                        app_.menu = {{1, app_.tr("Edit")},
                                     {2, app_.tr("Remove")},
                                     {3, app_.tr("Move up"), index > 0},
                                     {4, app_.tr("Move down"),
                                      size_t(index + 1) < app_.profile.shortcuts.size()}};
                        app_.menu_x = std::clamp(x, 8, width_ - 248);
                        app_.menu_y = std::clamp(y, 32, height_ - 150);
                        app_.menu_action = [this, index](int command) {
                            if (command == 1)
                                open_shortcuts(index);
                            else if (command == 2)
                                remove_shortcut(index);
                            else {
                                app_.profile.move_shortcut(index,
                                                           command == 3 ? index - 1 : index + 1);
                                app_.changed(true);
                            }
                        };
                    } else
                        app_.navigate(app_.profile.shortcuts[index].url);
                    return;
                }
        }
    }
    if (t && !t->ntp && t->page.failed && down &&
        inside({web.x + 64, web.y + 225, 125, 38}, x, y)) {
        app_.reload();
        return;
    }
    if (t && t->browser && !t->ntp) {
        auto point = input.point;
        point.x = int(std::lround((point.x - web.x) * app_.ui_scale));
        point.y = int(std::lround((point.y - web.y) * app_.ui_scale));
        const bool in = inside(web, x, y);
        const unsigned bit = 1u << std::min(input.button, 31);
        if (input.kind == PointerInput::Move || input.kind == PointerInput::Leave)
            t->browser->GetHost()->SendMouseMoveEvent(point,
                                                      !in || input.kind == PointerInput::Leave);
        else if (input.kind == PointerInput::Wheel && in) {
            if (input.point.modifiers & EVENTFLAG_CONTROL_DOWN)
                app_.zoom(input.dy > 0 ? 1 : -1);
            else
                t->browser->GetHost()->SendMouseWheelEvent(point, input.dx, input.dy);
        } else if ((down && in) ||
                   (input.kind == PointerInput::Up && (in || (web_buttons_ & bit)))) {
            if (down) {
                ++app_.audit["web_pointer_down"];
                web_buttons_ |= bit;
                t->browser->GetHost()->SetFocus(true);
                find_open = false;
            } else
                web_buttons_ &= ~bit;
            auto button = input.button == 1 ? MBT_LEFT : input.button == 2 ? MBT_MIDDLE : MBT_RIGHT;
            t->browser->GetHost()->SendMouseClickEvent(point, button, !down, input.clicks);
        }
        if (!in)
            app_.window->cursor(0);
    } else
        app_.window->cursor(inside(address_bounds(), x, y) ? 1 : x < 240 ? 2 : 0);
}
} // namespace aura

namespace aura {
void BrowserUi::show_protection_settings() {
    panel(Settings);
    settings_section_ = 7;
    settings_search_.set("");
    settings_scroll_ = 0;
    app_.changed();
}
void BrowserUi::draw_extensions(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    auto &m = app_.extensions;
    auto chip = [&](Rect b, const std::string &text, bool primary = false, bool enabled = true) {
        if (enabled && b.y >= r.y + 202 && b.y + b.height <= r.y + r.height - 116)
            focus_target(b);
        if (enabled && b.y < r.y + 202)
            focus_target(b);
        if (enabled && b.y >= r.y + r.height - 88)
            focus_target(b);
        const bool hover = enabled && inside(b, hover_x_, hover_y_);
        fill(c, b, 8,
             primary && enabled ? t.accent
                                : SkColorSetA(t.accent, hover     ? 32
                                                        : enabled ? 18
                                                                  : 6));
        centered_label(c, medium_, app_.tr(text), b, 12,
                       !enabled  ? t.muted
                       : primary ? t.background
                                 : t.text);
    };
    label(c, regular_, app_.tr("Chrome Web Store URL / ID, or an absolute .crx path"), r.x + 28,
          r.y + 77, 12, t.muted, r.width - 56);
    Rect input{r.x + 28, r.y + 90, r.width - 180, 37};
    focus_target(input);
    fill(c, input, 9, SkColorSetA(t.text, 7));
    border(c, input, 9,
           SkColorSetA(extension_input_focus_ ? t.accent : t.muted,
                       extension_input_focus_ ? 255 : 190));
    draw_editor(c, extension_input_, {input.x + 10, input.y + 2, input.width - 20, 33}, 12,
                extension_input_focus_);
    chip({r.x + r.width - 140, r.y + 90, 112, 37}, m.busy ? "Cancel" : "Download", !m.busy, true);
    label(c, regular_, app_.tr(m.status), r.x + 28, r.y + 154, 12, t.accent, r.width - 56);
    if (m.candidate) {
        const auto &e = m.candidate->extension;
        label(c, medium_, e.name + " · " + e.version, r.x + 28, r.y + 192, 18, t.text,
              r.width - 56);
        const auto old = std::find_if(m.items.begin(), m.items.end(),
                                      [&](const auto &item) { return item.id == e.id; });
        const auto added = m.added_permissions();
        label(c, regular_,
              app_.tr(old == m.items.end() ? "Requested permissions and website access"
                                           : "New permissions are highlighted"),
              r.x + 28, r.y + 224, 12, t.muted, r.width - 56);
        c.save();
        c.clipRect(SkRect::MakeXYWH(r.x + 24, r.y + 239, r.width - 48, r.height - 362));
        if (e.permissions.empty())
            label(c, regular_, app_.tr("No additional permissions"), r.x + 40, r.y + 266, 13,
                  t.text);
        for (size_t i = panel_scroll_; i < e.permissions.size(); i++)
            label(c, regular_,
                  (std::find(added.begin(), added.end(), e.permissions[i]) != added.end() ? "+ "
                                                                                          : "• ") +
                      (e.permissions[i].starts_with("optional:")
                           ? app_.tr("Optional: ") + e.permissions[i].substr(9)
                           : e.permissions[i]),
                  r.x + 40, r.y + 266 + (i - panel_scroll_) * 29, 13, t.text, r.width - 80);
        c.restore();
        label(c, regular_,
              std::to_string(e.permissions.size()) + " " +
                  app_.tr("permissions · scroll to review all"),
              r.x + 28, r.y + r.height - 106, 11, t.muted, r.width - 56);
        chip({r.x + 28, r.y + r.height - 88, 158, 36},
             old == m.items.end() ? "Install extension" : "Update extension", true);
        chip({r.x + 198, r.y + r.height - 88, 112, 36}, "Cancel");
    } else {
        label(c, regular_, app_.tr("Changes take effect after restarting Aura."), r.x + 28,
              r.y + 188, 12, t.muted, r.width - 56);
        c.save();
        c.clipRect(SkRect::MakeXYWH(r.x + 20, r.y + 202, r.width - 40, r.height - 318));
        if (m.items.empty())
            label(c, regular_, app_.tr("Your extensions will appear here."), r.x + 28, r.y + 248,
                  14, t.muted, r.width - 56);
        for (size_t i = panel_scroll_; i < m.items.size(); i++) {
            auto &e = m.items[i];
            int y = r.y + 210 + (i - panel_scroll_) * 119;
            fill(c, {r.x + 24, y, r.width - 48, 108}, 12, SkColorSetA(t.text, 5));
            const auto &bitmap = app_.extension_icon(i);
            if (bitmap.generation())
                image(c, bitmap, {r.x + 40, y + 11, 22, 22});
            else
                icon(c, "extension", r.x + 40, y + 11, 22, t.accent);
            label(c, medium_, e.name + " · " + e.version, r.x + 72, y + 27, 15, t.text,
                  r.width - 240);
            chip({r.x + r.width - 156, y + 8, 128, 32},
                 e.pinned ? "Unpin extension" : "Pin extension");
            label(c, regular_,
                  app_.tr(e.pending_update ? "Update after restart"
                          : !e.enabled ? (e.load_requested ? "Disabled after restart" : "Disabled")
                          : !e.load_requested ? "Restart Aura to activate"
                          : e.observed        ? "Activity detected"
                                              : "Enabled; awaiting activity"),
                  r.x + 40, y + 47, 12, t.muted, r.width - 80);
            chip({r.x + 40, y + 60, 104, 34}, e.enabled ? "Disable" : "Enable");
            chip({r.x + 152, y + 60, 108, 34}, "Open popup", false,
                 e.load_requested && e.enabled && !e.popup.empty());
            chip({r.x + 268, y + 60, 104, 34}, "Options", false,
                 e.load_requested && e.enabled && !e.options.empty());
            chip({r.x + 380, y + 60, 104, 34}, "Remove");
            chip({r.x + 492, y + 60, 104, 34}, "Check", false, !m.busy);
        }
        c.restore();
        chip({r.x + 28, r.y + r.height - 88, 216, 36}, "Open Chrome Web Store",
             m.items.empty() && extension_input_.value.empty());
        if (m.restart_required)
            chip({r.x + r.width - 256, r.y + r.height - 88, 228, 36}, "Restart Aura", true,
                 !m.busy);
        label(c, regular_, app_.tr("Some Chrome tab and toolbar APIs are unavailable in Aura."),
              r.x + 28, r.y + r.height - 106, 11, t.muted, r.width - 56);
    }
    label(c, regular_,
          app_.tr(m.restart_required ? "Restart required · use Restart Aura"
                                     : "MV3 extensions · signed CRX3 packages"),
          r.x + 28, r.y + r.height - 25, 11, m.restart_required ? t.accent : t.muted, r.width - 56);
}
void BrowserUi::extensions_pointer(Rect r, int x, int y) {
    auto &m = app_.extensions;
    extension_input_focus_ = inside({r.x + 28, r.y + 90, r.width - 180, 37}, x, y);
    if (extension_input_focus_) {
        app_.window->caret(r.x + 40, r.y + 127);
        return;
    }
    if (inside({r.x + r.width - 140, r.y + 90, 112, 37}, x, y)) {
        panel_scroll_ = 0;
        if (m.busy)
            m.cancel();
        else
            m.fetch(extension_input_.value);
        return;
    }
    if (m.candidate) {
        if (inside({r.x + 28, r.y + r.height - 88, 158, 36}, x, y)) {
            m.accept();
            panel_scroll_ = 0;
        } else if (inside({r.x + 198, r.y + r.height - 88, 112, 36}, x, y)) {
            m.reject();
            panel_scroll_ = 0;
        }
        return;
    }
    if (m.restart_required && !m.busy &&
        inside({r.x + r.width - 256, r.y + r.height - 88, 228, 36}, x, y)) {
        app_.restart();
        return;
    }
    if (inside({r.x + 28, r.y + r.height - 88, 216, 36}, x, y)) {
        panel(None);
        app_.new_tab("https://chromewebstore.google.com/");
        return;
    }
    if (y < r.y + 202 || y > r.y + r.height - 116)
        return;
    size_t index = (y - r.y - 210) / 119 + panel_scroll_;
    if (index >= m.items.size())
        return;
    int top = r.y + 210 + (index - panel_scroll_) * 119;
    if (inside({r.x + r.width - 156, top + 8, 128, 32}, x, y))
        app_.pin_extension(index);
    else if (inside({r.x + 40, top + 60, 104, 34}, x, y))
        m.toggle(index);
    else if (inside({r.x + 152, top + 60, 108, 34}, x, y))
        app_.extension_page(index, false);
    else if (inside({r.x + 268, top + 60, 104, 34}, x, y))
        app_.extension_page(index, true);
    else if (!m.busy && inside({r.x + 492, top + 60, 104, 34}, x, y)) {
        m.check_update(index);
        if (m.items[index].source != "Chrome Web Store") {
            extension_input_.set("");
            extension_input_focus_ = true;
        }
    } else if (inside({r.x + 380, top + 60, 104, 34}, x, y))
        app_.confirm_action("Remove extension?", m.items[index].name, [this, index] {
            app_.extensions.remove(index);
            panel_scroll_ = 0;
        });
}
std::string BrowserUi::extensions_report() const {
    if (panel_ != Extensions)
        return "null";
    auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    return "{\"bounds\":" + rect(r) +
           ",\"input\":" + rect({r.x + 28, r.y + 90, r.width - 180, 37}) +
           ",\"download\":" + rect({r.x + r.width - 140, r.y + 90, 112, 37}) +
           ",\"install\":" + rect({r.x + 28, r.y + r.height - 88, 158, 36}) +
           ",\"cancel\":" + rect({r.x + 198, r.y + r.height - 88, 112, 36}) +
           ",\"restart\":" + rect({r.x + r.width - 256, r.y + r.height - 88, 228, 36}) + "}";
}
} // namespace aura

namespace aura {
std::vector<std::pair<size_t, Rect>> BrowserUi::extension_buttons() const {
    std::vector<size_t> indices;
    const size_t capacity = std::max(0, (width_ - 628 - 180) / 36);
    for (size_t i = 0; i < app_.extensions.items.size() && indices.size() < capacity; i++)
        if (app_.extensions.items[i].pinned && app_.extensions.items[i].enabled)
            indices.push_back(i);
    std::vector<std::pair<size_t, Rect>> out;
    for (size_t n = 0; n < indices.size(); n++)
        out.push_back({indices[n], {width_ - 270 - int(indices.size() - 1 - n) * 36, 37, 34, 36}});
    return out;
}
void BrowserUi::layout_extension_popup() {
    auto p = app_.extension_popup;
    if (!p)
        return;
    int w = std::clamp(p->desired_width, 180, std::min(800, width_ - 296));
    int h = std::clamp(p->desired_height, 80, std::min(600, height_ - 140));
    int anchor = width_ - 156;
    for (const auto &[index, b] : extension_buttons())
        if (app_.extensions.items[index].id == p->extension_id)
            anchor = b.x + b.width;
    p->bounds = {std::clamp(anchor - w - 2, web_bounds().x + 8, width_ - w - 14), 82, w + 2,
                 h + 41};
    p->content = {p->bounds.x + 1, p->bounds.y + 40, w, h};
    const bool resized = p->page.view_width != w || p->page.view_height != h;
    p->page.view_width = w;
    p->page.view_height = h;
    if (p->browser && !p->page.closing) {
        if (resized)
            p->browser->GetHost()->WasResized();
        p->browser->GetHost()->NotifyScreenInfoChanged();
    }
}
void BrowserUi::draw_extension_popup(SkCanvas &c) {
    auto p = app_.extension_popup;
    if (!p || p->page.closing)
        return;
    const auto &t = interface_theme(app_.profile);
    const auto r = p->bounds, content = p->content;
    shadow(c, r, 12);
    c.save();
    c.clipRRect(SkRRect::MakeRectXY(box(r), 12, 12), true);
    fill(c, r, 12, t.surface);
    size_t index = 0;
    while (index < app_.extensions.items.size() &&
           app_.extensions.items[index].id != p->extension_id)
        ++index;
    const auto &bitmap = app_.extension_icon(index);
    if (bitmap.generation())
        image(c, bitmap, {r.x + 13, r.y + 10, 20, 20});
    else
        icon(c, "extension", r.x + 13, r.y + 10, 20, t.accent);
    label(c, medium_, p->name, r.x + 43, r.y + 25, 12, t.text, r.width - 124);
    if (index < app_.extensions.items.size()) {
        if (inside({r.x + r.width - 72, r.y + 5, 31, 30}, hover_x_, hover_y_))
            fill(c, {r.x + r.width - 72, r.y + 5, 31, 30}, 7, SkColorSetA(t.accent, 25));
        icon(c, "pin", r.x + r.width - 65, r.y + 12, 17,
             app_.extensions.items[index].pinned ? t.accent : t.muted);
    }
    if (inside({r.x + r.width - 36, r.y + 5, 30, 30}, hover_x_, hover_y_))
        fill(c, {r.x + r.width - 36, r.y + 5, 30, 30}, 7, SkColorSetA(t.accent, 25));
    icon(c, "close", r.x + r.width - 29, r.y + 12, 17, t.muted);
    fill(c, content, 0, SK_ColorWHITE);
    if (p->page.view.generation())
        image(c, p->page.view, content);
    if (p->page.popup_visible && p->page.popup.generation()) {
        c.save();
        c.clipRect(box(content));
        const auto &b = p->page.popup_bounds;
        image(c, p->page.popup, {content.x + b.x, content.y + b.y, b.width, b.height});
        c.restore();
    }
    if (p->page.failed)
        label(c, regular_, app_.tr("Could not open extension popup."), content.x + 18,
              content.y + 35, 12, 0xFF333333, content.width - 36);
    else if (!p->page.loaded)
        label(c, regular_, app_.tr("Opening extension..."), content.x + 18, content.y + 35, 12,
              0xFF666666, content.width - 36);
    c.restore();
    border(c, r, 12, SkColorSetA(t.text, 36));
}
bool BrowserUi::popup_pointer(const PointerInput &input) {
    auto p = app_.extension_popup;
    if (!p)
        return false;
    const int x = input.point.x, y = input.point.y;
    const bool down = input.kind == PointerInput::Down;
    if (down && !inside(p->bounds, x, y)) {
        bool same = false;
        for (const auto &[index, b] : extension_buttons())
            if (inside(b, x, y) && app_.extensions.items[index].id == p->extension_id)
                same = true;
        app_.close_extension_popup();
        popup_buttons_ = 0;
        return same || (x >= 240 && y >= 80);
    }
    if (down && inside(p->bounds, x, y) && !inside(p->content, x, y)) {
        if (inside({p->bounds.x + p->bounds.width - 36, p->bounds.y + 5, 30, 30}, x, y)) {
            app_.close_extension_popup();
            popup_buttons_ = 0;
        } else if (inside({p->bounds.x + p->bounds.width - 72, p->bounds.y + 5, 31, 30}, x, y)) {
            for (size_t i = 0; i < app_.extensions.items.size(); i++)
                if (app_.extensions.items[i].id == p->extension_id) {
                    app_.pin_extension(i);
                    break;
                }
        }
        return true;
    }
    if (!p->browser)
        return true;
    auto point = input.point;
    point.x -= p->content.x;
    point.y -= p->content.y;
    const bool inside_content = inside(p->content, x, y);
    const unsigned bit = 1u << std::min(input.button, 31);
    if (input.kind == PointerInput::Move || input.kind == PointerInput::Leave) {
        p->browser->GetHost()->SendMouseMoveEvent(point, !inside_content ||
                                                             input.kind == PointerInput::Leave);
        if (!inside_content)
            app_.window->cursor(0);
    } else if (input.kind == PointerInput::Wheel && inside_content)
        p->browser->GetHost()->SendMouseWheelEvent(point, input.dx, input.dy);
    else if ((down && inside_content) ||
             (input.kind == PointerInput::Up && (inside_content || (popup_buttons_ & bit)))) {
        if (down) {
            popup_buttons_ |= bit;
            p->browser->GetHost()->SetFocus(true);
        } else
            popup_buttons_ &= ~bit;
        p->browser->GetHost()->SendMouseClickEvent(point,
                                                   input.button == 1   ? MBT_LEFT
                                                   : input.button == 2 ? MBT_MIDDLE
                                                                       : MBT_RIGHT,
                                                   !down, input.clicks);
    }
    return true;
}
std::string BrowserUi::extension_popup_report() const {
    auto p = app_.extension_popup;
    if (!p)
        return "null";
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    return "{\"id\":" + json_quote(p->extension_id) + ",\"title\":" + json_quote(p->page.title) +
           ",\"url\":" + json_quote(p->page.url) +
           ",\"loaded\":" + (p->page.loaded ? "true" : "false") +
           ",\"frames\":" + std::to_string(p->page.view.generation()) +
           ",\"bounds\":" + rect(p->bounds) + ",\"content\":" + rect(p->content) +
           ",\"close\":" + rect({p->bounds.x + p->bounds.width - 36, p->bounds.y + 5, 30, 30}) +
           ",\"pin\":" + rect({p->bounds.x + p->bounds.width - 72, p->bounds.y + 5, 31, 30}) + "}";
}
std::string BrowserUi::extension_toolbar_report() const {
    std::string out = "[";
    bool first = true;
    for (const auto &[index, b] : extension_buttons()) {
        if (!first)
            out += ',';
        first = false;
        out += "{\"id\":" + json_quote(app_.extensions.items[index].id) +
               ",\"icon_loaded\":" + (app_.extension_icon(index).generation() ? "true" : "false") +
               ",\"bounds\":[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]}";
    }
    return out + "]";
}
} // namespace aura

namespace aura {
namespace {
using PasswordControls = std::vector<std::pair<std::string, Rect>>;
PasswordControls password_controls(Rect r, bool locked, int mode) {
    PasswordControls out;
    if (locked || mode != 0) {
        int count = mode == 1 && !locked ? 3 : 2;
        for (int i = 0; i < count; ++i)
            out.push_back(
                {"field" + std::to_string(i), {r.x + 28, r.y + 128 + i * 70, r.width - 56, 36}});
        if (locked)
            out.push_back({"unlock", {r.x + 28, r.y + 285, r.width - 56, 36}});
        else if (mode == 1)
            out.push_back({"save", {r.x + 28, r.y + 350, r.width - 56, 36}});
        else if (mode == 3)
            out.push_back({"change", {r.x + 28, r.y + 285, r.width - 56, 36}});
        else {
            int w = (r.width - 68) / 2;
            out.push_back({"export", {r.x + 28, r.y + 285, w, 36}});
            out.push_back({"import", {r.x + 40 + w, r.y + 285, w, 36}});
        }
        if (!locked)
            out.push_back({"back", {r.x + 28, r.y + r.height - 66, r.width - 56, 36}});
    } else {
        const char *ids[] = {"capture", "fill",     "add",         "edit",
                             "delete",  "transfer", "change_mode", "lock"};
        int w = (r.width - 68) / 2;
        for (int i = 0; i < 8; ++i)
            out.push_back(
                {ids[i],
                 {r.x + 28 + (i % 2) * (w + 12), r.y + r.height - 202 + (i / 2) * 43, w, 34}});
    }
    return out;
}
std::shared_ptr<std::string> secret_text(std::string value) {
    return {new std::string(std::move(value)), [](std::string *p) {
                erase_secret(*p);
                delete p;
            }};
}
} // namespace
void BrowserUi::clear_password_fields() {
    for (auto &field : password_fields_) {
        erase_secret(field.value);
        field.caret = field.anchor = 0;
    }
    password_focus_ = -1;
}
void BrowserUi::draw_passwords(SkCanvas &c, Rect r) {
    auto &p = app_.passwords;
    const auto &t = interface_theme(app_.profile);
    label(c, regular_, app_.tr(p.status), r.x + 28, r.y + 87, 12, t.muted, r.width - 56);
    if (p.busy)
        return;
    bool locked = !p.vault.unlocked();
    auto controls = password_controls(r, locked, password_mode_);
    bool exists = p.vault.exists();
    const char *titles[3] = {};
    if (locked) {
        titles[0] = exists ? "Master password" : "Create master password";
        titles[1] = exists ? "" : "Confirm master password";
    } else if (password_mode_ == 1) {
        titles[0] = "HTTPS site";
        titles[1] = "Username";
        titles[2] = "Password";
    } else if (password_mode_ == 2) {
        titles[0] = "Transfer file path";
        titles[1] = "Transfer password";
    } else if (password_mode_ == 3) {
        titles[0] = "New master password";
        titles[1] = "Confirm master password";
    }
    for (const auto &[id, b] : controls) {
        if (id.starts_with("field")) {
            int i = id.back() - '0';
            if (!titles[i] || !*titles[i])
                continue;
            focus_target(b);
            label(c, regular_, app_.tr(titles[i]), b.x, b.y - 9, 12, t.muted, b.width);
            fill(c, b, 9, SkColorSetA(t.text, 7));
            border(c, b, 9, password_focus_ == i ? t.accent : SkColorSetA(t.muted, 190));
            bool secret = locked || password_mode_ == 3 || (password_mode_ == 1 && i == 2) ||
                          (password_mode_ == 2 && i == 1);
            if (secret) {
                TextEditor masked;
                masked.value.assign(password_fields_[i].value.size(), '*');
                masked.caret = password_fields_[i].caret;
                masked.anchor = password_fields_[i].anchor;
                draw_editor(c, masked, {b.x + 10, b.y + 2, b.width - 20, 32}, 13,
                            password_focus_ == i);
            } else
                draw_editor(c, password_fields_[i], {b.x + 10, b.y + 2, b.width - 20, 32}, 12,
                            password_focus_ == i);
            continue;
        }
        bool selected =
            password_selected_ >= 0 && password_selected_ < int(p.vault.logins().size());
        bool enabled = (id != "fill" && id != "edit" && id != "delete") || selected;
        if (enabled)
            focus_target(b);
        const char *title = id == "unlock"        ? (exists ? "Unlock vault" : "Create vault")
                            : id == "save"        ? "Save login"
                            : id == "change"      ? "Change master password"
                            : id == "export"      ? "Export encrypted file"
                            : id == "import"      ? "Import encrypted file"
                            : id == "back"        ? "Back"
                            : id == "capture"     ? "Save login from page"
                            : id == "fill"        ? "Fill selected login"
                            : id == "add"         ? "Add login"
                            : id == "edit"        ? "Edit selected login"
                            : id == "delete"      ? "Delete selected login"
                            : id == "transfer"    ? "Transfer passwords"
                            : id == "change_mode" ? "Change master password"
                                                  : "Lock vault";
        fill(c, b, 9, SkColorSetA(t.accent, enabled && inside(b, hover_x_, hover_y_) ? 30 : 12));
        border(c, b, 9, SkColorSetA(t.muted, enabled ? 180 : 60));
        centered_label(c, medium_, app_.tr(title), b, 12, enabled ? t.text : t.muted, 8, 8);
    }
    if (!locked && password_mode_ == 0) {
        int visible = std::max(1, (r.height - 320) / 48),
            start = std::min(panel_scroll_, std::max(0, int(p.vault.logins().size()) - visible));
        if (p.vault.logins().empty())
            label(c, regular_, app_.tr("No saved logins yet."), r.x + 28, r.y + 135, 13, t.muted,
                  r.width - 56);
        for (int i = start; i < std::min(int(p.vault.logins().size()), start + visible); ++i) {
            const auto &v = p.vault.logins()[i];
            Rect b{r.x + 28, r.y + 108 + (i - start) * 48, r.width - 56, 44};
            focus_target(b);
            fill(c, b, 8, SkColorSetA(t.accent, i == password_selected_ ? 30 : 8));
            label(c, medium_, v.origin, b.x + 10, b.y + 19, 12, t.text, b.width - 20);
            label(c, regular_, v.username.empty() ? app_.tr("No username") : v.username, b.x + 10,
                  b.y + 36, 11, t.muted, b.width - 20);
        }
    }
    const char *hint =
        locked ? "Use at least 12 characters. A lost master password cannot be recovered."
        : password_mode_ == 2
            ? "Existing logins are kept. Import saves an encrypted backup."
            : "Explicit filling only, on the same HTTPS site. Locks after 15 minutes.";
    label(c, regular_, app_.tr(hint), r.x + 28, r.y + r.height - 13, 10, t.muted, r.width - 56);
}
void BrowserUi::passwords_pointer(Rect r, int x, int y) {
    auto &p = app_.passwords;
    if (p.busy)
        return;
    bool locked = !p.vault.unlocked();
    password_focus_ = -1;
    for (const auto &[id, b] : password_controls(r, locked, password_mode_))
        if (inside(b, x, y)) {
            if (id.starts_with("field")) {
                int i = id.back() - '0';
                if (locked && p.vault.exists() && i == 1)
                    return;
                password_focus_ = i;
                p.touch();
                return;
            }
            auto field = [&](int i) { return secret_text(password_fields_[i].value); };
            if (id == "unlock" || id == "change") {
                if ((!p.vault.exists() || id == "change") &&
                    password_fields_[0].value != password_fields_[1].value) {
                    toast(app_.tr("Passwords do not match."));
                    return;
                }
                auto password = field(0);
                bool exists = p.vault.exists();
                bool change = id == "change";
                p.run([&p, password, exists, change] {
                    if (change)
                        p.vault.change_password(*password);
                    else if (exists)
                        p.vault.unlock(*password);
                    else
                        p.vault.create(*password);
                    return change ? "Master password changed." : "Password vault unlocked.";
                });
                clear_password_fields();
                password_mode_ = 0;
            } else if (id == "lock") {
                p.lock();
                clear_password_fields();
                password_selected_ = -1;
            } else if (id == "back") {
                clear_password_fields();
                password_mode_ = 0;
            } else if (id == "add" || id == "edit") {
                clear_password_fields();
                password_edit_ = -1;
                if (id == "edit") {
                    if (password_selected_ < 0 ||
                        password_selected_ >= int(p.vault.logins().size()))
                        return;
                    password_edit_ = password_selected_;
                    auto &v = p.vault.logins()[password_selected_];
                    password_fields_[0].set(v.origin);
                    password_fields_[1].set(v.username);
                    password_fields_[2].set(v.password);
                }
                password_mode_ = 1;
            } else if (id == "transfer" || id == "change_mode") {
                clear_password_fields();
                password_mode_ = id == "transfer" ? 2 : 3;
            } else if (id == "save") {
                auto origin = password_origin(password_fields_[0].value);
                if (origin.empty()) {
                    toast(app_.tr("Choose a valid HTTPS site and a nonempty password."));
                    return;
                }
                auto user = field(1), password = field(2);
                int edit = password_edit_;
                p.run([&p, origin, user, password, edit] {
                    if (edit >= 0)
                        p.vault.replace(edit, {origin, *user, *password});
                    else
                        p.vault.save({origin, *user, *password});
                    return "Login saved.";
                });
                clear_password_fields();
                password_mode_ = 0;
            } else if (id == "delete") {
                if (password_selected_ < 0 || password_selected_ >= int(p.vault.logins().size()))
                    return;
                const auto index = password_selected_;
                app_.confirm_action(app_.tr("Delete selected login"),
                                    p.vault.logins()[index].origin, [this, index] {
                                        auto &p = app_.passwords;
                                        p.run([&p, index] {
                                            p.vault.remove(index);
                                            return "Login deleted.";
                                        });
                                        password_selected_ = -1;
                                    });
            } else if (id == "capture")
                app_.page_login(false);
            else if (id == "fill") {
                if (password_selected_ >= 0)
                    app_.page_login(true, password_selected_);
            } else if (id == "export" || id == "import") {
                auto path = path_from_utf8(password_fields_[0].value);
                auto password = field(1);
                bool exporting = id == "export";
                p.run([&p, path, password, exporting] {
                    if (exporting) {
                        p.vault.export_file(path, *password);
                        return std::string("Encrypted file exported.");
                    }
                    auto n = p.vault.import_file(path, *password);
                    (void)n;
                    return std::string("Encrypted file imported.");
                });
                clear_password_fields();
            }
            app_.changed();
            return;
        }
    if (!locked && password_mode_ == 0) {
        int visible = std::max(1, (r.height - 320) / 48),
            start = std::min(panel_scroll_, std::max(0, int(p.vault.logins().size()) - visible));
        int index = start + (y - r.y - 108) / 48;
        if (y >= r.y + 108 && y < r.y + 108 + visible * 48 && index < int(p.vault.logins().size()))
            password_selected_ = index;
    }
}
std::string BrowserUi::passwords_report() const {
    if (panel_ != Passwords)
        return "null";
    auto &p = app_.passwords;
    bool locked = p.busy || !p.vault.unlocked();
    std::string out = "{\"busy\":" + std::string(p.busy ? "true" : "false") +
                      ",\"locked\":" + (locked ? "true" : "false") +
                      ",\"mode\":" + std::to_string(password_mode_) +
                      ",\"count\":" + std::to_string(locked ? 0 : p.vault.logins().size()) +
                      ",\"status\":" + json_quote(p.status) + ",\"controls\":{";
    bool comma = false;
    for (const auto &[id, r] : password_controls(panel_bounds(), locked, password_mode_)) {
        if (comma)
            out += ",";
        comma = true;
        out += json_quote(id) + ":[" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
               std::to_string(r.width) + "," + std::to_string(r.height) + "]";
    }
    out += "},\"rows\":[";
    if (!locked && password_mode_ == 0) {
        auto r = panel_bounds();
        int visible = std::max(1, (r.height - 320) / 48),
            start = std::min(panel_scroll_, std::max(0, int(p.vault.logins().size()) - visible));
        for (int i = start; i < std::min(int(p.vault.logins().size()), start + visible); ++i) {
            if (i > start)
                out += ",";
            out += "[" + std::to_string(r.x + 28) + "," +
                   std::to_string(r.y + 108 + (i - start) * 48) + "," +
                   std::to_string(r.width - 56) + ",44]";
        }
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::sync_filter_lines() {
    const auto &manager = app_.filter_lists();
    if (filter_seen_ == manager.revision)
        return;
    filter_seen_ = manager.revision;
    filter_lines_.clear();
    std::istringstream text(manager.settings().custom);
    std::string line;
    while (std::getline(text, line))
        if (!line.empty())
            filter_lines_.push_back(std::move(line));
    panel_scroll_ = std::clamp(panel_scroll_, 0, std::max(0, int(filter_lines_.size()) - 1));
}
void BrowserUi::save_filter_rule() {
    auto &manager = app_.filter_lists();
    if (manager.busy() || filter_input_.value.empty())
        return;
    if (filter_edit_ >= 0 && filter_edit_revision_ != manager.revision) {
        toast(app_.tr("Rules changed in another window. Select the rule again."));
        return;
    }
    sync_filter_lines();
    auto lines = filter_lines_;
    if (filter_edit_ >= 0 && size_t(filter_edit_) < lines.size())
        lines[filter_edit_] = filter_input_.value;
    else
        lines.push_back(filter_input_.value);
    std::string rules;
    for (const auto &line : lines)
        rules += line + '\n';
    filter_submit_revision_ = manager.revision;
    filter_submitted_ = filter_input_.value;
    filter_pending_ = true;
    manager.set_custom(std::move(rules));
    filter_input_focus_ = false;
}
void BrowserUi::draw_filters(SkCanvas &c, Rect r) {
    sync_filter_lines();
    auto &manager = app_.filter_lists();
    const auto &theme = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &text, bool primary = false, bool enabled = true) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8,
             enabled && primary ? theme.accent : SkColorSetA(theme.text, enabled ? 12 : 6));
        if (inside(b, hover_x_, hover_y_) && enabled)
            border(c, b, 8, theme.accent);
        centered_label(c, medium_, app_.tr(text), b, 12,
                       enabled ? (primary ? theme.background : theme.text) : theme.muted);
    };
    button({r.x + r.width - 178, r.y + 72, 150, 34}, manager.busy() ? "Cancel" : "Check lists",
           true);
    auto message = app_.tr(manager.status);
    const auto lines = wrap_text(font(regular_, 12), message, r.width - 236, 2);
    for (size_t i = 0; i < lines.size(); ++i)
        label(c, regular_, lines[i], r.x + 28, r.y + 84 + int(i) * 17, 12, theme.muted,
              r.width - 236);
    for (size_t i = 0; i < 2; ++i) {
        const int y = r.y + 124 + int(i) * 48;
        label(c, medium_, i == 0 ? "EasyList" : "EasyPrivacy", r.x + 28, y + 20, 14, theme.text,
              148);
        std::string date = app_.tr("Bundled snapshot");
        if (manager.settings().updated[i] > 0) {
            const auto local = local_time(std::time_t(manager.settings().updated[i]));
            char text[32];
            std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
            date = text;
        }
        label(c, regular_, date, r.x + 202, y + 20, 12, theme.muted, r.width - 354);
        button({r.x + r.width - 132, y + 1, 104, 32}, manager.settings().enabled[i] ? "On" : "Off",
               false, !manager.busy());
    }
    label(c, regular_,
          app_.tr("Parsed rules: ") + std::to_string(manager.stats.network) + " / " +
              std::to_string(manager.stats.cosmetic) + " / " +
              std::to_string(manager.stats.skipped),
          r.x + 28, r.y + 236, 12, theme.muted, r.width - 56);
    label(c, medium_, app_.tr("Custom rule"), r.x + 28, r.y + 270, 13, theme.text, r.width - 220);
    button({r.x + r.width - 140, r.y + 249, 112, 26},
           filter_edit_ >= 0 ? "Cancel edit" : "Clear rules", false,
           !manager.busy() && (filter_edit_ >= 0 || !filter_lines_.empty()));
    Rect input{r.x + 28, r.y + 282, r.width - 180, 36};
    if (!manager.busy())
        focus_target(input);
    fill(c, input, 8, theme.background);
    border(c, input, 8, filter_input_focus_ ? theme.accent : theme.muted);
    draw_editor(c, filter_input_, {input.x + 10, input.y + 2, input.width - 20, input.height - 4},
                12, filter_input_focus_);
    button({r.x + r.width - 140, r.y + 282, 112, 36}, filter_edit_ >= 0 ? "Save rule" : "Add rule",
           false, !manager.busy() && !filter_input_.value.empty());
    c.save();
    c.clipRect(SkRect::MakeXYWH(r.x + 24, r.y + 328, r.width - 48, std::max(0, r.height - 454)));
    if (filter_lines_.empty())
        label(c, regular_, app_.tr("Add a rule or import a UTF-8 text file."), r.x + 28, r.y + 352,
              12, theme.muted, r.width - 56);
    for (size_t i = panel_scroll_;
         i < filter_lines_.size() &&
         r.y + 328 + int(i - panel_scroll_) * 38 + 32 <= r.y + r.height - 126;
         ++i) {
        const int y = r.y + 328 + int(i - panel_scroll_) * 38;
        Rect row{r.x + 28, y, r.width - 150, 32};
        if (!manager.busy())
            focus_target(row);
        if (int(i) == filter_edit_)
            fill(c, row, 6, SkColorSetA(theme.accent, 20));
        label(c, regular_, filter_lines_[i], row.x + 6, row.y + 21, 12, theme.text, row.width - 12);
        button({r.x + r.width - 110, y, 82, 32}, "Remove", false, !manager.busy());
    }
    c.restore();
    label(c, regular_, app_.tr("Network / cosmetic / skipped; scroll to review custom rules."),
          r.x + 28, r.y + r.height - 106, 10, theme.muted, r.width - 56);
    button({r.x + 28, r.y + r.height - 88, 160, 34}, "Import rules", false, !manager.busy());
    button({r.x + r.width - 248, r.y + r.height - 88, 220, 34}, "Restore bundled lists", false,
           !manager.busy());
    label(c, regular_, app_.tr("Network changes require a page reload."), r.x + 28,
          r.y + r.height - 35, 10, theme.muted, r.width - 56);
    label(c, regular_, app_.tr("Scriptlets, CSP and unsupported procedural rules are not applied."),
          r.x + 28, r.y + r.height - 19, 10, theme.muted, r.width - 56);
}
void BrowserUi::filters_pointer(Rect r, int x, int y) {
    auto &manager = app_.filter_lists();
    if (inside({r.x + r.width - 178, r.y + 72, 150, 34}, x, y)) {
        if (manager.busy())
            manager.cancel();
        else
            manager.check();
        return;
    }
    if (manager.busy())
        return;
    if (filter_seen_ != manager.revision) {
        sync_filter_lines();
        app_.changed();
        return;
    }
    filter_input_focus_ = inside({r.x + 28, r.y + 282, r.width - 180, 36}, x, y);
    if (filter_input_focus_) {
        app_.window->caret(r.x + 38, r.y + 314);
        return;
    }
    for (size_t i = 0; i < 2; ++i)
        if (inside({r.x + r.width - 132, r.y + 125 + int(i) * 48, 104, 32}, x, y)) {
            manager.set_list(i, !manager.settings().enabled[i]);
            return;
        }
    if (inside({r.x + r.width - 140, r.y + 282, 112, 36}, x, y)) {
        save_filter_rule();
        return;
    }
    if (inside({r.x + r.width - 140, r.y + 249, 112, 26}, x, y) && filter_edit_ >= 0) {
        filter_edit_ = -1;
        filter_input_.set("");
        return;
    }
    if (inside({r.x + r.width - 140, r.y + 249, 112, 26}, x, y) && !filter_lines_.empty()) {
        app_.confirm_action("Remove all custom rules?",
                            "Bundled lists and site exceptions are kept.",
                            [this] { app_.filter_lists().set_custom(""); });
        return;
    }
    if (y >= r.y + 328 && y < r.y + r.height - 126) {
        const size_t index = panel_scroll_ + (y - r.y - 328) / 38;
        if (index >= filter_lines_.size())
            return;
        if (x >= r.x + r.width - 110) {
            auto lines = filter_lines_;
            lines.erase(lines.begin() + index);
            std::string rules;
            for (const auto &line : lines)
                rules += line + '\n';
            manager.set_custom(std::move(rules));
        } else {
            filter_edit_ = int(index);
            filter_edit_revision_ = manager.revision;
            filter_input_.set(filter_lines_[index]);
            filter_input_focus_ = true;
        }
        return;
    }
    if (inside({r.x + 28, r.y + r.height - 88, 160, 34}, x, y))
        choose_file(false, true);
    if (inside({r.x + r.width - 248, r.y + r.height - 88, 220, 34}, x, y))
        manager.restore_bundled();
}
std::string BrowserUi::filters_report() const {
    if (panel_ != Filters)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out =
        "{\"bounds\":" + rect(r) + ",\"check\":" + rect({r.x + r.width - 178, r.y + 72, 150, 34}) +
        ",\"input\":" + rect({r.x + 28, r.y + 282, r.width - 180, 36}) +
        ",\"save\":" + rect({r.x + r.width - 140, r.y + 282, 112, 36}) +
        ",\"clear\":" + rect({r.x + r.width - 140, r.y + 249, 112, 26}) +
        ",\"import\":" + rect({r.x + 28, r.y + r.height - 88, 160, 34}) +
        ",\"bundled\":" + rect({r.x + r.width - 248, r.y + r.height - 88, 220, 34}) +
        ",\"picker_pid\":" + std::to_string(picker_pid_) +
        ",\"easylist_toggle\":" + rect({r.x + r.width - 132, r.y + 125, 104, 32}) +
        ",\"easyprivacy_toggle\":" + rect({r.x + r.width - 132, r.y + 173, 104, 32}) +
        ",\"custom_count\":" + std::to_string(filter_lines_.size()) +
        ",\"input_length\":" + std::to_string(filter_input_.value.size()) + ",\"rows\":[";
    bool first = true;
    for (size_t i = panel_scroll_;
         i < filter_lines_.size() && 328 + int(i - panel_scroll_) * 38 + 32 <= r.height - 126;
         ++i) {
        if (!first)
            out += ',';
        first = false;
        const int y = r.y + 328 + int(i - panel_scroll_) * 38;
        out += "{\"index\":" + std::to_string(i) +
               ",\"edit\":" + rect({r.x + 28, y, r.width - 150, 32}) +
               ",\"remove\":" + rect({r.x + r.width - 110, y, 82, 32}) + "}";
    }
    return out + "]}";
}
} // namespace aura
