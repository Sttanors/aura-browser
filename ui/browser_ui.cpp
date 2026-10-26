#include "ui/browser_ui.hpp"
#include "aura/design_tokens.hpp"
#include "common/branding_data.hpp"
#include "common/config.hpp"
#include "common/exports.hpp"
#include "common/platform.hpp"
#include "common/text.hpp"
#include "common/update_key.hpp"
#include "core/browser/client.hpp"
#include "core/browser/permissions.hpp"
#include "core/browser/spelling.hpp"
#include "core/browser/translation.hpp"
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
#include <iomanip>
#include <map>
#include <sstream>
#include <tuple>
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
SkColor tab_group_color(const Profile &p, int index) {
    static const SkColor dark[] = {0, 0xFF8DBEFF, 0xFF82D6B0, 0xFFF3A6C7, 0xFFEAC17B, 0xFF73DCD2};
    static const SkColor light[] = {0, 0xFF245CC6, 0xFF216747, 0xFFA12F62, 0xFF825400, 0xFF176B65};
    index = std::clamp(index, 0, 5);
    return index ? (p.theme % 2 ? light[index] : dark[index]) : interface_theme(p).accent;
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
    size_t low = 0, high = text.size();
    while (low < high) {
        auto middle = low + (high - low + 1) / 2;
        while (middle < high && (static_cast<unsigned char>(text[middle]) & 0xC0) == 0x80)
            ++middle;
        if (measure(f, text.substr(0, middle) + "…") <= width)
            low = middle;
        else {
            high = middle - 1;
            while (high > low && (static_cast<unsigned char>(text[high]) & 0xC0) == 0x80)
                --high;
        }
    }
    return text.substr(0, low) + "…";
}
std::vector<std::string> wrap_text(const SkFont &f, const std::string &text, int width, int limit) {
    std::vector<std::string> lines;
    std::string remaining = text;
    while (!remaining.empty() && int(lines.size()) < limit) {
        if (int(lines.size()) == limit - 1) {
            std::replace(remaining.begin(), remaining.end(), '\n', ' ');
            lines.push_back(elide(f, remaining, width));
            break;
        }
        const auto newline = remaining.find('\n');
        if (newline != std::string::npos && measure(f, remaining.substr(0, newline)) <= width) {
            lines.push_back(remaining.substr(0, newline));
            remaining.erase(0, newline + 1);
            continue;
        }
        if (measure(f, remaining) <= width) {
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
void page_image(SkCanvas &c, const PaintBuffer &buffer, Rect web, SkColor background) {
    // GPU canvases retain image data until submission. Preserve their original
    // single-image path instead of making a full buffer copy for every part.
    if (c.recordingContext()) {
        fill(c, web, 15, background);
        c.save();
        c.clipRRect(SkRRect::MakeRectXY(box(web), 15, 15), true);
        image(c, buffer, web);
        c.restore();
        return;
    }
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
    const auto page_part = [&](const SkRect &rect, bool curved) {
        c.save();
        c.resetMatrix();
        c.clipRect(rect);
        c.setMatrix(matrix);
        fill(c, web, curved ? 15 : 0, background);
        if (curved)
            c.clipRRect(SkRRect::MakeRectXY(box(web), 15, 15), true);
        image(c, buffer, web);
        c.restore();
    };
    for (const auto &rect : interiors)
        page_part(rect, false);
    for (const auto &rect : perimeter)
        page_part(rect, true);
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
        {"expand", "M8 5 L15 12 L8 19"},
        {"collapse", "M5 8 L12 15 L19 8"},
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
        {"camera",
         "M3 7 L7 7 L9 4 L15 4 L17 7 L21 7 L21 20 L3 20 Z M16 13 A4 4 0 1 1 8 13 A4 4 0 1 1 16 13"},
        {"microphone", "M12 2 A3 3 0 0 0 9 5 L9 11 A3 3 0 0 0 15 11 L15 5 A3 3 0 0 0 12 2 M5 10 L5 "
                       "11 A7 7 0 0 0 19 11 L19 10 M12 18 L12 22 M8 22 L16 22"},
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
    const int left = horizontal_tabs() ? 12 : 252;
    return {left, 88, std::max(100, width_ - left - 12), std::max(100, height_ - 100)};
}
Rect BrowserUi::address_bounds() const {
    const int left = navigation_x() + 124;
    return {left, 36, std::max(150, width_ - left - 244 - int(extension_buttons().size()) * 36),
            38};
}
Rect BrowserUi::panel_bounds() const {
    const auto r = web_bounds();
    const int w = std::min(panel_ == Settings             ? 1000
                           : panel_ == Extensions         ? 900
                           : panel_ == Shortcuts          ? 900
                           : panel_ == Passwords          ? 900
                           : panel_ == Import             ? 900
                           : panel_ == Groups             ? 900
                           : panel_ == Containers         ? 900
                           : panel_ == Commands           ? 900
                           : panel_ == Hotkeys            ? 900
                           : panel_ == DuplicateTabs      ? 900
                           : panel_ == Diagnostics        ? 900
                           : panel_ == ExtensionAccess    ? 900
                           : panel_ == SpellingDictionary ? 900
                           : panel_ == SearchProviders    ? 900
                           : panel_ == ReadingListPanel   ? 900
                           : panel_ == TaskManager        ? 900
                           : panel_ == Screenshot         ? 900
                           : panel_ == Sessions           ? 900
                           : panel_ == Bookmarks          ? 900
                           : panel_ == SiteData           ? 900
                           : panel_ == Permissions        ? 900
                           : panel_ == Translation        ? 900
                           : panel_ == Profiles           ? 900
                           : panel_ == Filters            ? 900
                                                          : 660,
                           r.width - 24);
    const int h = std::min(
        panel_ == Settings     ? 760
        : panel_ == Extensions ? 700
        : panel_ == Shortcuts  ? 700
        : panel_ == Passwords  ? 700
        : panel_ == Import     ? 700
        : panel_ == Commands
            ? std::clamp(180 + int(std::min<std::size_t>(8, command_results().size())) * 52, 284,
                         596)
        : panel_ == SiteData
            ? std::clamp(460 + 26 * int(std::count_if(
                                        app_.site_data.types.begin(), app_.site_data.types.end(),
                                        [](const auto &item) { return item.second > 0; })),
                         512, 620)
        : panel_ == Hotkeys
            ? std::clamp(280 + int(std::min<size_t>(7, hotkey_results().size())) * 56, 392, 700)
        : panel_ == DuplicateTabs      ? 700
        : panel_ == Diagnostics        ? 580
        : panel_ == ExtensionAccess    ? 700
        : panel_ == SpellingDictionary ? 620
        : panel_ == SearchProviders    ? 700
        : panel_ == ReadingListPanel   ? 700
        : panel_ == TaskManager        ? 700
        : panel_ == Screenshot         ? 520
        : panel_ == Sessions
            ? std::clamp(268 + int(app_.profile.named_sessions.size()) * 90, 390, 700)
        : panel_ == Permissions ? 700
        : panel_ == Translation ? 520
        : panel_ == Profiles    ? std::clamp(236 + int(listed_profiles().size()) * 84, 340, 700)
        : panel_ == Containers
            ? std::clamp(260 + int(container_rules_mode_ ? app_.profile.container_rules.size()
                                                         : app_.profile.containers.size()) *
                                   88,
                         440, 700)
        : panel_ == Groups
            ? std::clamp(
                  260 + int(std::count_if(app_.profile.groups.begin(), app_.profile.groups.end(),
                                          [&](const auto &g) {
                                              return g.workspace == app_.workspace;
                                          })) *
                            88,
                  400, 700)
        : panel_ == Filters ? 700
        : panel_ == Workspaces
            ? std::clamp(218 + int(app_.profile.workspaces.size()) * 52, 380, 700)
            : 580,
        height_ - 124);
    return {r.x + (r.width - w) / 2,
            panel_ == Commands ? std::clamp((height_ - 500) / 4, 100, 140)
                               : std::max(100, (height_ - h) / 2),
            w, h};
}
Rect BrowserUi::dialog_bounds() const {
    int h = 230;
    if (!app_.dialogs.empty() && app_.dialogs.front().owned && !app_.dialogs.front().prompt) {
        const int lines =
            int(wrap_text(font(regular_, 13), app_.dialogs.front().message, 440, 8).size());
        h = std::min(std::max(230, 148 + lines * 18), std::max(230, height_ - 140));
    }
    return {std::max(horizontal_tabs() ? 12 : 260, (width_ - 490) / 2),
            std::max(110, (height_ - h) / 2), 490, h};
}
std::array<Rect, 2> BrowserUi::dialog_buttons() const {
    const auto r = dialog_bounds();
    const auto &d = app_.dialogs.front();
    const auto accept = app_.tr(!d.accept_label.empty() ? d.accept_label.c_str()
                                : d.permission          ? "Allow"
                                : d.before_unload       ? "Leave"
                                                        : "OK");
    const auto cancel = app_.tr(d.permission ? "Deny" : "Cancel");
    const int a = std::clamp(int(std::ceil(measure(font(medium_, 12), accept))) + 24, 90, 180),
              b = std::clamp(int(std::ceil(measure(font(medium_, 12), cancel))) + 24, 90, 180);
    return {{{r.x + r.width - 24 - a - 13 - b, r.y + r.height - 60, b, 35},
             {r.x + r.width - 24 - a, r.y + r.height - 60, a, 35}}};
}
void BrowserUi::focus_target(Rect r) {
    if (r.x >= 0 && r.y >= 0 && r.x + r.width <= width_ && r.y + r.height <= height_)
        focus_targets_.push_back(r);
}
void BrowserUi::resize(int width, int height) {
    if (app_.screenshot.selecting) {
        finish_screenshot_area(false);
        app_.screenshot.error = app_.tr("The page view changed. Select the area again.");
    }
    visible_active_id_ = -1;
    strip_active_id_ = -1;
    keyboard_focus_ = -1;
    width_ = width;
    height_ = height;
}
void BrowserUi::draw_editor(SkCanvas &c, const TextEditor &e, Rect r, float size, bool active) {
    // Clipboard input can be megabytes. Keep the full editor value, but shape
    // only a bounded UTF-8 window around the caret for a responsive native UI.
    if (e.value.size() > 8192) {
        size_t begin = e.caret > 4096 ? e.caret - 4096 : 0;
        while (begin && (static_cast<unsigned char>(e.value[begin]) & 0xC0) == 0x80)
            --begin;
        size_t end = std::min(e.value.size(), begin + 8192);
        while (end < e.value.size() && (static_cast<unsigned char>(e.value[end]) & 0xC0) == 0x80)
            --end;
        TextEditor visible;
        visible.value = e.value.substr(begin, end - begin);
        visible.caret = std::clamp(e.caret, begin, end) - begin;
        visible.anchor = std::clamp(e.anchor, begin, end) - begin;
        draw_editor(c, visible, r, size, active);
        return;
    }
    if (active && !preedit_.empty() && (&e == &address || &e == &find)) {
        auto draft = e;
        draft.insert(preedit_);
        auto composing = std::move(preedit_);
        draw_editor(c, draft, r, size, active);
        preedit_ = std::move(composing);
        return;
    }
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
    if (app_.state()->tab_drop.target == app_.window_uid)
        return false;
    const auto current = app_.current();
    // A CEF repaint changes only the page. Preserve native chrome and its focus
    // targets; overlays use the complete painter so they remain above the page.
    if (app_.split_enabled() || !current || current->ntp || current->loading ||
        current->page.failed || !current->page.view.generation() || !web_backdrop_ ||
        panel_ != None || editing || find_open || app_.extension_popup || !app_.dialogs.empty() ||
        !app_.menu.empty() || app_.menu_action || app_.menu_callback || keyboard_focus_ >= 0 ||
        (app_.profile.show_status && !status.empty()) || strip_hint_visible_ ||
        (!toast_.empty() && std::chrono::steady_clock::now() < toast_until_))
        return false;
    const auto web = web_bounds();
    c.save();
    c.resetMatrix();
    c.drawImage(web_backdrop_, backdrop_x_, backdrop_y_);
    c.restore();
    page_image(c, current->page.view, web,
               app_.profile.theme % 2 ? 0xFFFFFFFF : 0xFF161B25);
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
    const int shell_left = horizontal_tabs() ? 0 : 240;
    fill(c, {shell_left, 0, width_ - shell_left, height_}, 0, SkColorSetA(t.background, 80));
    if (!horizontal_tabs()) {
        brand_logo(c, {20, 17, 30, 30});
        label(c, medium_, "aura", 58, 40, 23, ink);
        label(c, regular_, app_.workspace_name(), 24, 65, 11, muted, 196);
        focus_target({16, 88, 208, 40});
        fill(c, {16, 88, 208, 40}, 11, SkColorSetA(t.surface, light ? 205 : 150));
        border(c, {16, 88, 208, 40}, 11, SkColorSetA(ink, 18));
        icon(c, "plus", 29, 99, 18, t.accent);
        focus_target({16, 12, 208, 62});
        label(c, medium_, app_.tr("New tab"), 59, 113, 13, ink);
        if (shortcut_hint("new_tab").size() <= 6)
            label(c, regular_, shortcut_hint("new_tab"), 170, 112, 10, muted, 50);
        const bool receiving = app_.state()->tab_drop.target == app_.window_uid;
        label(c, receiving ? medium_ : regular_, app_.tr(receiving ? "Move tab here" : "Tabs"), 24,
              161, 12, receiving ? ink : muted, 180);
        const auto visible = sidebar_rows();
        int visible_active =
            int(std::find(visible.begin(), visible.end(), app_.active) - visible.begin());
        const auto tree = app_.tree_rows(false);
        if (app_.profile.tree_tabs && visible_active == int(visible.size()) && current) {
            auto parent = app_.tab(current->parent);
            int remaining = int(app_.tabs.size());
            while (parent && remaining-- > 0) {
                for (int position = 0; position < int(visible.size()); ++position)
                    if (visible[position] >= 0 && app_.tabs[visible[position]]->id == parent->id)
                        visible_active = position;
                if (visible_active < int(visible.size()))
                    break;
                parent = app_.tab(parent->parent);
            }
        }
        if (visible_active == int(visible.size()) && current && current->group)
            visible_active =
                int(std::find(visible.begin(), visible.end(), -current->group) - visible.begin());
        if (!receiving)
            label(c, regular_, std::to_string(app_.visible_tabs().size()), 207, 161, 9, muted);
        const int list_bottom = height_ - 220;
        int tab_capacity = std::max(1, (list_bottom - 178) / 52);
        if (current && (current->id != visible_active_id_ || app_.active != visible_active_index_ ||
                        visible_active != visible_active_row_)) {
            if (visible_active < tab_scroll_)
                tab_scroll_ = visible_active;
            else if (visible_active >= tab_scroll_ + tab_capacity)
                tab_scroll_ = visible_active - tab_capacity + 1;
            visible_active_id_ = current->id;
            visible_active_index_ = app_.active;
            visible_active_row_ = visible_active;
        }
        tab_scroll_ = std::clamp(tab_scroll_, 0,
                                 std::max(0, static_cast<int>(visible.size()) - tab_capacity));
        c.save();
        c.clipRect(SkRect::MakeLTRB(12, 176, 232, list_bottom));
        for (int position = tab_scroll_; position < static_cast<int>(visible.size()); ++position) {
            const int i = visible[position];
            const int y = 178 + (position - tab_scroll_) * 52;
            if (i < 0) {
                if (y + 46 > list_bottom)
                    break;
                const auto group = app_.profile.group(-i);
                if (!group)
                    continue;
                const int count =
                    int(std::count_if(app_.tabs.begin(), app_.tabs.end(), [&](const auto &tab) {
                        return tab->workspace == app_.workspace && tab->group == group->id;
                    }));
                focus_target({14, y, 180, 46});
                focus_target({196, y + 7, 30, 32});
                fill(c, {14, y, 212, 46}, 4,
                     SkColorSetA(tab_group_color(app_.profile, group->color),
                                 inside({14, y, 212, 46}, hover_x_, hover_y_) ? 35 : 18));
                icon(c, group->collapsed ? "expand" : "collapse", 24, y + 8, 16, t.text);
                label(c, medium_, group->name, 46, y + 21, 12,
                      tab_group_color(app_.profile, group->color), 145);
                label(c, regular_,
                      std::to_string(count) + " " +
                          app_.tr(group->collapsed ? "tabs, collapsed" : "tabs, expanded"),
                      46, y + 38, 10, t.muted, 145);
                icon(c, "plus", 201, y + 15, 18, t.accent);
                continue;
            }
            auto &tab = *app_.tabs[i];
            if (y + 46 > list_bottom)
                break;
            const bool selected = i == app_.active;
            const bool branch_selected = !selected && position == visible_active;
            const auto row =
                std::find_if(tree.begin(), tree.end(), [&](const auto &r) { return r.index == i; });
            const int depth = app_.profile.tree_tabs && row != tree.end() ? row->depth : 0;
            const int descendants =
                app_.profile.tree_tabs && row != tree.end() ? row->descendants : 0;
            const int indent = std::min(depth, 3) * 12;
            const int icon_x = app_.profile.tree_tabs ? 44 + indent : 26;
            const int title_x = app_.profile.tree_tabs ? 70 + indent : 57;
            Rect r{14, y, 212, 46};
            if (auto group = app_.profile.group(tab.group))
                fill(c, {19, y + 8, 2, 30}, 1, tab_group_color(app_.profile, group->color));
            if (descendants)
                focus_target({20 + indent, y + 7, 24, 32});
            focus_target({app_.profile.tree_tabs ? 44 + indent : r.x, r.y,
                          app_.profile.tree_tabs ? 150 - indent : 180, r.height});
            if (!tab.pinned)
                focus_target({200, y + 8, 24, 30});
            const bool hover = inside(r, hover_x_, hover_y_);
            if (selected || branch_selected || hover) {
                fill(c, r, 12, SkColorSetA(t.surface, selected ? (light ? 235 : 245) : 125));
                if (selected || branch_selected) {
                    if (selected)
                        border(c, r, 12, SkColorSetA(ink, 22));
                    fill(c, {14, y + 13, 3, 20}, 2, t.accent);
                }
            }
            if (descendants) {
                icon(c, tab.tree_collapsed ? "expand" : "collapse", 24 + indent, y + 15, 16,
                     t.accent);
            }
            if (tab.loading) {
                const double time = std::chrono::duration<double>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
                fill(c, {icon_x + 3, y + 17, 12, 12}, 6,
                     SkColorSetA(
                         t.accent,
                         static_cast<U8CPU>(
                             130 + 100 * (app_.profile.reduce_motion ? 0.0 : std::sin(time * 4)))));
            } else if (tab.sleeping)
                label(c, medium_, "z", icon_x + 4, y + 29, 17, muted);
            else if (tab.media_capture)
                icon(c, tab.media_video ? "camera" : "microphone", icon_x, y + 14, 19, t.accent);
            else if (tab.private_tab)
                icon(c, "private", icon_x, y + 14, 19, t.accent);
            else if (app_.profile.show_favicons && tab.favicon.generation())
                image(c, tab.favicon, {icon_x + 1, y + 15, 17, 17});
            else
                icon(c, tab.ntp ? "plus" : "globe", icon_x, y + 14, 19,
                     selected ? t.accent : muted);
            label(c, selected ? medium_ : regular_,
                  tab.page.title.empty() ? url_host(tab.page.url) : tab.page.title, title_x,
                  y + (app_.profile.show_tab_host ? 21 : 28), 12.5f, ink, 196 - title_x);
            if (app_.profile.show_tab_host)
                label(c, regular_,
                      descendants && tab.tree_collapsed
                          ? app_.tr("Hidden tabs: ") + std::to_string(descendants)
                      : depth > 3       ? app_.tr("Level ") + std::to_string(depth + 1)
                      : tab.media_video ? app_.tr("Camera in use")
                      : tab.media_audio ? app_.tr("Microphone in use")
                      : tab.sleeping    ? app_.tr("Sleeping")
                      : tab.private_tab ? app_.tr("Private browsing")
                      : tab.ntp
                          ? app_.tr("A fresh perspective")
                          : (tab.container ? app_.profile.container_label(tab.container) + " · "
                                           : "") +
                                url_host(tab.page.url),
                      title_x, y + 36, 10, muted, 196 - title_x);
            if (hover || selected)
                icon(c, tab.pinned ? "pin" : "close", 202, y + 16, 14, muted);
        }
        c.restore();
        const std::string labels[] = {app_.tr("Bookmarks"), app_.tr("History"),
                                      app_.tr("Downloads"), app_.tr("Settings")};
        const char *icons[] = {"star", "history", "download", "settings"};
        for (int i = 0; i < 4; ++i) {
            Rect r{16, height_ - 204 + i * 35, 208, 32};
            focus_target(r);
            if (inside(r, hover_x_, hover_y_) || panel_ == i + 1)
                fill(c, r, 9, SkColorSetA(t.surface, 180));
            icon(c, icons[i], 28, r.y + 7, 18, muted);
            label(c, regular_, labels[i], 58, r.y + 21, 12, ink);
            if (i == 2 && !app_.visible_downloads().empty())
                label(c, medium_, std::to_string(app_.visible_downloads().size()), 204, r.y + 20,
                      10, t.accent);
        }
        focus_target({16, height_ - 60, 208, 54});
        fill(c, {20, height_ - 48, 28, 28}, 10, SkColorSetA(t.accent, 35));
        label(c, medium_, current && current->private_tab ? "P" : "A", 29, height_ - 29, 12,
              t.accent);
        label(c, medium_,
              current && current->private_tab ? app_.tr("Private space") : app_.profile_name(), 58,
              height_ - 31, 11, ink, 162);
        label(c, regular_,
              current && current->private_tab ? app_.profile_name() : app_.tr("Local profile"), 58,
              height_ - 17, 9, muted, 162);
    } else {
        draw_tab_strip(c);
        focus_target({12, 37, 34, 36});
        if (inside({12, 37, 34, 36}, hover_x_, hover_y_))
            fill(c, {12, 37, 34, 36}, 9, SkColorSetA(t.accent, 22));
        brand_logo(c, {17, 42, 24, 24});
    }
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
    const std::string title =
        app_.safe_mode() ? app_.tr("Safe mode") : (current ? current->page.title : "Aura");
    if (!horizontal_tabs())
        label(c, regular_, title, 265, 21, 10, muted, width_ - 435);
    if (!horizontal_tabs())
        label(c, regular_, current && current->private_tab ? app_.tr("PRIVATE") : "", width_ - 240,
              21, 9, t.accent);
    button({width_ - 144, 0, 48, 30}, "", true);
    SkPaint line;
    line.setAntiAlias(true);
    line.setColor(muted);
    line.setStrokeWidth(1.2f);
    c.drawLine(width_ - 125, 16, width_ - 114, 16, line);
    line.setStyle(SkPaint::kStroke_Style);
    c.drawRect(SkRect::MakeXYWH(width_ - 77, 10, 10, 10), line);
    icon(c, "close", width_ - 39, 7, 17, muted);
    button({navigation_x(), 37, 34, 36}, "back", current && current->back);
    button({navigation_x() + 38, 37, 34, 36}, "forward", current && current->forward);
    button({navigation_x() + 76, 37, 34, 36}, current && current->loading ? "close" : "reload");
    focus_target({addr.x + 5, addr.y + 4, 30, 30});
    focus_target({addr.x + 40, addr.y, addr.width - 40, addr.height});
    fill(c, addr, 12, SkColorSetA(t.surface, light ? 230 : 185));
    border(c, addr, 12, SkColorSetA(muted, 190));
    icon(c,
         current && current->media_video   ? "camera"
         : current && current->media_audio ? "microphone"
         : current && current->private_tab ? "private"
         : current && current->page.loaded && !current->page.failed &&
                 current->page.url.starts_with("https://")
             ? "lock"
             : "search",
         addr.x + 12, addr.y + 10, 17, muted);
    label(c, regular_,
          current && !current->ntp ? current->page.url : app_.tr("Search or enter an address"),
          addr.x + 40, addr.y + 24, 12, current && !current->ntp ? ink : muted,
          addr.width - (current && current->container
                            ? std::min(84, std::max(48, addr.width / 3)) + 64
                            : 150));
    if (current && current->container) {
        const auto container = app_.profile.container(current->container);
        const int width = std::min(84, std::max(48, addr.width / 3));
        const auto color = tab_group_color(app_.profile, container ? container->color : 0);
        const Rect badge{addr.x + addr.width - width - 12, addr.y + 7, width, 24};
        focus_target(badge);
        fill(c, badge, 6, SkColorSetA(color, 22));
        border(c, badge, 6, SkColorSetA(color, 85));
        centered_label(c, medium_, app_.profile.container_label(current->container), badge, 10,
                       color, 5, 5);
    } else
        label(c, regular_, shortcut_hint("focus_address"), addr.x + addr.width - 100, addr.y + 24,
              9, muted, 88);
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
        const bool usable =
            e.load_requested && !(current && (current->private_tab || current->container));
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
    if (app_.split_enabled())
        draw_split(c);
    else if (current && !current->ntp)
        draw_page(c, *current, web);
    else
        draw_start_page(c);
    draw_tab_hint(c);
    draw_tab_drop(c);
    if (app_.profile.show_status && !status.empty() && !editing && panel_ == None) {
        const auto area = current ? page_bounds(*current) : web;
        Rect r{area.x + 9, area.y + area.height - 31,
               std::min(area.width - 18, static_cast<int>(status.size() * 7 + 22)), 25};
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
        fill(c, {shell_left, 30, width_ - shell_left, height_ - 30}, 0,
             SkColorSetA(0xFF050712, light ? 40 : 90));
        focus_targets_.clear();
        const auto r = panel_bounds();
        focus_target({r.x + r.width - 52, r.y + 16, 36, 36});
        shadow(c, r, design::radii::overlay);
        fill(c, r, design::radii::overlay, t.surface);
        border(c, r, design::radii::overlay, SkColorSetA(ink, 30));
        const std::string titles[] = {
            "",
            app_.tr("Bookmarks"),
            app_.tr("History"),
            app_.tr("Downloads"),
            app_.tr("Settings"),
            app_.tr("Extensions"),
            app_.tr("Quick links"),
            app_.tr("About Aura and updates"),
            app_.tr("Import browser data"),
            app_.tr("Passwords"),
            app_.tr("Filter lists"),
            app_.tr("Workspaces"),
            app_.tr("Tab groups"),
            app_.tr(container_rules_mode_ ? "Site rules" : "Site containers"),
            app_.tr("Profiles"),
            app_.tr("Command palette"),
            app_.tr("Translate"),
            app_.tr("Site permissions"),
            app_.tr("Site data"),
            app_.tr("Session snapshots"),
            app_.tr("Screenshot"),
            app_.tr("Keyboard shortcuts"),
            app_.tr("Task manager"),
            app_.tr("Read later"),
            app_.tr("Search engines"),
            app_.tr("Duplicate tabs"),
            app_.tr(spelling_private_ ? "Private session dictionary" : "Spelling dictionary"),
            app_.tr("Extension site access"),
            app_.tr("Diagnostics")};
        label(c, medium_, titles[panel_], r.x + 28, r.y + 47, 23, ink);
        if (panel_ == Bookmarks || panel_ == Settings || panel_ == Groups) {
            Rect button{r.x + r.width - 220, r.y + 21, 160, 32};
            focus_target(button);
            border(c, button, 8, muted);
            centered_label(c, medium_,
                           app_.tr(panel_ == Bookmarks ? "Read later"
                                   : panel_ == Groups  ? "Duplicate tabs"
                                                       : "Search engines"),
                           button, 12, ink, 8, 8);
        }
        icon(c, "close", r.x + r.width - 43, r.y + 26, 19, muted);
        if (panel_ == Bookmarks)
            draw_bookmarks(c, r);
        if (panel_ == History) {
            auto &list = panel_ == History ? app_.profile.history : app_.profile.bookmarks;
            std::string info = panel_ == History
                                   ? app_.tr("Your recent places, stored on this device.")
                                   : app_.tr("Good places, kept close.");
            label(c, regular_, info, r.x + 28, r.y + 73, 12, muted);
            if (list.empty())
                label(c, regular_,
                      panel_ == History ? app_.tr("Your history starts with your first page.")
                                        : app_.tr("Save a page using the star button."),
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
            const auto visible = app_.visible_downloads();
            label(c, regular_,
                  app_.tr("Downloads from all windows of this profile. Private downloads stay in "
                          "their window."),
                  r.x + 28, r.y + 75, 12, muted);
            fill(c, {r.x + 25, r.y + 91, 132, 31}, 8, SkColorSetA(t.accent, 25));
            icon(c, "folder", r.x + 35, r.y + 98, 17, t.accent);
            centered_label(c, medium_, app_.tr("Show folder"), {r.x + 25, r.y + 91, 132, 31}, 11,
                           ink, 34, 8);
            focus_target({r.x + 25, r.y + 91, 132, 31});
            if (visible.empty())
                label(c, regular_, app_.tr("Your downloads will appear here."), r.x + 28, r.y + 169,
                      14, muted);
            c.save();
            c.clipRect(
                SkRect::MakeLTRB(r.x + 12, r.y + 132, r.x + r.width - 12, r.y + r.height - 14));
            for (std::size_t i = panel_scroll_; i < visible.size(); ++i) {
                auto &d = app_.downloads[visible[i]];
                int y = r.y + 141 + (static_cast<int>(i) - panel_scroll_) * 75;
                if (y > r.y + r.height - 20)
                    break;
                icon(c, d.complete ? "check" : "download", r.x + 29, y + 7, 20, t.accent);
                label(c, medium_, d.name.empty() ? app_.tr("Downloading…") : d.name, r.x + 62,
                      y + 21, 13, ink,
                      r.width - (d.complete || d.cancelled || d.interrupted ? 230 : 170));
                label(c, regular_,
                      d.cancelled  ? app_.tr("Cancelled")
                      : d.complete ? app_.tr("Complete · ") + bytes(d.received)
                      : d.interrupted
                          ? app_.tr("Interrupted · reason ") + std::to_string(d.interrupt_reason)
                      : d.paused ? app_.tr("Paused")
                                 : bytes(d.received) + " / " + bytes(d.total),
                      r.x + 62, y + 42, 11, muted);
                if (d.complete || d.cancelled || d.interrupted) {
                    Rect action{r.x + r.width - 149, y + 5, 124, 34};
                    if (y + 50 <= r.y + r.height - 14 && d.checksum_status != "hashing")
                        focus_target(action);
                    fill(c, action, 8,
                         SkColorSetA(t.accent, d.checksum_status == "hashing" ? 8 : 20));
                    centered_label(c, medium_,
                                   app_.tr(d.checksum_status == "hashing" ? "Hashing…"
                                           : d.complete                   ? "SHA-256"
                                                                          : "Retry"),
                                   action, 12, d.checksum_status == "hashing" ? muted : ink, 5, 5);
                    if (!d.checksum_status.empty()) {
                        const char *status =
                            d.checksum_status == "hashing"    ? "Hashing…"
                            : d.checksum_status == "match"    ? "SHA-256 matches"
                            : d.checksum_status == "mismatch" ? "SHA-256 does not match"
                            : d.checksum_status == "computed" ? "SHA-256 calculated"
                                                              : "Could not hash this file. Check "
                                                                "that it exists and is unchanged.";
                        label(c, regular_, app_.tr(status), r.x + 62, y + 62, 11, muted,
                              r.width - 95);
                    }
                } else {
                    if (!d.paused && d.speed > 0) {
                        std::string status = bytes(d.speed) + "/s";
                        if (d.total > d.received) {
                            const auto seconds = (d.total - d.received) / d.speed +
                                                 ((d.total - d.received) % d.speed != 0);
                            status += " · ~" + std::to_string(seconds / 60) + " " + app_.tr("min") +
                                      " " + std::to_string(seconds % 60) + " " + app_.tr("sec");
                        }
                        label(c, regular_, status, r.x + 62, y + 61, 11, muted, r.width - 95);
                    }
                    if (y + 50 <= r.y + r.height - 14) {
                        focus_target({r.x + r.width - 98, y + 5, 36, 36});
                        focus_target({r.x + r.width - 57, y + 5, 36, 36});
                    }
                    icon(c, d.paused || d.interrupted ? "play" : "pause", r.x + r.width - 89,
                         y + 13, 18, muted);
                    icon(c, "close", r.x + r.width - 48, y + 13, 18, muted);
                    fill(c, {r.x + 62, y + 67, r.width - 94, 3}, 2, SkColorSetA(t.accent, 30));
                    fill(c,
                         {r.x + 62, y + 67,
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
        if (panel_ == Workspaces)
            draw_workspaces(c, r);
        if (panel_ == Groups)
            draw_groups(c, r);
        if (panel_ == Containers)
            draw_containers(c, r);
        if (panel_ == Profiles)
            draw_profiles(c, r);
        if (panel_ == Commands)
            draw_commands(c, r);
        if (panel_ == Translation)
            draw_translation(c, r);
        if (panel_ == Permissions)
            draw_permissions(c, r);
        if (panel_ == SiteData)
            draw_site_data(c, r);
        if (panel_ == Sessions)
            draw_sessions(c, r);
        if (panel_ == Hotkeys)
            draw_hotkeys(c, r);
        if (panel_ == DuplicateTabs)
            draw_duplicates(c, r);
        if (panel_ == SpellingDictionary)
            draw_spelling(c, r);
        if (panel_ == ExtensionAccess)
            draw_extension_access(c, r);
        if (panel_ == Diagnostics)
            draw_diagnostics(c, r);
        if (panel_ == SearchProviders)
            draw_search_providers(c, r);
        if (panel_ == ReadingListPanel)
            draw_reading_list(c, r);
        if (panel_ == TaskManager)
            draw_task_manager(c, r);
        if (panel_ == Screenshot)
            draw_screenshot(c, r);
    }

    if (editing) {
        focus_targets_.clear();
        fill(c, {shell_left, 30, width_ - shell_left, height_ - 30}, 0,
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
                  app_.profile.current_search_name(address.value),
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
            {{addr.x + 5, addr.y + 4, 30, 30},
             app_.tr("Site permissions") + (shortcut_hint("permissions").empty()
                                                ? ""
                                                : " (" + shortcut_hint("permissions") + ")")},
            {{260, 37, 34, 36}, app_.tr("Back") + " (Alt+←)"},
            {{298, 37, 34, 36}, app_.tr("Forward") + " (Alt+→)"},
            {{336, 37, 34, 36},
             app_.tr("Reload") +
                 (shortcut_hint("reload").empty() ? "" : " (" + shortcut_hint("reload") + ")")},
            {{width_ - 230, 37, 34, 36}, app_.tr("Protection")},
            {{width_ - 190, 37, 34, 36}, app_.tr("Extensions")},
            {{width_ - 150, 37, 34, 36},
             app_.tr("Bookmarks") +
                 (shortcut_hint("bookmark").empty() ? "" : " (" + shortcut_hint("bookmark") + ")")},
            {{width_ - 110, 37, 34, 36},
             app_.tr("Downloads") + (shortcut_hint("downloads").empty()
                                         ? ""
                                         : " (" + shortcut_hint("downloads") + ")")},
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
        const auto buttons = dialog_buttons();
        focus_target(buttons[0]);
        focus_target(buttons[1]);
        fill(c, {shell_left, 30, width_ - shell_left, height_ - 30}, 0, 0x99050712);
        shadow(c, r, 18);
        fill(c, r, 18, t.surface);
        label(c, medium_,
              dialog.before_unload ? app_.tr("Leave this page?")
              : dialog.owned       ? app_.tr(dialog.origin)
                                   : dialog.origin,
              r.x + 25, r.y + 38, 16, ink, r.width - 50);
        const auto lines = wrap_text(font(regular_, 13),
                                     dialog.before_unload
                                         ? app_.tr("This page may contain unsaved changes.") +
                                               (dialog.context.empty() ? "" : "\n" + dialog.context)
                                     : dialog.owned ? app_.tr(dialog.message)
                                                    : dialog.message,
                                     r.width - 50,
                                     dialog.prompt  ? 2
                                     : dialog.owned ? 8
                                                    : 4);
        for (size_t i = 0; i < lines.size(); ++i)
            label(c, regular_, lines[i], r.x + 25, r.y + 78 + int(i) * 18, 13, muted, r.width - 50);
        if (dialog.prompt) {
            fill(c, {r.x + 25, r.y + 101, r.width - 50, 37}, 8, t.background);
            draw_editor(c, dialog.input, {r.x + 37, r.y + 104, r.width - 74, 31}, 13, true);
        }
        fill(c, buttons[0], 9, SkColorSetA(ink, 14));
        const auto cancel = app_.tr(dialog.permission ? "Deny" : "Cancel");
        centered_label(c, medium_, cancel, buttons[0], 12, ink);
        fill(c, buttons[1], 9, t.accent);
        const auto accept = app_.tr(!dialog.accept_label.empty() ? dialog.accept_label.c_str()
                                    : dialog.permission          ? "Allow"
                                    : dialog.before_unload       ? "Leave"
                                                                 : "OK");
        centered_label(c, medium_, accept, buttons[1], 12, t.background);
    }
    if (app_.screenshot.selecting)
        draw_screenshot_area(c);
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
std::string fold_settings(std::string text) { return fold_text(text); }

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
void BrowserUi::finish_export(const std::string &path) {
    try {
        write_export(path_from_utf8(path), save_payload_);
        last_export_ = path;
        save_error_.clear();
        ++app_.audit["exports_saved"];
        toast(app_.tr("Export saved"));
    } catch (...) {
        save_error_ = app_.tr("Could not save the export. Check the path and permissions.");
        toast(save_error_);
    }
    save_payload_.clear();
    app_.changed();
}
void BrowserUi::save_export(std::string payload, const std::string &filename,
                            const std::string &filter, const std::string &caption) {
#ifndef _WIN32
    if (picker_pid_ > 0)
        return;
#endif
    save_payload_ = std::move(payload);
    save_error_.clear();
    last_export_.clear();
    auto directory = app_.profile.download_directory.empty()
                         ? app_.profile.directory / "downloads"
                         : path_from_utf8(app_.profile.download_directory);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
#ifdef _WIN32
    std::wstring file(32768, L'\0');
    const auto initial = (directory / path_from_utf8(filename)).wstring();
    std::copy(initial.begin(), initial.end(), file.begin());
    const auto title = CefString(app_.tr(caption)).ToWString();
    const auto extension = path_from_utf8(filename).extension().wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = app_.window->handle();
    dialog.lpstrFilter = L"All files\0*.*\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = DWORD(file.size());
    dialog.lpstrTitle = title.c_str();
    dialog.lpstrDefExt = extension.empty() ? nullptr : extension.c_str() + 1;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetSaveFileNameW(&dialog)) {
        file.resize(wcslen(file.c_str()));
        finish_export(utf8_path(std::filesystem::path(file)));
    } else {
        save_payload_.clear();
        if (CommDlgExtendedError()) {
            save_error_ = app_.tr("Could not open the save dialog.");
            toast(save_error_);
        }
    }
#else
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK)) {
        save_payload_.clear();
        save_error_ = app_.tr("Could not open the save dialog.");
        toast(save_error_);
        return;
    }
    std::vector<std::string> args = {
        "env",
        app_.window->backend() == "wayland" ? "GDK_BACKEND=wayland" : "GDK_BACKEND=x11",
        "GSK_RENDERER=cairo",
        "GTK_USE_PORTAL=0",
        "LIBGL_ALWAYS_SOFTWARE=1",
        "zenity",
        "--file-selection",
        "--save",
        "--confirm-overwrite",
        "--modal",
        "--title=" + app_.tr(caption),
        "--file-filter=" + filter,
        "--file-filter=" + app_.tr("All files") + " | *",
        "--filename=" + utf8_path(directory / path_from_utf8(filename))};
    std::vector<char *> argv;
    for (auto &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    pid_t child;
    const int status = posix_spawnp(&child, "env", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (status) {
        close(pipes[0]);
        save_payload_.clear();
        save_error_ = app_.tr("Could not open the save dialog.");
        toast(save_error_);
        return;
    }
    picker_pid_ = child;
    picker_fd_ = pipes[0];
    picker_save_ = true;
    picker_import_ = picker_filters_ = false;
    picker_output_.clear();
    keyboard_focus_ = -1;
#endif
    app_.changed();
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
        app_.window->backend() == "wayland" ? "GDK_BACKEND=wayland" : "GDK_BACKEND=x11",
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
    picker_save_ = false;
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
            if (picker_save_)
                finish_export(picker_output_);
            else if (picker_filters_)
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
        toast(app_.tr(picker_save_      ? "Could not open the save dialog."
                      : picker_filters_ ? "Could not open the file chooser. Add rules manually."
                      : picker_import_
                          ? "Could not open the file chooser. Paste a file path instead."
                          : "Could not open the image chooser. Paste a file path instead."));
    picker_output_.clear();
    save_payload_.clear();
    picker_save_ = false;
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
            app_.changed(true);
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
        {"spelling_dictionary", "Webpages", "Spelling dictionary",
         "Add or remove words. Private additions last for the private session."},
        {"passwords", "Data", "Passwords", "Manage saved logins and encrypted transfer files."},
        {"import", "Data", "Import browser data",
         "Preview bookmarks and history from Firefox, Chrome or an HTML export."},
        {"diagnostics", "General", "Diagnostics",
         "Restart without add-ons or save a report without website addresses."},
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
        description(spec && spec->id == "search_engine" && app_.profile.default_search_provider
                        ? app_.tr("Default search") + ": " + app_.profile.current_search_name()
                        : row.description,
                    b, reserve);
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
                     value == spec->choices[i].value &&
                         !(spec->id == "search_engine" && app_.profile.default_search_provider));
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
                if (row.action == "diagnostics")
                    open_diagnostics();
                else if (row.action == "spelling_dictionary")
                    open_spelling_dictionary();
                else if (row.action == "shortcuts")
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
            if (spec->id == "search_engine")
                app_.profile.default_search_provider = 0;
            if (spec->id == "language")
                toast_.clear();
            app_.apply_settings(spec->id == "default_zoom" || spec->id == "remember_site_zoom");
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
    if (panel_ == TaskManager)
        panel(None);
    if (panel_ == Screenshot || app_.screenshot.pending || app_.screenshot.selecting)
        app_.cancel_screenshot();
    session_input_focus_ = false;
    provider_focus_ = -1;
    reading_focus_ = false;
    spelling_focus_ = false;
    extension_site_focus_ = false;
    data_search_focus_ = false;
    clear_password_fields();
    app_.close_extension_popup();
    editing = true;
    panel_ = None;
    permissions_search_focus_ = false;
    translation_text_.clear();
    translation_source_.clear();
    translation_tab_ = 0;
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
    if (app_.safe_mode() && (value == Filters || value == SpellingDictionary ||
                             value == Screenshot || value == DuplicateTabs)) {
        toast(app_.tr("This tool is paused in safe mode."));
        return;
    }
    if (value != ExtensionAccess) {
        extension_site_input_.set("");
        extension_site_error_.clear();
    }
    if (value != SpellingDictionary) {
        spelling_input_.set("");
        spelling_error_.clear();
    }
    if (panel_ == TaskManager && value != TaskManager) {
        const auto &windows = app_.state()->windows;
        if (std::none_of(windows.begin(), windows.end(), [&](auto *w) {
                return w != &app_ && w->ui && w->ui->visible_panel() == TaskManager;
            }))
            app_.state()->task_manager = nullptr;
    }
    if (value != Hotkeys)
        hotkey_recording_.clear();
    if (value != Screenshot &&
        (panel_ == Screenshot || app_.screenshot.pending || app_.screenshot.selecting))
        app_.cancel_screenshot();
    provider_focus_ = -1;
    reading_focus_ = false;
    spelling_focus_ = false;
    extension_site_focus_ = false;
    if (value != Sessions)
        session_input_focus_ = false;
    hotkey_search_focus_ = false;
    bookmark_focus_ = -1;
    if (value == Bookmarks) {
        bookmark_mode_ = 0;
        bookmark_scroll_ = 0;
        bookmark_error_.clear();
        bookmark_selection_.clear();
        bookmark_search_.set("");
        bookmark_focus_ = 0;
    }

    app_.close_extension_popup();
    popup_buttons_ = 0;
    keyboard_focus_ = -1;
    clear_password_fields();
    password_mode_ = 0;
    panel_ = value;
    if (value != Permissions)
        permissions_search_focus_ = false;
    if (value != SiteData)
        data_search_focus_ = false;
    if (value != Translation) {
        translation_text_.clear();
        translation_source_.clear();
        translation_tab_ = 0;
    }
    if (value != Workspaces)
        workspace_input_focus_ = false;
    if (value != Groups)
        group_input_focus_ = false;
    if (value != Containers)
        container_input_focus_ = false;
    if (value != Profiles)
        profile_input_focus_ = false;
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
    extension_site_input_.set("");
    extension_site_error_.clear();
    spelling_input_.set("");
    spelling_error_.clear();
    if (panel_ == TaskManager)
        panel(None);
    hotkey_recording_.clear();
    if (panel_ == Screenshot || app_.screenshot.pending || app_.screenshot.selecting)
        app_.cancel_screenshot();
    session_input_focus_ = false;
    provider_focus_ = -1;
    reading_focus_ = false;
    spelling_focus_ = false;
    extension_site_focus_ = false;
    data_search_focus_ = false;
    clear_password_fields();
    editing = false;
    bookmark_focus_ = -1;
    panel_ = None;
    permissions_search_focus_ = false;
    translation_text_.clear();
    translation_source_.clear();
    translation_tab_ = 0;
    workspace_input_focus_ = group_input_focus_ = container_input_focus_ = profile_input_focus_ =
        false;
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
            if (panel_ == SearchProviders && provider_focus_ >= 0 &&
                target == &provider_fields_[provider_focus_] &&
                text.size() + target->value.size() - target->selected().size() <=
                    (provider_focus_ == 0   ? 120u
                     : provider_focus_ == 1 ? 16u
                                            : 4096u))
                live = target;
            if (panel_ == ExtensionAccess && extension_site_focus_ &&
                target == &extension_site_input_ &&
                text.size() + target->value.size() - target->selected().size() <= 2048)
                live = target;
            if (panel_ == SpellingDictionary && spelling_focus_ && target == &spelling_input_ &&
                text.size() + target->value.size() - target->selected().size() <= 99)
                live = target;
            if (panel_ == ReadingListPanel && reading_focus_ && target == &reading_search_)
                live = &reading_search_;
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
            else if (panel_ == Workspaces && target == &workspace_input_ && workspace_input_focus_)
                live = &workspace_input_;
            else if (panel_ == Permissions && permissions_picker_ && permissions_search_focus_ &&
                     target == &permissions_search_)
                live = &permissions_search_;
            else if (panel_ == Hotkeys && hotkey_search_focus_ && target == &hotkey_search_ &&
                     text.size() + hotkey_search_.value.size() <= 512)
                live = target;
            else if (panel_ == Sessions && session_input_focus_ && target == &session_name_)
                live = target;
            else if (panel_ == Bookmarks && bookmark_focus_ >= 0 &&
                     target == (bookmark_focus_ == 0   ? &bookmark_search_
                                : bookmark_focus_ == 1 ? &bookmark_title_
                                : bookmark_focus_ == 2 ? &bookmark_url_
                                : bookmark_focus_ == 3 ? &bookmark_tags_
                                                       : &bookmark_folder_name_))
                live = target;
            else if (panel_ == SiteData && data_picker_ && data_search_focus_ &&
                     target == &data_search_)
                live = &data_search_;
            else if (panel_ == Commands && target == &command_query_)
                live = &command_query_;
            else if (panel_ == Profiles && target == &profile_input_ && profile_input_focus_)
                live = &profile_input_;
            else if (panel_ == Containers && target == &container_input_ && container_input_focus_)
                live = &container_input_;
            else if (panel_ == Groups && target == &group_input_ && group_input_focus_)
                live = &group_input_;
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
                (live != &filter_input_ || live->value.size() + text.size() <= 4096) &&
                (live != &workspace_input_ || live->value.size() + text.size() <= 120) &&
                (live != &group_input_ || live->value.size() + text.size() <= 120) &&
                (live != &container_input_ ||
                 live->value.size() + text.size() <= (container_rules_mode_ ? 253u : 120u)) &&
                (live != &permissions_search_ || live->value.size() + text.size() <= 512) &&
                (live != &data_search_ || live->value.size() + text.size() <= 512) &&
                (live != &reading_search_ || live->value.size() + text.size() <= 512) &&
                (panel_ != Bookmarks || live->value.size() + text.size() <= 16384) &&
                (live != &session_name_ || live->value.size() + text.size() <= 4096) &&
                (live != &command_query_ || live->value.size() + text.size() <= 512) &&
                (live != &profile_input_ || live->value.size() + text.size() <= 120))
                live->insert(text);
            if (live == &bookmark_search_)
                bookmark_scroll_ = bookmark_folder_selected_ = 0;
            if (live == &data_search_)
                data_selected_ = data_scroll_ = 0;
            if (live == &permissions_search_)
                permissions_pick_selected_ = permissions_scroll_ = 0;
            if (live == &command_query_)
                command_selected_ = command_scroll_ = 0;
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
bool BrowserUi::key(const CefKeyEvent &incoming) {
    const bool down = incoming.type != KEYEVENT_KEYUP;
    if (down && incoming.windows_key_code == 0x1B && drag_source_) {
        cancel_tab_gesture();
        return true;
    }
    const int code = incoming.windows_key_code;
    const int mods = ((incoming.modifiers & EVENTFLAG_CONTROL_DOWN) ? 1 : 0) |
                     ((incoming.modifiers & EVENTFLAG_SHIFT_DOWN) ? 2 : 0) |
                     ((incoming.modifiers & EVENTFLAG_ALT_DOWN) ? 4 : 0);
    if (panel_ == Hotkeys && !hotkey_recording_.empty() && app_.dialogs.empty()) {
        if (!down)
            return true;
        if (code == 0x1B && mods == 0) {
            hotkey_recording_.clear();
            hotkey_error_.clear();
            app_.changed();
            return true;
        }
        if (code == 0x10 || code == 0x11 || code == 0x12 || code == 0x5B || code == 0x5C)
            return true;
        if (incoming.modifiers & (EVENTFLAG_ALTGR_DOWN | EVENTFLAG_COMMAND_DOWN)) {
            hotkey_error_ = app_.tr("This combination is reserved. Choose another.");
            app_.changed();
            return true;
        }
        save_hotkey(hotkey_recording_, {code, mods});
        return true;
    }
    if (app_.screenshot.selecting || !app_.dialogs.empty() || app_.menu_action ||
        app_.menu_callback)
        return key_default(incoming);
    if (incoming.modifiers & (EVENTFLAG_ALTGR_DOWN | EVENTFLAG_COMMAND_DOWN)) {
        if (auto t = app_.current(); t && t->browser && panel_ == None && !editing)
            t->browser->GetHost()->SendKeyEvent(incoming);
        return false;
    }
    if (panel_ == Hotkeys) {
        if (!down)
            return true;
        if (code == 0x1B) {
            dismiss();
            return true;
        }
        if (mods == 1 && code == 'F') {
            hotkey_search_focus_ = true;
            hotkey_search_.select_all();
            app_.changed();
            return true;
        }
        if (code == 0x21 || code == 0x22 ||
            ((code == 0x24 || code == 0x23) && !hotkey_search_focus_)) {
            const int cap = std::max(1, (panel_bounds().height - 280) / 56),
                      maximum = std::max(0, int(hotkey_results().size()) - cap);
            hotkey_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? maximum
                                          : hotkey_scroll_ + (code == 0x21 ? -cap : cap),
                           0, maximum);
            app_.changed();
            return true;
        }
        if (hotkey_search_focus_ && code == 0x09)
            keyboard_focus_ = 1;
        if (hotkey_search_focus_ && code != 0x09 && code != 0x79) {
            editor_key(hotkey_search_, incoming);
            hotkey_scroll_ = 0;
            app_.changed();
            return code < 0x20 || (mods & 1) || (code >= 0x21 && code <= 0x2E);
        }
    }
    // Remap to the existing context-aware dispatcher: modal/editor/page guards remain intact.
    const Hotkey chord{code, mods};
    for (const auto &spec : hotkey_catalog())
        if (effective_hotkey(app_.profile.hotkeys, spec.id) == chord && code) {
            if (!down)
                return true;
            if (app_.extension_popup)
                app_.close_extension_popup();
            if (std::string_view(spec.id) == "settings") {
                panel(Settings);
                return true;
            }
            if (std::string_view(spec.id) == "task_manager") {
                open_task_manager();
                return true;
            }
            if (std::string_view(spec.id) == "hotkeys") {
                open_hotkeys();
                return true;
            }
            auto mapped = incoming;
            mapped.windows_key_code = spec.standard.key;
            mapped.modifiers &=
                ~(EVENTFLAG_CONTROL_DOWN | EVENTFLAG_SHIFT_DOWN | EVENTFLAG_ALT_DOWN);
            if (spec.standard.modifiers & 1)
                mapped.modifiers |= EVENTFLAG_CONTROL_DOWN;
            if (spec.standard.modifiers & 2)
                mapped.modifiers |= EVENTFLAG_SHIFT_DOWN;
            if (spec.standard.modifiers & 4)
                mapped.modifiers |= EVENTFLAG_ALT_DOWN;
            return key_default(mapped);
        }
    for (const auto &spec : hotkey_catalog())
        if (spec.standard == chord && code)
            return true;
    return key_default(incoming);
}
bool BrowserUi::key_default(const CefKeyEvent &key) {
    const bool down = key.type != KEYEVENT_KEYUP;
    const auto code = key.windows_key_code;
    const bool ctrl = key.modifiers & EVENTFLAG_CONTROL_DOWN,
               shift = key.modifiers & EVENTFLAG_SHIFT_DOWN,
               alt = key.modifiers & EVENTFLAG_ALT_DOWN;
    if (app_.screenshot.selecting) {
        if (!down)
            return true;
        if (code == 0x1B) {
            finish_screenshot_area(false);
            return true;
        }
        if (code == 0x09) {
            screenshot_cancel_focus_ = !screenshot_cancel_focus_;
            app_.changed();
            return true;
        }
        if (code == 0x0D) {
            finish_screenshot_area(!screenshot_cancel_focus_);
            return true;
        }
        if (code >= 0x25 && code <= 0x28) {
            const int step = ctrl ? 1 : 10,
                      dx = code == 0x25   ? -step
                           : code == 0x27 ? step
                                          : 0,
                      dy = code == 0x26   ? -step
                           : code == 0x28 ? step
                                          : 0;
            if (shift) {
                screenshot_region_.width = std::clamp(
                    screenshot_region_.width + dx,
                    std::min(8, screenshot_area_.x + screenshot_area_.width - screenshot_region_.x),
                    screenshot_area_.x + screenshot_area_.width - screenshot_region_.x);
                screenshot_region_.height =
                    std::clamp(screenshot_region_.height + dy,
                               std::min(8, screenshot_area_.y + screenshot_area_.height -
                                               screenshot_region_.y),
                               screenshot_area_.y + screenshot_area_.height - screenshot_region_.y);
            } else {
                screenshot_region_.x = std::clamp(screenshot_region_.x + dx, screenshot_area_.x,
                                                  screenshot_area_.x + screenshot_area_.width -
                                                      screenshot_region_.width);
                screenshot_region_.y = std::clamp(screenshot_region_.y + dy, screenshot_area_.y,
                                                  screenshot_area_.y + screenshot_area_.height -
                                                      screenshot_region_.height);
            }
            screenshot_cancel_focus_ = false;
            app_.changed();
            return true;
        }
        return true;
    }
    if (down && ctrl && shift && code == 'S' && app_.dialogs.empty() && !editing) {
        open_screenshot();
        return true;
    }
    if (ctrl && shift && code == '0') {
        if (down) {
            app_.profile.ui_zoom = 100;
            app_.apply_settings();
        }
        return true;
    }
    if (down)
        last_key_ = key;
    if (down && ctrl && !alt && code == 'K' && app_.dialogs.empty()) {
        open_commands();
        return true;
    }
    if (app_.dialogs.empty() && (app_.menu_action || app_.menu_callback)) {
        if (!down)
            return true;
        if (code == 0x1B) {
            app_.close_menu();
            keyboard_focus_ = -1;
            app_.changed();
            return true;
        }
        std::vector<int> enabled;
        for (int i = 0; i < int(app_.menu.size()); ++i)
            if (app_.menu[i].enabled && !app_.menu[i].label.empty())
                enabled.push_back(i);
        if (code == 0x26 || code == 0x28 || code == 0x09 || code == 0x24 || code == 0x23) {
            if (!enabled.empty()) {
                const int n = int(enabled.size()),
                          direction = code == 0x26 || (code == 0x09 && shift) ? -1 : 1;
                keyboard_focus_ = code == 0x24          ? 0
                                  : code == 0x23        ? n - 1
                                  : keyboard_focus_ < 0 ? (direction < 0 ? n - 1 : 0)
                                                        : (keyboard_focus_ + direction + n) % n;
                app_.changed();
            }
            return true;
        }
        if ((code == 0x0D || code == 0x20) && !enabled.empty()) {
            const auto item =
                app_.menu[enabled[std::clamp(keyboard_focus_, 0, int(enabled.size()) - 1)]];
            auto action = std::move(app_.menu_action);
            auto callback = std::move(app_.menu_callback);
            app_.menu.clear();
            keyboard_focus_ = -1;
            if (action)
                action(item.id);
            else if (callback)
                callback->Continue(item.id, static_cast<cef_event_flags_t>(key.modifiers));
            app_.changed();
            return true;
        }
        return true;
    }
    if (panel_ == Commands && app_.dialogs.empty() && !app_.menu_action && !app_.menu_callback) {
        if (!down)
            return true;
        if (code == 0x1B) {
            dismiss();
            command_query_.set("");
            return true;
        }
        if (code == 0x0D) {
            run_command(command_selected_);
            return true;
        }
        if (code == 0x26 || code == 0x28 || code == 0x09 || code == 0x21 || code == 0x22) {
            const int direction = code == 0x26 || code == 0x21 || (code == 0x09 && shift) ? -1 : 1;
            move_command(direction * (code == 0x21 || code == 0x22
                                          ? std::max(1, (panel_bounds().height - 190) / 52)
                                          : 1));
            return true;
        }
        if (ctrl && (code == 'L' || code == 'T' || code == 'W' || code == 'Q'))
            dismiss();
        else {
            editor_key(command_query_, key);
            command_selected_ = command_scroll_ = 0;
            app_.changed();
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
    }
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
        workspace_input_focus_ = false;
        group_input_focus_ = false;
        container_input_focus_ = false;
        profile_input_focus_ = false;
        permissions_search_focus_ = false;
        data_search_focus_ = false;
        bookmark_focus_ = -1;
        session_input_focus_ = false;
        provider_focus_ = -1;
        reading_focus_ = false;
        spelling_focus_ = false;
        extension_site_focus_ = false;
        hotkey_search_focus_ = false;
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
        password_focus_ < 0 && !workspace_input_focus_ && !group_input_focus_ &&
        !container_input_focus_ && !profile_input_focus_ && !permissions_search_focus_ &&
        !data_search_focus_ && bookmark_focus_ < 0 && !session_input_focus_ && !reading_focus_ &&
        !hotkey_search_focus_ && provider_focus_ < 0 && !spelling_focus_ &&
        !extension_site_focus_ && !editing) {
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
        // Keep focus on disclosures and scroll arrows as their rows change.
        const bool stable_tab_control =
            previous_panel == None &&
            ((!horizontal_tabs() && app_.profile.tree_tabs && r.x >= 20 && r.x <= 56 &&
              r.width == 24 && r.height == 32) ||
             (horizontal_tabs() &&
              ((app_.profile.tree_tabs && r.y == 4 && r.width == 20 && r.height == 24) ||
               (r.y == 2 && r.width == 28 && r.height == 28))));
        if ((panel_ != None || stable_tab_control) && panel_ == previous_panel)
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
        if (ctrl && alt && code == 'E' && app_.dialogs.empty() && !editing) {
            open_sessions();
            return true;
        }
        if (ctrl && alt && code == 'D' && app_.dialogs.empty() && !editing) {
            open_site_data();
            return true;
        }
        if (ctrl && alt && code == 'I' && app_.dialogs.empty() && !editing) {
            open_permissions();
            return true;
        }
        if (ctrl && alt && code == 'T' && app_.dialogs.empty() && !editing) {
            open_translation(app_.current() && !app_.current()->selected_text.empty());
            return true;
        }
        if (ctrl && alt && code == 'R' && app_.dialogs.empty() && panel_ == None && !editing) {
            app_.toggle_reader();
            return true;
        }
        if (ctrl && alt && code == 'P' && app_.dialogs.empty()) {
            open_profiles();
            return true;
        }
        if (ctrl && alt && code == 'C' && app_.dialogs.empty()) {
            open_containers();
            return true;
        }
        if (ctrl && alt && code == 'V' && app_.dialogs.empty() && panel_ == None && !editing) {
            app_.toggle_split();
            return true;
        }
        if (ctrl && alt && code == 'S' && app_.dialogs.empty() && panel_ == None && !editing) {
            if (auto t = app_.current())
                app_.sleep_tab(t->id);
            return true;
        }
        if (ctrl && alt && code == 'G' && app_.dialogs.empty()) {
            open_groups();
            return true;
        }
        if (ctrl && shift && code == 'E' && app_.dialogs.empty()) {
            open_workspaces();
            return true;
        }
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
        if (panel_ == DuplicateTabs && code == 0x1B && duplicate_selected_) {
            duplicate_selected_.reset();
            duplicate_scroll_ = 0;
            app_.changed();
            return true;
        }
        if (panel_ == DuplicateTabs &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            int cap = std::max(1, (panel_bounds().height - 232) / 92);
            int count =
                duplicate_selected_ ? duplicate_selected_->tabs.size() : duplicate_groups_.size();
            int maximum = std::max(0, count - cap);
            duplicate_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? maximum
                                          : duplicate_scroll_ + (code == 0x21 ? -cap : cap),
                           0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == ExtensionAccess && extension_site_focus_ &&
            (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q' && code != 'N'))) {
            if (code == 0x1B) {
                dismiss();
                return true;
            }
            if (code == 0x0D) {
                add_extension_site();
                return true;
            }
            editor_key(extension_site_input_, key);
            extension_site_error_.clear();
            app_.changed();
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
        if (panel_ == ExtensionAccess &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const auto &a = app_.state()->extension_access.snapshot();
            const int cap = std::max(1, (panel_bounds().height - 420) / 36),
                      last = std::max(0, int(a.hosts.size()) - cap);
            extension_site_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? last
                                          : extension_site_scroll_ + (code == 0x21 ? -cap : cap),
                           0, last);
            app_.changed();
            return true;
        }
        if (panel_ == SpellingDictionary && spelling_focus_ &&
            (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q' && code != 'N'))) {
            if (code == 0x1B) {
                dismiss();
                return true;
            }
            if (code == 0x0D) {
                save_spelling_word();
                return true;
            }
            editor_key(spelling_input_, key);
            spelling_scroll_ = 0;
            spelling_error_.clear();
            app_.changed();
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
        if (panel_ == SpellingDictionary &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int cap = std::max(1, (panel_bounds().height - 252) / 40),
                      max = std::max(0, int(spelling_results().size()) - cap);
            spelling_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? max
                                          : spelling_scroll_ + (code == 0x21 ? -cap : cap),
                           0, max);
            app_.changed();
            return true;
        }
        if (panel_ == SearchProviders && provider_focus_ >= 0 &&
            (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q' && code != 'N'))) {
            if (code == 0x1B) {
                provider_editing_ = false;
                provider_focus_ = -1;
                provider_error_.clear();
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                save_search_provider();
                return true;
            }
            editor_key(provider_fields_[provider_focus_], key);
            app_.changed();
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
        if (panel_ == SearchProviders && !provider_editing_ &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            int cap = std::max(1, (panel_bounds().height - 200) / 88);
            int max = std::max(0, int(app_.profile.search_providers.size()) - cap);
            provider_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? max
                                          : provider_scroll_ + (code == 0x21 ? -cap : cap),
                           0, max);
            app_.changed();
            return true;
        }
        if (panel_ == ReadingListPanel && reading_focus_ &&
            (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q' && code != 'N'))) {
            if (code == 0x1B) {
                dismiss();
                return true;
            }
            editor_key(reading_search_, key);
            reading_scroll_ = 0;
            app_.changed();
            return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
        }
        if (panel_ == ReadingListPanel &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            int cap = std::max(1, (panel_bounds().height - 228) / 104);
            int maximum = std::max(0, int(reading_results().size()) - cap);
            reading_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? maximum
                                          : reading_scroll_ + (code == 0x21 ? -cap : cap),
                           0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == Sessions && session_input_focus_) {
            if (code == 0x1B) {
                if (session_edit_) {
                    session_edit_ = 0;
                    session_name_.set("");
                    session_error_.clear();
                    app_.changed();
                } else
                    dismiss();
                return true;
            }
            if (code == 0x0D) {
                save_named_session();
                return true;
            }
            if (!ctrl ||
                (code != 'L' && code != 'T' && code != 'W' && code != 'Q' && code != 'N')) {
                editor_key(session_name_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Sessions && (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int cap = std::max(1, (panel_bounds().height - 268) / 90),
                      maximum = std::max(0, int(app_.profile.named_sessions.size()) - cap);
            session_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? maximum
                                          : session_scroll_ + (code == 0x21 ? -cap : cap),
                           0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == Bookmarks) {
            if (code == 0x1B && bookmark_mode_ == 0) {
                dismiss();
                return true;
            }
            if (ctrl && code == 'F') {
                bookmark_mode_ = 0;
                bookmark_focus_ = 0;
                bookmark_search_.select_all();
                app_.changed();
                return true;
            }
            if (code == 0x1B && bookmark_mode_ == 5) {
                choose_bookmark_folder(-999);
                return true;
            }
            if (code == 0x1B && bookmark_mode_) {
                bookmark_mode_ = 0;
                bookmark_focus_ = -1;
                bookmark_error_.clear();
                app_.changed();
                return true;
            }
            if (bookmark_mode_ == 5 && bookmark_focus_ == 0 &&
                (code == 0x26 || code == 0x28 || code == 0x21 || code == 0x22)) {
                const int n = int(bookmark_folder_results().size()),
                          cap = std::max(1, (panel_bounds().height - 236) / 44);
                bookmark_folder_selected_ = std::clamp(
                    bookmark_folder_selected_ + (code == 0x26 || code == 0x21 ? -1 : 1) *
                                                    (code == 0x21 || code == 0x22 ? cap : 1),
                    0, std::max(0, n - 1));
                bookmark_scroll_ =
                    std::clamp(bookmark_scroll_, std::max(0, bookmark_folder_selected_ - cap + 1),
                               bookmark_folder_selected_);
                app_.changed();
                return true;
            }
            if (code == 0x0D && bookmark_focus_ >= 0) {
                if (bookmark_mode_ == 5) {
                    const auto folders = bookmark_folder_results();
                    if (!folders.empty())
                        choose_bookmark_folder(folders[std::clamp(bookmark_folder_selected_, 0,
                                                                  int(folders.size()) - 1)]);
                } else if (bookmark_mode_)
                    save_bookmark_edit();
                return true;
            }
            if (bookmark_focus_ >= 0 && (!ctrl || (code != 'L' && code != 'T' && code != 'W' &&
                                                   code != 'Q' && code != 'N' && code != 'B'))) {
                auto &editor = bookmark_focus_ == 0   ? bookmark_search_
                               : bookmark_focus_ == 1 ? bookmark_title_
                               : bookmark_focus_ == 2 ? bookmark_url_
                               : bookmark_focus_ == 3 ? bookmark_tags_
                                                      : bookmark_folder_name_;
                editor_key(editor, key);
                bookmark_scroll_ = bookmark_folder_selected_ = 0;
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
            if (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23) {
                const int n = int(bookmark_results().size()),
                          cap = std::max(1, (panel_bounds().height - 320) / 52),
                          maximum = std::max(0, n - cap);
                bookmark_scroll_ =
                    std::clamp(code == 0x24   ? 0
                               : code == 0x23 ? maximum
                                              : bookmark_scroll_ + (code == 0x21 ? -cap : cap),
                               0, maximum);
                app_.changed();
                return true;
            }
        }
        if (panel_ == SiteData && ctrl && code == 'F') {
            data_picker_ = true;
            data_search_focus_ = true;
            data_search_.select_all();
            data_selected_ = data_scroll_ = 0;
            app_.changed();
            return true;
        }
        if (panel_ == SiteData && data_picker_ && data_search_focus_) {
            const auto sites = data_sites();
            const int capacity = std::max(1, (panel_bounds().height - 230) / 52),
                      n = int(sites.size());
            if (code == 0x1B) {
                data_picker_ = false;
                data_search_focus_ = false;
                app_.changed();
                return true;
            }
            if (code == 0x26 || code == 0x28 || code == 0x21 || code == 0x22) {
                data_selected_ =
                    std::clamp(data_selected_ + (code == 0x26 || code == 0x21 ? -1 : 1) *
                                                    (code == 0x21 || code == 0x22 ? capacity : 1),
                               0, std::max(0, n - 1));
                data_scroll_ = std::clamp(data_scroll_, std::max(0, data_selected_ - capacity + 1),
                                          data_selected_);
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                if (n) {
                    data_picker_ = false;
                    data_search_focus_ = false;
                    app_.site_data.message.clear();
                    app_.refresh_site_data(sites[std::clamp(data_selected_, 0, n - 1)]);
                }
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(data_search_, key);
                data_selected_ = data_scroll_ = 0;
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Permissions && ctrl && code == 'F') {
            permissions_picker_ = true;
            permissions_search_focus_ = true;
            permissions_search_.select_all();
            permissions_scroll_ = 0;
            app_.changed();
            return true;
        }
        if (panel_ == Permissions && permissions_picker_ && permissions_search_focus_) {
            if (code == 0x26 || code == 0x28 || code == 0x21 || code == 0x22) {
                const int count = int(permission_sites().size()),
                          capacity = std::max(1, (panel_bounds().height - 274) / 52);
                const int direction = code == 0x26 || code == 0x21 ? -1 : 1;
                permissions_pick_selected_ =
                    std::clamp(permissions_pick_selected_ +
                                   direction * (code == 0x21 || code == 0x22 ? capacity : 1),
                               0, std::max(0, count - 1));
                permissions_scroll_ = std::clamp(
                    permissions_scroll_, std::max(0, permissions_pick_selected_ - capacity + 1),
                    permissions_pick_selected_);
                app_.changed();
                return true;
            }
            if (code == 0x1B) {
                permissions_picker_ = false;
                permissions_search_focus_ = false;
                permissions_scroll_ = 0;
                app_.changed();
                return true;
            }
            if (code == 0x0D) {
                const auto sites = permission_sites();
                if (!sites.empty()) {
                    const auto &site =
                        sites[std::clamp(permissions_pick_selected_, 0, int(sites.size()) - 1)];
                    permissions_origin_ = site.origin;
                    permissions_container_ = site.container;
                    permissions_picker_ = false;
                    permissions_search_focus_ = false;
                    permissions_scroll_ = 0;
                    app_.changed();
                }
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(permissions_search_, key);
                permissions_pick_selected_ = permissions_scroll_ = 0;
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Permissions &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int count = permissions_picker_ ? int(permission_sites().size())
                                                  : int(permission_catalog().size());
            const int capacity = std::max(1, (panel_bounds().height - 274) / 52),
                      maximum = std::max(0, count - capacity);
            permissions_scroll_ = std::clamp(
                code == 0x24   ? 0
                : code == 0x23 ? maximum
                               : permissions_scroll_ + (code == 0x22 ? capacity : -capacity),
                0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == Profiles && profile_input_focus_) {
            if (code == 0x0D) {
                save_profile();
                return true;
            }
            if (code == 0x1B) {
                profile_input_focus_ = false;
                app_.changed();
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(profile_input_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Profiles && !profile_input_focus_ &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int capacity = std::max(1, (panel_bounds().height - 236) / 84),
                      maximum = std::max(0, int(listed_profiles().size()) - capacity);
            profiles_scroll_ = std::clamp(
                code == 0x24   ? 0
                : code == 0x23 ? maximum
                               : profiles_scroll_ + (code == 0x22 ? capacity : -capacity),
                0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == TaskManager &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int cap = std::max(1, (panel_bounds().height - 194) / 82);
            const int maximum = std::max(0, int(process_rows_.size()) - cap);
            process_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? maximum
                                          : process_scroll_ + (code == 0x21 ? -cap : cap),
                           0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == Containers && container_input_focus_) {
            if (code == 0x0D) {
                save_container();
                return true;
            }
            if (code == 0x1B) {
                container_input_focus_ = false;
                app_.changed();
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(container_input_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Containers && !container_input_focus_ &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int capacity = std::max(1, (panel_bounds().height - 260) / 88);
            const int maximum =
                std::max(0, int(container_rules_mode_ ? app_.profile.container_rules.size()
                                                      : app_.profile.containers.size()) -
                                capacity);
            container_scroll_ = std::clamp(
                code == 0x24   ? 0
                : code == 0x23 ? maximum
                               : container_scroll_ + (code == 0x22 ? capacity : -capacity),
                0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == Groups && group_input_focus_) {
            if (code == 0x0D) {
                save_group();
                return true;
            }
            if (code == 0x1B) {
                group_input_focus_ = false;
                group_edit_ = -1;
                app_.changed();
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(group_input_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Groups && !group_input_focus_ &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            int count = 0;
            for (const auto &g : app_.profile.groups)
                if (g.workspace == app_.workspace)
                    ++count;
            const int capacity = std::max(1, (panel_bounds().height - 260) / 88),
                      maximum = std::max(0, count - capacity);
            group_scroll_ =
                std::clamp(code == 0x24   ? 0
                           : code == 0x23 ? maximum
                                          : group_scroll_ + (code == 0x22 ? capacity : -capacity),
                           0, maximum);
            app_.changed();
            return true;
        }
        if (panel_ == Workspaces && workspace_input_focus_) {
            if (code == 0x0D) {
                save_workspace();
                return true;
            }
            if (code == 0x1B) {
                workspace_input_focus_ = false;
                workspace_edit_ = -1;
                app_.changed();
                return true;
            }
            if (!ctrl || (code != 'L' && code != 'T' && code != 'W' && code != 'Q')) {
                editor_key(workspace_input_, key);
                app_.changed();
                return code < 0x20 || ctrl || (code >= 0x21 && code <= 0x2E);
            }
        }
        if (panel_ == Workspaces && !workspace_input_focus_ &&
            (code == 0x21 || code == 0x22 || code == 0x24 || code == 0x23)) {
            const int capacity = std::max(1, (panel_bounds().height - 218) / 52);
            const int maximum = std::max(0, int(app_.profile.workspaces.size()) - capacity);
            workspace_scroll_ = std::clamp(
                code == 0x24   ? 0
                : code == 0x23 ? maximum
                               : workspace_scroll_ + (code == 0x22 ? capacity : -capacity),
                0, maximum);
            app_.changed();
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
            app_.cycle_tab(shift ? -1 : 1);
            return true;
        }
        if (ctrl && code >= '1' && code <= '9') {
            app_.select_visible_tab(code - '0');
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
        if (ctrl && code == 'C' && panel_ == None && !editing && !find_open) {
            if (auto tab = app_.current();
                tab && tab->reader_active && !tab->selected_text.empty()) {
                app_.window->copy(tab->selected_text);
                return true;
            }
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
            if (panel_ == None && !editing && !find_open && app_.current() &&
                app_.current()->reader_active) {
                app_.toggle_reader();
                return true;
            }
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
                              : panel_ == Downloads ? int(app_.visible_downloads().size())
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
    if (auto t = app_.current(); t && t->browser && !t->ntp) {
        if (down && ctrl && !alt && app_.window->backend() == "wayland") {
            if (code == 'C' || code == 'X') {
                if (t->client)
                    static_cast<BrowserClient *>(t->client.get())->native_copy(code == 'X');
                return true;
            }
            if (code == 'V') {
                const int id = t->id;
                const auto navigation = t->navigation_serial;
                std::weak_ptr<void> weak = app_.callback_lifetime;
                app_.window->paste([this, weak, id, navigation](std::string text) {
                    if (weak.expired() || text.empty())
                        return;
                    auto tab = app_.tab(id);
                    if (tab && tab == app_.current() && tab->browser &&
                        tab->navigation_serial == navigation && !editing && panel_ == None &&
                        app_.dialogs.empty())
                        tab->browser->GetHost()->ImeCommitText(text, CefRange(-1, -1), 0);
                });
                return true;
            }
        }
        t->browser->GetHost()->SendKeyEvent(key);
    }
    return false;
}
void BrowserUi::text(const std::string &value) {
    if (app_.screenshot.selecting || !hotkey_recording_.empty())
        return;
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
    } else if (panel_ == Hotkeys && hotkey_search_focus_) {
        if (hotkey_search_.value.size() + clean.size() <= 512)
            hotkey_search_.insert(clean);
        hotkey_scroll_ = 0;
    } else if (panel_ == ExtensionAccess && extension_site_focus_) {
        if (extension_site_input_.value.size() - extension_site_input_.selected().size() +
                clean.size() <=
            2048)
            extension_site_input_.insert(clean);
        extension_site_error_.clear();
    } else if (panel_ == SpellingDictionary && spelling_focus_) {
        if (spelling_input_.value.size() - spelling_input_.selected().size() + clean.size() <= 99)
            spelling_input_.insert(clean);
        spelling_scroll_ = 0;
        spelling_error_.clear();
    } else if (panel_ == SearchProviders && provider_focus_ >= 0) {
        auto &field = provider_fields_[provider_focus_];
        if (field.value.size() - field.selected().size() + clean.size() <=
            (provider_focus_ == 0   ? 120u
             : provider_focus_ == 1 ? 16u
                                    : 4096u))
            field.insert(clean);
    } else if (panel_ == ReadingListPanel && reading_focus_) {
        if (reading_search_.value.size() + clean.size() <= 512)
            reading_search_.insert(clean);
        reading_scroll_ = 0;
    } else if (panel_ == Sessions && session_input_focus_) {
        if (session_name_.value.size() + clean.size() <= 4096)
            session_name_.insert(clean);
    } else if (panel_ == Bookmarks && bookmark_focus_ >= 0) {
        auto &editor = bookmark_focus_ == 0   ? bookmark_search_
                       : bookmark_focus_ == 1 ? bookmark_title_
                       : bookmark_focus_ == 2 ? bookmark_url_
                       : bookmark_focus_ == 3 ? bookmark_tags_
                                              : bookmark_folder_name_;
        if (editor.value.size() + clean.size() <= 16384)
            editor.insert(clean);
        bookmark_scroll_ = bookmark_folder_selected_ = 0;
    } else if (panel_ == SiteData && data_picker_ && data_search_focus_) {
        if (data_search_.value.size() + clean.size() <= 512)
            data_search_.insert(clean);
        data_selected_ = data_scroll_ = 0;
    } else if (panel_ == Permissions && permissions_picker_ && permissions_search_focus_) {
        if (permissions_search_.value.size() + clean.size() <= 512)
            permissions_search_.insert(clean);
        permissions_pick_selected_ = permissions_scroll_ = 0;
    } else if (panel_ == Commands) {
        if (command_query_.value.size() + clean.size() <= 512)
            command_query_.insert(clean);
        command_selected_ = command_scroll_ = 0;
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
    } else if (panel_ == Profiles) {
        if (profile_input_focus_ && profile_input_.value.size() + clean.size() <= 120)
            profile_input_.insert(clean);
    } else if (panel_ == Containers) {
        if (container_input_focus_ &&
            container_input_.value.size() + clean.size() <= (container_rules_mode_ ? 253u : 120u))
            container_input_.insert(clean);
    } else if (panel_ == Groups) {
        if (group_input_focus_ && group_input_.value.size() + clean.size() <= 120)
            group_input_.insert(clean);
    } else if (panel_ == Workspaces) {
        if (workspace_input_focus_ && workspace_input_.value.size() + clean.size() <= 120)
            workspace_input_.insert(clean);
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
    if (panel_ == ExtensionAccess) {
        const auto it = std::find_if(app_.extensions.items.begin(), app_.extensions.items.end(),
                                     [&](const auto &e) { return e.id == extension_access_id_; });
        if (it == app_.extensions.items.end() || !it->enabled || !it->load_requested ||
            it->pending_update || app_.private_window ||
            (app_.current() && (app_.current()->private_tab || app_.current()->container))) {
            panel(Extensions);
            return true;
        }
    }
    if (panel_ == ExtensionAccess &&
        extension_site_revision_ != app_.state()->extension_access.snapshot().revision) {
        extension_site_revision_ = app_.state()->extension_access.snapshot().revision;
        return true;
    }
    if (panel_ == SpellingDictionary && spelling_private_ && !app_.private_window &&
        (!app_.current() || !app_.current()->private_tab)) {
        panel(None);
        return true;
    }
    if (strip_hint_id_ && !strip_hint_ready_ &&
        std::chrono::steady_clock::now() >= strip_hint_at_) {
        strip_hint_ready_ = true;
        app_.changed();
    }
    if (panel_ == DuplicateTabs && std::chrono::steady_clock::now() >= duplicate_refresh_) {
        refresh_duplicates();
        app_.changed();
    }
    if (panel_ == TaskManager && std::chrono::steady_clock::now() >= process_refresh_) {
        refresh_task_manager();
        app_.changed();
    }
    auto &shot = app_.screenshot;
    if (shot.native_running) {
        const auto job = app_.tab(shot.job_tab);
        if (!job || !job->browser) {
            shot.native_running = false;
            shot.pending = false;
            ++shot.serial;
        }
    }
    if (shot.pending &&
        std::chrono::steady_clock::now() - shot.started > std::chrono::seconds(20)) {
        ++shot.serial;
        shot.pending = false;
        shot.error = app_.tr("Capture timed out. Wait for it to finish or close this tab.");
        app_.changed();
    }
    if (shot.selecting) {
        const auto source = app_.tab(shot.tab);
        if (!source || !source->browser || source->navigation_serial != shot.navigation ||
            source->page.url != shot.source || !app_.tab_visible(shot.tab) ||
            source->page.view_width != shot.area_view_width ||
            source->page.view_height != shot.area_view_height) {
            finish_screenshot_area(false);
            shot.error = app_.tr("The page view changed. Select the area again.");
            app_.changed();
        }
    }

    if (panel_ == SiteData) {
        const auto &d = app_.site_data;
        const auto source = app_.tab(d.tab);
        const auto error = app_.tr("Open a website in this context and reopen Site data.");
        if ((!source || source->closing || !source->browser || source->container != d.container ||
             source->private_tab != d.private_mode) &&
            d.error != error) {
            ++app_.site_data.serial;
            app_.site_data.pending = 0;
            app_.site_data.clearing = false;
            app_.site_data.error = error;
            app_.changed();
        }
    }

    if ((app_.site_data.pending || app_.site_data.clearing) &&
        std::chrono::steady_clock::now() - app_.site_data.started > std::chrono::seconds(20)) {
        ++app_.site_data.serial;
        app_.site_data.pending = 0;
        app_.site_data.clearing = false;
        app_.site_data.error = app_.tr("Site data request timed out. Reopen this panel to retry.");
        app_.changed();
    }
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
    if (app_.screenshot.selecting) {
        screenshot_area_pointer(input);
        return;
    }
    if (!app_.split_enabled())
        split_drag_ = false;
    const int x = input.point.x, y = input.point.y;
    auto web = web_bounds();
    auto t = app_.current();
    const bool web_only = t && !t->ntp && inside(web, x, y) && inside(web, hover_x_, hover_y_) &&
                          panel_ == None && !editing && !find_open && drag_tab_ < 0 &&
                          keyboard_focus_ < 0 && !app_.extension_popup && app_.dialogs.empty() &&
                          !app_.menu_callback && !app_.menu_action;
    hover_x_ = x;
    hover_y_ = y;
    if (horizontal_tabs() && input.kind == PointerInput::Move) {
        int id = 0;
        if (y >= 2 && y < 30) {
            const auto rows = sidebar_rows();
            for (const auto &slot : horizontal_layout().slots)
                if (inside(slot.bounds, x, y)) {
                    const int index = rows[slot.position];
                    id = index < 0 ? index : app_.tabs[index]->id;
                    break;
                }
        }
        if (id != strip_hint_id_) {
            strip_hint_id_ = id;
            strip_hint_ready_ = false;
            strip_hint_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
    } else if (input.kind != PointerInput::Move) {
        strip_hint_id_ = 0;
        strip_hint_ready_ = false;
    }
    // CEF owns hover and wheel changes inside a page. Its paint callback will
    // invalidate content; the shell does not need a full redraw per input event.
    if (!web_only || (input.kind != PointerInput::Wheel && input.kind != PointerInput::Move))
        app_.changed();
    const bool down = input.kind == PointerInput::Down;
    if (down && (preedit_browser_ || !preedit_.empty()))
        composition("", 0, 0);
    if (down || input.kind == PointerInput::Wheel)
        keyboard_focus_ = -1;
    if (down) {
        drag_tab_ = -1;
        drag_source_ = 0;
        drag_moved_ = drag_cancelled_ = false;
        app_.cancel_tab_drag();
    }
    if (input.kind == PointerInput::Up) {
        const bool tab_gesture = drag_source_ != 0;
        if (tab_gesture && drag_moved_ && !drag_cancelled_)
            app_.tab_drag(drag_source_, x, y, true);
        app_.cancel_tab_drag();
        drag_tab_ = -1;
        drag_source_ = 0;
        if (tab_gesture)
            return; // A page never received the matching mouse-down.
    }
    if (input.kind == PointerInput::Move && drag_source_) {
        if (drag_cancelled_)
            return;
        drag_moved_ = drag_moved_ || std::abs(x - drag_x_) > 8 || std::abs(y - drag_y_) > 8;
        if (drag_moved_)
            app_.tab_drag(drag_source_, x, y);
        drag_tab_ = -1;
        for (int i = 0; i < int(app_.tabs.size()); ++i)
            if (app_.tabs[i]->id == drag_source_ && !app_.tabs[i]->closing &&
                !app_.tabs[i]->page.closed)
                drag_tab_ = i;
        // Keep UI drags out of CEF, even if the source disappeared while held.
        if (drag_tab_ < 0 ||
            (horizontal_tabs() ? (y < 0 || y >= 32) : (x >= 240 || y < 178 || y >= height_ - 220)))
            return;
    }
    if (app_.extension_popup && app_.dialogs.empty() && !app_.menu_callback && !app_.menu_action &&
        popup_pointer(input))
        return;
    if (!horizontal_tabs() && input.kind == PointerInput::Move && drag_tab_ >= 0 &&
        std::abs(y - drag_y_) > 18) {
        if (x >= 240 || y < 178 || y >= height_ - 220)
            return;
        const auto visible = sidebar_rows();
        if (visible.empty())
            return;
        const int index =
            visible[std::clamp((y - 178) / 52 + tab_scroll_, 0, int(visible.size()) - 1)];
        if (index < 0) {
            app_.set_tab_group(app_.tabs[drag_tab_]->id, -index, true);
            return;
        }
        const int dragged = app_.tabs[drag_tab_]->id;
        if (!app_.profile.tree_tabs)
            app_.set_tab_group(dragged, app_.tabs[index]->group);
        app_.move_tab(drag_tab_, index);
        for (int i = 0; i < int(app_.tabs.size()); ++i)
            if (app_.tabs[i]->id == dragged)
                drag_tab_ = i;
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
    if (input.kind == PointerInput::Wheel && panel_ == TaskManager) {
        const int cap = std::max(1, (panel_bounds().height - 194) / 82);
        process_scroll_ = std::clamp(process_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                     std::max(0, int(process_rows_.size()) - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Hotkeys) {
        const int cap = std::max(1, (panel_bounds().height - 280) / 56),
                  maximum = std::max(0, int(hotkey_results().size()) - cap);
        hotkey_scroll_ = std::clamp(hotkey_scroll_ + (input.dy < 0 ? 1 : -1), 0, maximum);
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == DuplicateTabs) {
        int cap = std::max(1, (panel_bounds().height - 232) / 92);
        int count =
            duplicate_selected_ ? duplicate_selected_->tabs.size() : duplicate_groups_.size();
        duplicate_scroll_ =
            std::clamp(duplicate_scroll_ + (input.dy < 0 ? 1 : -1), 0, std::max(0, count - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == SearchProviders) {
        int cap = std::max(1, (panel_bounds().height - 200) / 88);
        provider_scroll_ = std::clamp(provider_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                      std::max(0, int(app_.profile.search_providers.size()) - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == ExtensionAccess) {
        const auto &a = app_.state()->extension_access.snapshot();
        const int cap = std::max(1, (panel_bounds().height - 420) / 36);
        extension_site_scroll_ = std::clamp(extension_site_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                            std::max(0, int(a.hosts.size()) - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == SpellingDictionary) {
        int cap = std::max(1, (panel_bounds().height - 252) / 40);
        spelling_scroll_ = std::clamp(spelling_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                      std::max(0, int(spelling_results().size()) - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == ReadingListPanel) {
        int cap = std::max(1, (panel_bounds().height - 228) / 104);
        reading_scroll_ = std::clamp(reading_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                     std::max(0, int(reading_results().size()) - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Sessions) {
        const int cap = std::max(1, (panel_bounds().height - 268) / 90);
        session_scroll_ = std::clamp(session_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                     std::max(0, int(app_.profile.named_sessions.size()) - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Bookmarks) {
        const int n = bookmark_mode_ == 5 ? int(bookmark_folder_results().size())
                                          : int(bookmark_results().size());
        const int cap = std::max(1, (panel_bounds().height - (bookmark_mode_ == 5 ? 236 : 320)) /
                                        (bookmark_mode_ == 5 ? 44 : 52));
        bookmark_scroll_ =
            std::clamp(bookmark_scroll_ + (input.dy < 0 ? 1 : -1), 0, std::max(0, n - cap));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == SiteData && data_picker_) {
        const int count = int(data_sites().size()),
                  capacity = std::max(1, (panel_bounds().height - 230) / 52);
        data_scroll_ =
            std::clamp(data_scroll_ + (input.dy < 0 ? 1 : -1), 0, std::max(0, count - capacity));
        if (count)
            data_selected_ = std::clamp(data_selected_, data_scroll_,
                                        std::min(count - 1, data_scroll_ + capacity - 1));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Permissions) {
        const int count =
            permissions_picker_ ? int(permission_sites().size()) : int(permission_catalog().size());
        const int capacity = std::max(1, (panel_bounds().height - 274) / 52);
        permissions_scroll_ = std::clamp(permissions_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                         std::max(0, count - capacity));
        if (permissions_picker_ && count > 0)
            permissions_pick_selected_ =
                std::clamp(permissions_pick_selected_, permissions_scroll_,
                           std::min(count - 1, permissions_scroll_ + capacity - 1));
        app_.changed();
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Commands) {
        move_command(input.dy < 0 ? 1 : -1);
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Filters) {
        panel_scroll_ = std::clamp(panel_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                   std::max(0, int(filter_lines_.size()) - 1));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Profiles) {
        const int capacity = std::max(1, (panel_bounds().height - 236) / 84);
        profiles_scroll_ = std::clamp(profiles_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                      std::max(0, int(listed_profiles().size()) - capacity));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Containers) {
        const int capacity = std::max(1, (panel_bounds().height - 260) / 88);
        container_scroll_ =
            std::clamp(container_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                       std::max(0, int(container_rules_mode_ ? app_.profile.container_rules.size()
                                                             : app_.profile.containers.size()) -
                                       capacity));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Groups) {
        int count = 0;
        for (const auto &g : app_.profile.groups)
            if (g.workspace == app_.workspace)
                ++count;
        group_scroll_ =
            std::clamp(group_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                       std::max(0, count - std::max(1, (panel_bounds().height - 260) / 88)));
        return;
    }
    if (input.kind == PointerInput::Wheel && panel_ == Workspaces) {
        const int capacity = std::max(1, (panel_bounds().height - 218) / 52);
        workspace_scroll_ = std::clamp(workspace_scroll_ + (input.dy < 0 ? 1 : -1), 0,
                                       std::max(0, int(app_.profile.workspaces.size()) - capacity));
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
                                              : app_.visible_downloads().size();
            panel_scroll_ =
                std::clamp(panel_scroll_ + (input.dy < 0 ? 2 : -2), 0, std::max(0, count - 4));
            return;
        }
    }
    if (!horizontal_tabs() && input.kind == PointerInput::Wheel && x < 240) {
        if (app_.profile.wheel_switch && !app_.tabs.empty())
            app_.cycle_tab(input.dy < 0 ? 1 : -1);
        else
            tab_scroll_ = std::max(0, tab_scroll_ + (input.dy < 0 ? 1 : -1));
        return;
    }
    if (!app_.dialogs.empty()) {
        if (down) {
            const auto buttons = dialog_buttons();
            if (inside(buttons[1], x, y))
                app_.answer_dialog(true);
            else if (inside(buttons[0], x, y))
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
                    app_.menu_x = std::clamp(b.x - 180, web_bounds().x, width_ - 250);
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
            if (panel_ == History) {
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
                    const auto visible = app_.visible_downloads();
                    if (i >= 0 && i < int(visible.size())) {
                        const auto index = visible[i];
                        const auto &d = app_.downloads[index];
                        if (d.complete || d.cancelled || d.interrupted) {
                            const int top = r.y + 141 + (i - panel_scroll_) * 75;
                            if (inside({r.x + r.width - 149, top + 5, 124, 34}, x, y)) {
                                if (d.complete)
                                    app_.verify_download(index);
                                else
                                    app_.retry_download(index);
                            }
                        } else if (x > r.x + r.width - 110)
                            app_.download_action(index, x > r.x + r.width - 64);
                    }
                }
            } else if (panel_ == Settings) {
                if (inside({r.x + r.width - 220, r.y + 21, 160, 32}, x, y))
                    open_search_providers();
                else
                    settings_pointer(r, x, y);
            } else if (panel_ == Extensions)
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
            else if (panel_ == Workspaces)
                workspaces_pointer(r, x, y);
            else if (panel_ == Groups) {
                if (inside({r.x + r.width - 220, r.y + 21, 160, 32}, x, y))
                    open_duplicates();
                else
                    groups_pointer(r, x, y);
            } else if (panel_ == Containers)
                containers_pointer(r, x, y);
            else if (panel_ == Profiles)
                profiles_pointer(r, x, y);
            else if (panel_ == Commands)
                commands_pointer(r, x, y);
            else if (panel_ == Translation)
                translation_pointer(r, x, y);
            else if (panel_ == Permissions)
                permissions_pointer(r, x, y);
            else if (panel_ == SiteData)
                site_data_pointer(r, x, y);
            else if (panel_ == Bookmarks) {
                if (inside({r.x + r.width - 220, r.y + 21, 160, 32}, x, y))
                    open_reading_list();
                else
                    bookmarks_pointer(r, x, y);
            } else if (panel_ == DuplicateTabs)
                duplicates_pointer(r, x, y);
            else if (panel_ == SpellingDictionary)
                spelling_pointer(r, x, y);
            else if (panel_ == ExtensionAccess)
                extension_access_pointer(r, x, y);
            else if (panel_ == Diagnostics)
                diagnostics_pointer(r, x, y);
            else if (panel_ == SearchProviders)
                search_providers_pointer(r, x, y);
            else if (panel_ == ReadingListPanel)
                reading_pointer(r, x, y);
            else if (panel_ == Sessions)
                sessions_pointer(r, x, y);
            else if (panel_ == TaskManager)
                task_manager_pointer(r, x, y);
            else if (panel_ == Hotkeys)
                hotkeys_pointer(r, x, y);
            else if (panel_ == Screenshot)
                screenshot_pointer(r, x, y);
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
    if (horizontal_tabs() && tab_strip_pointer(input))
        return;
    if (horizontal_tabs() && down && inside({12, 37, 34, 36}, x, y)) {
        open_aura_menu();
        return;
    }
    if (!horizontal_tabs() && down && x < 240) {
        if (inside({16, height_ - 60, 208, 54}, x, y)) {
            open_profiles();
            return;
        }
        if (inside({16, 12, 208, 62}, x, y)) {
            open_aura_menu();
            return;
        }
        if (inside({16, 88, 208, 40}, x, y)) {
            app_.open_new_tab(t && t->private_tab);
            return;
        }
        if (y >= 178 && y < height_ - 220) {
            const auto visible = sidebar_rows();
            const int position = (y - 178) / 52 + tab_scroll_;
            if (position - tab_scroll_ >= std::max(1, (height_ - 220 - 178) / 52))
                return;
            if (position < static_cast<int>(visible.size())) {
                const int index = visible[position];
                if (index < 0) {
                    auto group = app_.profile.group(-index);
                    if (!group)
                        return;
                    if (input.button == 3) {
                        open_groups();
                        group_edit_ = group->id;
                        group_input_.set(group->name, true);
                        group_color_ = group->color;
                        group_input_focus_ = true;
                    } else if (input.button == 1 && x >= 195) {
                        app_.new_tab("aura://newtab", t && t->private_tab, true, false,
                                     app_.workspace, group->id);
                    } else if (input.button == 1) {
                        group->collapsed = !group->collapsed;
                        app_.changed(true);
                    }
                    return;
                }
                if (app_.profile.tree_tabs && input.button == 1) {
                    for (const auto &row : app_.tree_rows(false))
                        if (row.index == index && row.descendants &&
                            inside({20 + std::min(row.depth, 3) * 12,
                                    178 + (position - tab_scroll_) * 52 + 7, 24, 32},
                                   x, y)) {
                            app_.toggle_tree(app_.tabs[index]->id);
                            return;
                        }
                }
                if (input.button == 2 && !app_.profile.middle_close)
                    return;
                if (input.button == 2 || (x > 195 && !app_.tabs[index]->pinned))
                    app_.close_tab(index);
                else {
                    app_.activate(index);
                    if (input.button == 1) {
                        drag_tab_ = index;
                        drag_source_ = app_.tabs[index]->id;
                        drag_x_ = x;
                        drag_y_ = y;
                    }
                    if (input.button == 3) {
                        open_tab_menu(index, x, y);
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
        if (t && t->container) {
            const auto addr = address_bounds();
            const int width = std::min(84, std::max(48, addr.width / 3));
            if (inside({addr.x + addr.width - width - 12, addr.y + 7, width, 24}, x, y)) {
                open_containers();
                return;
            }
        }
        if (inside({address_bounds().x + 5, address_bounds().y + 4, 30, 30}, x, y)) {
            open_permissions();
            return;
        }
        if (inside(address_bounds(), x, y)) {
            focus_address();
            return;
        }
        if (x >= navigation_x() && x < navigation_x() + 34) {
            app_.back();
            return;
        }
        if (x >= navigation_x() + 38 && x < navigation_x() + 72) {
            app_.forward();
            return;
        }
        if (x >= navigation_x() + 76 && x < navigation_x() + 110) {
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
    if (app_.split_enabled()) {
        const auto divider = split_divider();
        if (split_drag_ || (down && input.button == 1 && inside(divider, x, y))) {
            split_drag_ = input.kind != PointerInput::Up;
            if (input.kind == PointerInput::Move || down) {
                app_.profile.split_percent =
                    std::clamp(int(std::lround(100.0 * (x - web.x) / (web.width - 8))), 25, 75);
                app_.resize_pages();
                app_.changed(true);
            }
            return;
        }
        for (int id : app_.split_tabs)
            if (auto candidate = app_.tab(id)) {
                const auto header = split_header(*candidate);
                const auto bounds = page_bounds(*candidate);
                if (inside(header, x, y) || inside(bounds, x, y)) {
                    if (down || input.kind == PointerInput::Wheel) {
                        for (int i = 0; i < int(app_.tabs.size()); i++)
                            if (app_.tabs[i]->id == id) {
                                if (app_.active != i)
                                    app_.activate(i);
                                break;
                            }
                    }
                    if (inside(header, x, y)) {
                        if (down && x > header.x + header.width - 34)
                            app_.end_split();
                        return;
                    }
                    t = candidate;
                    web = bounds;
                    break;
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
        app_.window->cursor(inside(address_bounds(), x, y)    ? 1
                            : (!horizontal_tabs() && x < 240) ? 2
                                                              : 0);
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
        enabled = enabled && !app_.safe_mode();
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
    if (!app_.safe_mode())
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
        label(c, regular_,
              app_.tr(app_.safe_mode() ? "Extensions are paused in safe mode."
                                       : "Changes take effect after restarting Aura."),
              r.x + 28, r.y + 188, 12, t.muted, r.width - 56);
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
                  r.width - 400);
            chip({r.x + r.width - 156, y + 8, 128, 32},
                 e.pinned ? "Unpin extension" : "Pin extension");
            chip({r.x + r.width - 316, y + 8, 152, 32}, "Site access", false,
                 e.load_requested && e.enabled && !e.pending_update);
            label(c, regular_,
                  app_.tr(app_.safe_mode()   ? "Paused in safe mode."
                          : e.pending_update ? "Update after restart"
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
    if (app_.safe_mode()) {
        toast(app_.tr("Extensions are paused in safe mode."));
        return;
    }
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
    if (inside({r.x + r.width - 316, top + 8, 152, 32}, x, y))
        open_extension_access(m.items[index].id);
    else if (inside({r.x + r.width - 156, top + 8, 128, 32}, x, y))
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
    if (app_.safe_mode())
        return {};
    std::vector<size_t> indices;
    const size_t capacity = std::max(0, (width_ - (horizontal_tabs() ? 424 : 628) - 180) / 36);
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
    int w = std::clamp(p->desired_width, 180, std::min(800, web_bounds().width - 32));
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
        return same || inside(web_bounds(), x, y);
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
        ",\"list_revision\":" + std::to_string(filter_seen_) +
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

namespace aura {
void BrowserUi::open_workspaces(int move_tab) {
    panel(Workspaces);
    workspace_move_tab_ = move_tab;
    workspace_edit_ = -1;
    workspace_input_focus_ = false;
    workspace_input_.set("");
    workspace_error_.clear();
    workspace_scroll_ = 0;
    app_.changed();
}
void BrowserUi::save_workspace() {
    if (app_.private_window)
        return;
    const bool editing = workspace_edit_ >= 0;
    const int id = editing ? workspace_edit_ : app_.profile.add_workspace(workspace_input_.value);
    if (id < 0 || (editing && !app_.profile.rename_workspace(id, workspace_input_.value))) {
        workspace_error_ = app_.tr("Enter a short, unique name. Up to 32 workspaces.");
        app_.changed();
        return;
    }
    workspace_error_.clear();
    workspace_input_.set("");
    workspace_input_focus_ = false;
    workspace_edit_ = -1;
    if (!editing && workspace_move_tab_ < 0)
        app_.switch_workspace(id);
    app_.changed(true);
}
void BrowserUi::draw_workspaces(SkCanvas &c, Rect r) {
    const auto t = interface_theme(app_.profile);
    const bool enabled = !app_.private_window;
    label(c, regular_,
          app_.tr(enabled ? "Switch tasks without reloading your pages."
                          : "Workspaces are unavailable in private windows."),
          r.x + 28, r.y + 74, 12, t.muted, r.width - 56);
    auto button = [&](Rect b, const std::string &text, bool active, bool primary = false) {
        if (active)
            focus_target(b);
        fill(c, b, 8,
             SkColorSetA(t.accent, primary                                   ? 45
                                   : inside(b, hover_x_, hover_y_) && active ? 28
                                                                             : 12));
        border(c, b, 8, SkColorSetA(t.accent, active ? 110 : 30));
        centered_label(c, medium_, app_.tr(text), b, 12, active ? t.text : t.muted, 5, 5);
    };
    label(c, regular_, app_.tr("Workspace name"), r.x + 28, r.y + 99, 11, t.muted);
    Rect input{r.x + 28, r.y + 108, r.width - 166, 36};
    if (enabled)
        focus_target(input);
    fill(c, input, 8, t.background);
    border(c, input, 8, SkColorSetA(t.accent, workspace_input_focus_ ? 220 : 85));
    draw_editor(c, workspace_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                workspace_input_focus_);
    button({r.x + r.width - 126, r.y + 108, 98, 36}, workspace_edit_ >= 0 ? "Save name" : "Create",
           enabled && (workspace_edit_ >= 0 || app_.profile.workspaces.size() < 32), true);
    label(c, regular_, workspace_error_, r.x + 28, r.y + 162, 11, t.text, r.width - 56);
    if (workspace_move_tab_ >= 0)
        label(c, regular_, app_.tr("Choose where to move the selected tab."), r.x + 28, r.y + 185,
              11, t.muted, r.width - 56);
    const int capacity = std::max(1, (r.height - 218) / 52);
    workspace_scroll_ = std::clamp(workspace_scroll_, 0,
                                   std::max(0, int(app_.profile.workspaces.size()) - capacity));
    for (int i = workspace_scroll_;
         i < int(app_.profile.workspaces.size()) && i < workspace_scroll_ + capacity; ++i) {
        const auto &w = app_.profile.workspaces[i];
        const int y = r.y + 192 + (i - workspace_scroll_) * 52;
        const int count =
            int(std::count_if(app_.tabs.begin(), app_.tabs.end(),
                              [&](const auto &tab) { return tab->workspace == w.id; }));
        const auto name = app_.profile.workspace_label(w.id) + " (" + std::to_string(count) + ")";
        if (w.id == app_.workspace)
            fill(c, {r.x + 18, y - 3, r.width - 36, 46}, 8, SkColorSetA(t.accent, 18));
        label(c, w.id == app_.workspace ? medium_ : regular_, name, r.x + 28, y + 22, 13, t.text,
              std::max(40, r.width - 364));
        button({r.x + r.width - 324, y, 86, 34},
               workspace_move_tab_ >= 0 ? "Move here"
               : w.id == app_.workspace ? "Current"
                                        : "Open",
               enabled);
        button({r.x + r.width - 230, y, 112, 34}, "Rename", enabled);
        button({r.x + r.width - 110, y, 82, 34}, "Remove", enabled && w.id != 0);
    }
    label(c, regular_, app_.tr("Page Up / Page Down to scroll. Private tabs are never saved."),
          r.x + 28, r.y + r.height - 13, 10, t.muted, r.width - 56);
}
void BrowserUi::workspaces_pointer(Rect r, int x, int y) {
    if (app_.private_window)
        return;
    if (inside({r.x + 28, r.y + 108, r.width - 166, 36}, x, y)) {
        workspace_input_focus_ = true;
        keyboard_focus_ = -1;
        app_.window->caret(r.x + 38, r.y + 142);
        return;
    }
    if (inside({r.x + r.width - 126, r.y + 108, 98, 36}, x, y)) {
        save_workspace();
        return;
    }
    workspace_input_focus_ = false;
    const int index = workspace_scroll_ + (y - r.y - 192) / 52;
    const int capacity = std::max(1, (r.height - 218) / 52);
    if (y < r.y + 192 || index >= int(app_.profile.workspaces.size()) ||
        index >= workspace_scroll_ + capacity)
        return;
    const auto item = app_.profile.workspaces[index];
    if (x >= r.x + r.width - 324 && x < r.x + r.width - 238) {
        if (workspace_move_tab_ >= 0) {
            app_.move_tab_to_workspace(workspace_move_tab_, item.id);
            app_.changed(true);
        } else
            app_.switch_workspace(item.id);
        panel(None);
    } else if (x >= r.x + r.width - 230 && x < r.x + r.width - 118) {
        workspace_edit_ = item.id;
        workspace_input_.set(app_.profile.workspace_label(item.id), true);
        workspace_input_focus_ = true;
    } else if (x >= r.x + r.width - 110 && item.id != 0) {
        app_.confirm_action(app_.tr("Remove workspace?") + " " +
                                app_.profile.workspace_label(item.id),
                            app_.tr("Tabs will move to Personal space. Pages stay open."),
                            [this, id = item.id] { app_.remove_workspace(id); });
    }
}
std::string BrowserUi::workspaces_report() const {
    if (panel_ != Workspaces)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"input\":" + rect({r.x + 28, r.y + 108, r.width - 166, 36}) +
                      ",\"save\":" + rect({r.x + r.width - 126, r.y + 108, 98, 36}) +
                      ",\"input_length\":" + std::to_string(workspace_input_.value.size()) +
                      ",\"error\":" + json_quote(workspace_error_) + ",\"items\":[";
    const int capacity = std::max(1, (r.height - 218) / 52);
    bool first = true;
    for (int i = workspace_scroll_;
         i < int(app_.profile.workspaces.size()) && i < workspace_scroll_ + capacity; ++i) {
        if (!first)
            out += ',';
        first = false;
        const auto &w = app_.profile.workspaces[i];
        const int y = r.y + 192 + (i - workspace_scroll_) * 52;
        out += "{\"id\":" + std::to_string(w.id) + ",\"name\":" + json_quote(w.name) +
               ",\"open\":" + rect({r.x + r.width - 324, y, 86, 34}) +
               ",\"rename\":" + rect({r.x + r.width - 230, y, 112, 34}) +
               ",\"remove\":" + rect({r.x + r.width - 110, y, 82, 34}) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
std::vector<int> BrowserUi::sidebar_rows() const {
    auto tabs = app_.visible_tabs();
    if (app_.profile.tree_tabs) {
        tabs.clear();
        for (const auto &r : app_.tree_rows())
            if (app_.tabs[r.index]->workspace == app_.workspace)
                tabs.push_back(r.index);
    }
    if (app_.private_window)
        return tabs;
    std::vector<int> rows;
    for (const auto &g : app_.profile.groups) {
        if (g.workspace != app_.workspace)
            continue;
        rows.push_back(-g.id);
        if (!g.collapsed)
            for (int i : tabs)
                if (app_.tabs[i]->group == g.id)
                    rows.push_back(i);
    }
    for (int i : tabs) {
        const auto g = app_.profile.group(app_.tabs[i]->group);
        if (!g || g->workspace != app_.workspace)
            rows.push_back(i);
    }
    return rows;
}
void BrowserUi::open_groups(int tab_id) {
    panel(Groups);
    group_target_ = tab_id >= 0 ? tab_id : app_.current() ? app_.current()->id : -1;
    group_edit_ = -1;
    group_color_ = 0;
    group_scroll_ = 0;
    group_error_.clear();
    group_input_.set("");
    group_input_focus_ = false;
}
void BrowserUi::save_group() {
    if (app_.private_window)
        return;
    const bool edit = group_edit_ >= 0;
    const int id = edit ? group_edit_
                        : app_.profile.add_group(app_.workspace, group_input_.value, group_color_);
    if (id < 0 || (edit && !app_.profile.rename_group(id, group_input_.value))) {
        group_error_ = app_.tr("Enter a short unique group name. Up to 128 groups.");
        app_.changed();
        return;
    }
    if (auto g = app_.profile.group(id))
        g->color = group_color_;
    if (!edit && group_target_ >= 0)
        app_.set_tab_group(group_target_, id);
    group_input_.set("");
    group_input_focus_ = false;
    group_edit_ = -1;
    group_error_.clear();
    app_.changed(true);
}
void BrowserUi::draw_groups(SkCanvas &c, Rect r) {
    const auto t = interface_theme(app_.profile);
    const bool enabled = !app_.private_window;
    label(c, regular_,
          app_.tr(enabled ? "Groups organize this workspace. Collapsed pages keep running."
                          : "Tab groups are unavailable in private windows."),
          r.x + 28, r.y + 74, 12, t.muted, r.width - 56);
    auto button = [&](Rect b, const std::string &text, bool active, SkColor color = 0) {
        if (active)
            focus_target(b);
        const auto ink = color ? color : t.accent;
        fill(c, b, 8, SkColorSetA(ink, inside(b, hover_x_, hover_y_) && active ? 35 : 16));
        border(c, b, 8, SkColorSetA(ink, active ? 120 : 30));
        centered_label(c, medium_, app_.tr(text), b, 12, active ? t.text : t.muted, 5, 5);
    };
    label(c, regular_, app_.tr("Group name"), r.x + 28, r.y + 99, 11, t.muted);
    Rect input{r.x + 28, r.y + 108, r.width - 196, 36};
    if (enabled)
        focus_target(input);
    fill(c, input, 8, t.background);
    border(c, input, 8, SkColorSetA(t.accent, group_input_focus_ ? 220 : 85));
    draw_editor(c, group_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                group_input_focus_);
    button({r.x + r.width - 156, r.y + 108, 128, 36},
           group_edit_ >= 0 ? "Save name" : "Create group", enabled);
    static const char *colors[] = {"Accent", "Blue", "Green", "Rose", "Amber", "Teal"};
    for (int i = 0; i < 6; ++i) {
        Rect b{r.x + 28 + i * 90, r.y + 154, 82, 32};
        button(b, colors[i], enabled, tab_group_color(app_.profile, i));
        if (group_color_ == i) {
            border(c, b, 8, SkColorSetA(t.text, 210));
            icon(c, "check", b.x + b.width - 11, b.y + 2, 9, t.text);
        }
    }
    const auto selected = app_.tab(group_target_);
    label(c, regular_,
          group_error_.empty() && selected ? app_.tr("Selected tab: ") + selected->page.title
                                           : group_error_,
          r.x + 28, r.y + 205, 11, t.text, r.width - 56);
    std::vector<const TabGroup *> list;
    for (const auto &g : app_.profile.groups)
        if (g.workspace == app_.workspace)
            list.push_back(&g);
    const int capacity = std::max(1, (r.height - 260) / 88);
    group_scroll_ = std::clamp(group_scroll_, 0, std::max(0, int(list.size()) - capacity));
    if (list.empty())
        label(c, regular_, app_.tr("Create a group to organize this workspace."), r.x + 28,
              r.y + 250, 13, t.muted, r.width - 56);
    for (int i = group_scroll_; i < int(list.size()) && i < group_scroll_ + capacity; ++i) {
        const auto &g = *list[i];
        const int y = r.y + 212 + (i - group_scroll_) * 88;
        const int count =
            int(std::count_if(app_.tabs.begin(), app_.tabs.end(), [&](const auto &tab) {
                return tab->workspace == app_.workspace && tab->group == g.id;
            }));
        fill(c, {r.x + 28, y + 9, 12, 12}, 3, tab_group_color(app_.profile, g.color));
        label(c, medium_, g.name + " (" + std::to_string(count) + ")", r.x + 48, y + 23, 13, t.text,
              r.width - 210);
        label(c, regular_, app_.tr(g.collapsed ? "Collapsed" : "Expanded"), r.x + r.width - 145,
              y + 23, 11, t.muted, 115);
        button({r.x + 28, y + 36, 84, 34}, "Add tab", enabled);
        button({r.x + 120, y + 36, 112, 34}, "Rename", enabled);
        button({r.x + 240, y + 36, 60, 34}, "Color", enabled,
               tab_group_color(app_.profile, g.color));
        button({r.x + 308, y + 36, 100, 34}, g.collapsed ? "Expand" : "Collapse", enabled);
        button({r.x + 416, y + 36, 82, 34}, "Remove", enabled);
    }
    button({r.x + r.width - 160, r.y + r.height - 48, 132, 32}, "Ungroup tab",
           enabled && selected && selected->group != 0);
    label(c, regular_, app_.tr("Page Up / Page Down to scroll. Removing a group keeps its tabs."),
          r.x + 28, r.y + r.height - 13, 10, t.muted, r.width - 56);
}
void BrowserUi::groups_pointer(Rect r, int x, int y) {
    if (app_.private_window)
        return;
    if (inside({r.x + r.width - 160, r.y + r.height - 48, 132, 32}, x, y)) {
        app_.set_tab_group(group_target_, 0);
        panel(None);
        return;
    }
    if (inside({r.x + 28, r.y + 108, r.width - 196, 36}, x, y)) {
        group_input_focus_ = true;
        keyboard_focus_ = -1;
        return;
    }
    if (inside({r.x + r.width - 156, r.y + 108, 128, 36}, x, y)) {
        save_group();
        return;
    }
    for (int i = 0; i < 6; ++i)
        if (inside({r.x + 28 + i * 90, r.y + 154, 82, 32}, x, y)) {
            group_color_ = i;
            app_.changed();
            return;
        }
    group_input_focus_ = false;
    if (y < r.y + 212)
        return;
    std::vector<int> list;
    for (const auto &g : app_.profile.groups)
        if (g.workspace == app_.workspace)
            list.push_back(g.id);
    const int position = group_scroll_ + (y - r.y - 212) / 88;
    const int capacity = std::max(1, (r.height - 260) / 88);
    if (position >= int(list.size()) || position >= group_scroll_ + capacity)
        return;
    auto g = app_.profile.group(list[position]);
    if (!g)
        return;
    const int row_y = r.y + 212 + (position - group_scroll_) * 88;
    if (y < row_y + 36 || y >= row_y + 70)
        return;
    if (x >= r.x + 28 && x < r.x + 112) {
        app_.set_tab_group(group_target_, g->id);
        panel(None);
    } else if (x >= r.x + 120 && x < r.x + 232) {
        group_edit_ = g->id;
        group_color_ = g->color;
        group_input_.set(g->name, true);
        group_input_focus_ = true;
    } else if (x >= r.x + 240 && x < r.x + 300) {
        g->color = (g->color + 1) % 6;
        app_.changed(true);
    } else if (x >= r.x + 308 && x < r.x + 408) {
        g->collapsed = !g->collapsed;
        app_.changed(true);
    } else if (x >= r.x + 416 && x < r.x + 498)
        app_.confirm_action(app_.tr("Remove group?") + " " + g->name,
                            app_.tr("Tabs will remain open without this group."),
                            [this, id = g->id] { app_.remove_group(id); });
}
std::string BrowserUi::groups_report() const {
    if (panel_ != Groups)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"ungroup\":" + rect({r.x + r.width - 160, r.y + r.height - 48, 132, 32}) +
                      ",\"input\":" + rect({r.x + 28, r.y + 108, r.width - 196, 36}) +
                      ",\"save\":" + rect({r.x + r.width - 156, r.y + 108, 128, 36}) +
                      ",\"input_length\":" + std::to_string(group_input_.value.size()) +
                      ",\"error\":" + json_quote(group_error_) + ",\"items\":[";
    int position = 0;
    bool first = true;
    const int capacity = std::max(1, (r.height - 260) / 88);
    for (const auto &g : app_.profile.groups) {
        if (g.workspace != app_.workspace)
            continue;
        int i = position++;
        if (i < group_scroll_ || i >= group_scroll_ + capacity)
            continue;
        if (!first)
            out += ',';
        first = false;
        const int y = r.y + 212 + (i - group_scroll_) * 88;
        out += "{\"id\":" + std::to_string(g.id) + ",\"name\":" + json_quote(g.name) +
               ",\"color\":" + std::to_string(g.color) +
               ",\"collapsed\":" + (g.collapsed ? "true" : "false") +
               ",\"assign\":" + rect({r.x + 28, y + 36, 84, 34}) +
               ",\"rename\":" + rect({r.x + 120, y + 36, 112, 34}) +
               ",\"color_button\":" + rect({r.x + 240, y + 36, 60, 34}) +
               ",\"collapse\":" + rect({r.x + 308, y + 36, 100, 34}) +
               ",\"remove\":" + rect({r.x + 416, y + 36, 82, 34}) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
std::string BrowserUi::sidebar_report() const {
    if (horizontal_tabs())
        return "[]";
    const auto rows = sidebar_rows();
    const auto tree = app_.tree_rows(false);
    std::string out = "[";
    bool first = true;
    const int capacity = std::max(1, (height_ - 220 - 178) / 52);
    for (int position = tab_scroll_;
         position < int(rows.size()) && position < tab_scroll_ + capacity; ++position) {
        if (!first)
            out += ',';
        first = false;
        const int y = 178 + (position - tab_scroll_) * 52;
        const int index = rows[position];
        const auto node =
            std::find_if(tree.begin(), tree.end(), [&](const auto &r) { return r.index == index; });
        const int depth = app_.profile.tree_tabs && node != tree.end() ? node->depth : 0;
        const int descendants =
            app_.profile.tree_tabs && node != tree.end() ? node->descendants : 0;
        out += "{\"group\":" + std::to_string(index < 0 ? -index : 0) +
               ",\"tab_index\":" + std::to_string(index < 0 ? -1 : index) +
               ",\"depth\":" + std::to_string(depth) +
               ",\"descendants\":" + std::to_string(descendants) + ",\"disclosure\":" +
               (descendants ? "[" + std::to_string(20 + std::min(depth, 3) * 12) + "," +
                                  std::to_string(y + 7) + ",24,32]"
                            : "null") +
               ",\"bounds\":[14," + std::to_string(y) + ",212,46]}";
    }
    return out + "]";
}
} // namespace aura

namespace aura {
void BrowserUi::draw_page(SkCanvas &c, const Tab &tab, Rect web) {
    const auto &t = interface_theme(app_.profile);
    const bool light = app_.profile.theme % 2;
    const auto ink = t.text, muted = t.muted;
    shadow(c, web, 15);
    // Cache only the native backdrop, before drawing CEF pixels. Restoring
    // it prevents antialiased corners and borders from darkening over time.
    if (!app_.split_enabled())
        web_backdrop_.reset();
    if (!app_.split_enabled() && !c.recordingContext()) {
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
    page_image(c, tab.page.view, web, light ? 0xFFFFFFFF : 0xFF161B25);
    c.save();
    c.clipRRect(SkRRect::MakeRectXY(box(web), 15, 15), true);
    if (tab.page.popup_visible) {
        auto p = tab.page.popup_bounds;
        image(c, tab.page.popup,
              {web.x + int(p.x / app_.ui_scale), web.y + int(p.y / app_.ui_scale),
               int(p.width / app_.ui_scale), int(p.height / app_.ui_scale)});
    }
    if (tab.page.failed) {
        fill(c, web, 0, t.background);
        icon(c, "globe", web.x + 64, web.y + 70, 38, t.accent);
        label(c, medium_,
              app_.tr(tab.process_ended ? "Page process ended" : "This page couldn't be reached"),
              web.x + 64, web.y + 158, 29, ink, web.width - 100);
        label(c, regular_,
              tab.process_ended ? app_.tr("Reload this tab to restore the page.") : tab.page.error,
              web.x + 64, web.y + 195, 13, muted, web.width - 100);
        fill(c, {web.x + 64, web.y + 225, 125, 38}, 10, t.accent);
        centered_label(c, medium_, app_.tr(tab.process_ended ? "Reload" : "Try again"),
                       {web.x + 64, web.y + 225, 125, 38}, 12, t.background);
    } else if (!tab.page.view.generation()) {
        label(c, regular_, app_.tr("Opening ") + url_host(tab.page.url) + "…", web.x + 40,
              web.y + 65, 14, muted);
    }
    c.restore();
    border(c, web, 15, SkColorSetA(ink, 22));
    if (tab.loading)
        fill(c,
             {web.x + 12, web.y,
              std::max(20, static_cast<int>((web.width - 24) *
                                            (app_.split_enabled() ? tab.progress : progress_))),
              2},
             2, t.accent);
}
Rect BrowserUi::split_divider() const {
    const auto r = web_bounds();
    const int left =
        std::clamp((r.width - 8) * app_.profile.split_percent / 100,
                   std::min(220, (r.width - 8) / 2), std::max((r.width - 8) / 2, r.width - 228));
    return {r.x + left, r.y, 8, r.height};
}
Rect BrowserUi::page_bounds(const Tab &tab) const {
    auto r = web_bounds();
    if (!app_.split_enabled())
        return r;
    const auto divider = split_divider();
    if (tab.id == app_.split_tabs[0])
        return {r.x, r.y + 40, divider.x - r.x, r.height - 40};
    if (tab.id == app_.split_tabs[1])
        return {divider.x + 8, r.y + 40, r.x + r.width - divider.x - 8, r.height - 40};
    return r;
}
Rect BrowserUi::split_header(const Tab &tab) const {
    auto r = page_bounds(tab);
    return {r.x, r.y - 40, r.width, 40};
}
void BrowserUi::draw_split(SkCanvas &c) {
    const auto &theme = interface_theme(app_.profile);
    web_backdrop_.reset();
    for (int id : app_.split_tabs)
        if (auto tab = app_.tab(id)) {
            const auto r = page_bounds(*tab), header = split_header(*tab);
            const bool active = tab == app_.current();
            draw_page(c, *tab, r);
            fill(c, header, 8, active ? SkColorSetA(theme.accent, 28) : theme.surface);
            focus_target(header);
            icon(c, tab->private_tab ? "private" : "globe", header.x + 8, header.y + 12, 16,
                 theme.muted);
            label(c, medium_, tab->page.title, header.x + 32, header.y + 16, 11,
                  active ? theme.text : theme.muted, header.width - 70);
            label(c, regular_, url_host(tab->page.url), header.x + 32, header.y + 31, 9,
                  theme.muted, header.width - 70);
            focus_target({header.x + header.width - 34, header.y, 34, 40});
            icon(c, "close", header.x + header.width - 25, header.y + 13, 14, theme.muted);
            border(c, r, 15, SkColorSetA(theme.accent, active ? 170 : 35));
        }
    const auto d = split_divider();
    fill(c, {d.x + 2, d.y + d.height / 2 - 24, 4, 48}, 2,
         SkColorSetA(theme.accent, split_drag_ || inside(d, hover_x_, hover_y_) ? 210 : 80));
}
std::string BrowserUi::split_report() const {
    if (!app_.split_enabled())
        return "null";
    auto rect = [](Rect r) {
        return "[" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
               std::to_string(r.width) + "," + std::to_string(r.height) + "]";
    };
    std::string result = "{\"divider\":" + rect(split_divider()) +
                         ",\"percent\":" + std::to_string(app_.profile.split_percent) +
                         ",\"panes\":[";
    for (int i = 0; i < 2; i++) {
        auto t = app_.tab(app_.split_tabs[i]);
        if (i)
            result += ",";
        if (!t) {
            result += "null";
            continue;
        }
        result += "{\"id\":" + std::to_string(t->id) + ",\"url\":" + json_quote(t->page.url) +
                  ",\"header\":" + rect(split_header(*t)) + ",\"bounds\":" + rect(page_bounds(*t)) +
                  ",\"active\":" + (t == app_.current() ? "true" : "false") + "}";
    }
    return result + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::open_containers(int id) {
    auto source = id >= 0 ? app_.tab(id) : app_.current();
    if (app_.private_window || (source && source->private_tab)) {
        app_.ui->toast(app_.tr("Containers are unavailable in private tabs."));
        return;
    }
    container_rules_mode_ = false;
    container_target_ = id >= 0 ? id : (app_.current() ? app_.current()->id : -1);
    container_edit_ = -1;
    container_color_ = 0;
    container_scroll_ = 0;
    container_input_.set("");
    container_error_.clear();
    panel(Containers);
    container_input_focus_ = false;
}
void BrowserUi::save_container() {
    if (app_.private_window)
        return;
    if (container_rules_mode_) {
        if (!app_.save_container_rule(container_input_.value, container_rule_target_,
                                      container_rule_subdomains_)) {
            container_error_ = app_.tr("Enter a hostname without a path or port. Up to 256 rules.");
            app_.changed();
            return;
        }
        container_input_.set("");
        container_error_.clear();
        container_input_focus_ = false;
        app_.changed(true);
        return;
    }
    bool success = false;
    if (container_edit_ > 0)
        success = app_.profile.rename_container(container_edit_, container_input_.value,
                                                container_color_);
    else
        success = app_.profile.add_container(container_input_.value, container_color_) > 0;
    if (!success) {
        container_error_ = app_.tr("Enter a short unique container name. Up to 32 containers.");
        app_.changed();
        return;
    }
    container_input_.set("");
    container_input_focus_ = false;
    container_edit_ = -1;
    container_error_.clear();
    app_.changed(true);
}
void BrowserUi::draw_containers(SkCanvas &canvas, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    Rect mode{r.x + r.width - 254, r.y + 20, 194, 32};
    focus_target(mode);
    fill(canvas, mode, 8, SkColorSetA(theme.accent, 16));
    border(canvas, mode, 8, SkColorSetA(theme.accent, 100));
    centered_label(canvas, medium_,
                   app_.tr(container_rules_mode_ ? "Back to containers" : "Site rules"), mode, 12,
                   theme.text, 5, 5);
    if (container_rules_mode_) {
        draw_container_rules(canvas, r);
        return;
    }
    const auto &t = interface_theme(app_.profile);
    auto &c = canvas;
    const bool enabled = !app_.private_window;
    label(c, regular_,
          app_.tr(enabled ? "Cookies are separate. History and bookmarks are shared."
                          : "Containers are unavailable in private windows."),
          r.x + 28, r.y + 73, 12, t.muted, r.width - 56);
    auto button = [&](Rect b, const std::string &text, bool usable = true, SkColor color = 0) {
        usable = usable && enabled;
        if (usable)
            focus_target(b);
        const auto ink = color ? color : t.accent;
        fill(c, b, 8, SkColorSetA(ink, usable && inside(b, hover_x_, hover_y_) ? 35 : 16));
        border(c, b, 8, SkColorSetA(ink, usable ? 120 : 30));
        centered_label(c, medium_, app_.tr(text), b, 12, usable ? t.text : t.muted, 5, 5);
    };
    label(c, regular_, app_.tr("Container name"), r.x + 28, r.y + 99, 11, t.muted);
    Rect input{r.x + 28, r.y + 108, r.width - 196, 36};
    if (enabled)
        focus_target(input);
    fill(c, input, 8, t.background);
    border(c, input, 8, SkColorSetA(t.accent, container_input_focus_ ? 220 : 85));
    draw_editor(c, container_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                container_input_focus_);
    button({r.x + r.width - 156, r.y + 108, 128, 36},
           container_edit_ > 0 ? "Save name" : "Create container");
    static const char *colors[] = {"Accent", "Blue", "Green", "Rose", "Amber", "Teal"};
    for (int i = 0; i < 6; i++) {
        Rect b{r.x + 28 + i * 90, r.y + 154, 82, 32};
        button(b, colors[i], true, tab_group_color(app_.profile, i));
        if (container_color_ == i) {
            border(c, b, 8, SkColorSetA(t.text, 210));
            icon(c, "check", b.x + b.width - 11, b.y + 2, 9, t.text);
        }
    }
    label(c, regular_, container_error_, r.x + 28, r.y + 205, 11, t.text, r.width - 56);
    const int capacity = std::max(1, (r.height - 260) / 88);
    container_scroll_ =
        std::clamp(container_scroll_, 0,
                   std::max(0, int(container_rules_mode_ ? app_.profile.container_rules.size()
                                                         : app_.profile.containers.size()) -
                                   capacity));
    for (int i = container_scroll_;
         i < int(app_.profile.containers.size()) && i < container_scroll_ + capacity; i++) {
        const auto &item = app_.profile.containers[i];
        const int y = r.y + 212 + (i - container_scroll_) * 88;
        fill(c, {r.x + 28, y + 8, 12, 12}, 3, tab_group_color(app_.profile, item.color));
        label(c, medium_, app_.profile.container_label(item.id), r.x + 48, y + 19, 13, t.text,
              r.width - 120);
        button({r.x + 28, y + 36, 148, 34}, "New tab");
        button({r.x + 184, y + 36, 170, 34}, "Open page copy",
               app_.tab(container_target_) && !app_.tab(container_target_)->private_tab);
        button({r.x + 362, y + 36, 112, 34}, "Rename", item.id != 0);
        button({r.x + 482, y + 36, 82, 34}, "Remove",
               item.id != 0 && !app_.container_in_use(item.id));
    }
    label(c, regular_,
          app_.tr("Close a container's tabs before removing it. Site data clears after restart."),
          r.x + 28, r.y + r.height - 22, 10, t.muted, r.width - 56);
}
void BrowserUi::containers_pointer(Rect r, int x, int y) {
    if (app_.private_window)
        return;
    if (inside({r.x + r.width - 254, r.y + 20, 194, 32}, x, y)) {
        container_rules_mode_ = !container_rules_mode_;
        container_scroll_ = 0;
        container_input_.set("");
        container_error_.clear();
        container_input_focus_ = false;
        if (!app_.profile.container(container_rule_target_))
            container_rule_target_ = 0;
        app_.changed();
        return;
    }
    if (container_rules_mode_) {
        container_rules_pointer(r, x, y);
        return;
    }
    if (inside({r.x + 28, r.y + 108, r.width - 196, 36}, x, y)) {
        container_input_focus_ = true;
        keyboard_focus_ = -1;
        return;
    }
    if (inside({r.x + r.width - 156, r.y + 108, 128, 36}, x, y)) {
        save_container();
        return;
    }
    for (int i = 0; i < 6; i++)
        if (inside({r.x + 28 + i * 90, r.y + 154, 82, 32}, x, y)) {
            container_color_ = i;
            app_.changed();
            return;
        }
    container_input_focus_ = false;
    if (y < r.y + 212)
        return;
    const int position = (y - r.y - 212) / 88 + container_scroll_;
    if (position < 0 || position >= int(app_.profile.containers.size()))
        return;
    const auto item = app_.profile.containers[position];
    const int top = r.y + 212 + (position - container_scroll_) * 88;
    if (inside({r.x + 28, top + 36, 148, 34}, x, y)) {
        panel(None);
        app_.new_tab("aura://newtab", false, true, false, app_.workspace, 0, item.id);
    } else if (inside({r.x + 184, top + 36, 170, 34}, x, y)) {
        auto source = app_.tab(container_target_);
        if (!source || source->private_tab)
            return;
        auto url = source->page.url;
        panel(None);
        app_.new_tab(url, false, true, false, app_.workspace, 0, item.id);
    } else if (item.id && inside({r.x + 362, top + 36, 112, 34}, x, y)) {
        container_edit_ = item.id;
        container_input_.set(item.name, true);
        container_color_ = item.color;
        container_input_focus_ = true;
        app_.changed();
    } else if (item.id && !app_.container_in_use(item.id) &&
               inside({r.x + 482, top + 36, 82, 34}, x, y)) {
        app_.confirm_action(
            app_.tr("Remove container?"),
            item.name + "\n" +
                app_.tr("Cookies and site storage will be deleted the next time Aura starts."),
            [this, id = item.id] { app_.remove_container(id); });
    }
}
std::string BrowserUi::containers_report() const {
    if (panel_ != Containers)
        return "null";
    if (container_rules_mode_)
        return container_rules_report();
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"rules_button\":" + rect({r.x + r.width - 254, r.y + 20, 194, 32}) +
                      ",\"input\":" + rect({r.x + 28, r.y + 108, r.width - 196, 36}) +
                      ",\"save\":" + rect({r.x + r.width - 156, r.y + 108, 128, 36}) +
                      ",\"input_length\":" + std::to_string(container_input_.value.size()) +
                      ",\"error\":" + json_quote(container_error_) + ",\"items\":[";
    const int capacity = std::max(1, (r.height - 260) / 88);
    for (int i = container_scroll_;
         i < int(app_.profile.containers.size()) && i < container_scroll_ + capacity; i++) {
        if (i != container_scroll_)
            out += ",";
        const auto &item = app_.profile.containers[i];
        const int y = r.y + 212 + (i - container_scroll_) * 88;
        out += "{\"id\":" + std::to_string(item.id) +
               ",\"name\":" + json_quote(app_.profile.container_label(item.id)) +
               ",\"color\":" + std::to_string(item.color) +
               ",\"in_use\":" + (app_.container_in_use(item.id) ? "true" : "false") +
               ",\"new_tab\":" + rect({r.x + 28, y + 36, 148, 34}) +
               ",\"copy\":" + rect({r.x + 184, y + 36, 170, 34}) +
               ",\"rename\":" + rect({r.x + 362, y + 36, 112, 34}) +
               ",\"remove\":" + rect({r.x + 482, y + 36, 82, 34}) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
std::vector<int> BrowserUi::listed_profiles() const {
    std::vector<int> ids;
    const auto &catalog = app_.state()->profiles;
    for (const auto &p : catalog.items)
        if (profiles_archived_ ? p.archived : (!p.archived || p.id == catalog.current_id))
            ids.push_back(p.id);
    return ids;
}
void BrowserUi::open_profiles() {
    profile_edit_ = -1;
    profile_input_.set("");
    profile_error_.clear();
    profiles_scroll_ = 0;
    profiles_archived_ = false;
    try {
        app_.state()->profiles.reload();
    } catch (const std::exception &e) {
        profile_error_ = app_.tr(e.what());
    }
    panel(Profiles);
    profile_input_focus_ = false;
}
void BrowserUi::save_profile() {
    auto &catalog = app_.state()->profiles;
    try {
        const bool edit = profile_edit_ >= 0;
        const int id =
            edit ? profile_edit_ : catalog.create(profile_input_.value, app_.profile.language);
        if (id < 0 || (edit && !catalog.rename(id, profile_input_.value))) {
            profile_error_ = app_.tr("Enter a short unique profile name. Up to 32 profiles.");
            app_.changed();
            return;
        }
        app_.profile.profiles_root = utf8_path(catalog.root);
        profile_edit_ = -1;
        profile_input_.set("");
        profile_input_focus_ = false;
        profile_error_.clear();
        for (auto *window : app_.state()->windows)
            window->update_window_title();
        app_.changed(true);
        if (!edit)
            app_.open_profile(id);
    } catch (const std::exception &e) {
        profile_error_ = app_.tr(e.what());
        app_.changed();
    }
}
void BrowserUi::draw_profiles(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    const auto &catalog = app_.state()->profiles;
    label(c, regular_,
          app_.tr("Separate settings, history, extensions and passwords. Opens a normal window."),
          r.x + 28, r.y + 73, 12, t.muted, r.width - 56);
    auto button = [&](Rect b, const std::string &text, bool enabled = true) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8, SkColorSetA(t.accent, enabled && inside(b, hover_x_, hover_y_) ? 35 : 16));
        border(c, b, 8, SkColorSetA(t.accent, enabled ? 120 : 30));
        centered_label(c, medium_, app_.tr(text), b, 12, enabled ? t.text : t.muted, 5, 5);
    };
    label(c, regular_, app_.tr("Profile name"), r.x + 28, r.y + 99, 11, t.muted);
    const Rect input{r.x + 28, r.y + 108, r.width - 240, 36};
    focus_target(input);
    fill(c, input, 8, t.background);
    border(c, input, 8, SkColorSetA(t.accent, profile_input_focus_ ? 220 : 85));
    draw_editor(c, profile_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                profile_input_focus_);
    button({r.x + r.width - 200, r.y + 108, 172, 36},
           profile_edit_ >= 0 ? "Save name" : "Create and open", catalog.error.empty());
    const auto error = profile_error_.empty() ? app_.tr(catalog.error) : profile_error_;
    label(c, regular_, error, r.x + 28, r.y + 172, 11, t.text, r.width - 56);
    const auto ids = listed_profiles();
    const int capacity = std::max(1, (r.height - 236) / 84);
    profiles_scroll_ = std::clamp(profiles_scroll_, 0, std::max(0, int(ids.size()) - capacity));
    if (ids.empty())
        label(c, regular_, app_.tr("No archived profiles. Archiving keeps all files."), r.x + 28,
              r.y + 210, 13, t.muted, r.width - 56);
    for (int position = profiles_scroll_;
         position < int(ids.size()) && position < profiles_scroll_ + capacity; position++) {
        const int id = ids[position], y = r.y + 184 + (position - profiles_scroll_) * 84;
        const auto item = std::find_if(catalog.items.begin(), catalog.items.end(),
                                       [&](const auto &p) { return p.id == id; });
        if (item == catalog.items.end())
            continue;
        label(c, medium_, catalog.name(id, app_.profile.language), r.x + 28, y + 19, 13, t.text,
              r.width - 190);
        if (id == catalog.current_id)
            label(c, regular_, app_.tr("Current profile"), r.x + r.width - 158, y + 19, 10,
                  t.accent, 130);
        button({r.x + 28, y + 34, 104, 32}, "Open");
        button({r.x + 140, y + 34, 112, 32}, "Rename", catalog.error.empty());
        button({r.x + 260, y + 34, 108, 32}, item->archived ? "Restore profile" : "Archive",
               id != 0 && (item->archived || id != catalog.current_id) && catalog.error.empty());
        button({r.x + 376, y + 34, 92, 32}, "Files");
    }
    button({r.x + 28, r.y + r.height - 48, 180, 32},
           profiles_archived_ ? "Show active profiles" : "Show archived profiles");
    label(c, regular_, app_.tr("Archiving keeps all profile files."), r.x + 222,
          r.y + r.height - 27, 10, t.muted, r.width - 250);
}
void BrowserUi::profiles_pointer(Rect r, int x, int y) {
    auto &catalog = app_.state()->profiles;
    if (inside({r.x + 28, r.y + 108, r.width - 240, 36}, x, y)) {
        profile_input_focus_ = true;
        keyboard_focus_ = -1;
        return;
    }
    if (inside({r.x + r.width - 200, r.y + 108, 172, 36}, x, y)) {
        save_profile();
        return;
    }
    profile_input_focus_ = false;
    if (inside({r.x + 28, r.y + r.height - 48, 180, 32}, x, y)) {
        profiles_archived_ = !profiles_archived_;
        profiles_scroll_ = 0;
        app_.changed();
        return;
    }
    const auto ids = listed_profiles();
    const int position = (y - r.y - 184) / 84 + profiles_scroll_;
    if (y < r.y + 184 || position < 0 || position >= int(ids.size()))
        return;
    const int id = ids[position], top = r.y + 184 + (position - profiles_scroll_) * 84;
    const auto item = std::find_if(catalog.items.begin(), catalog.items.end(),
                                   [&](const auto &p) { return p.id == id; });
    if (item == catalog.items.end())
        return;
    if (inside({r.x + 28, top + 34, 104, 32}, x, y)) {
        app_.open_profile(id);
        return;
    }
    if (inside({r.x + 140, top + 34, 112, 32}, x, y)) {
        profile_edit_ = id;
        profile_input_.set(item->name, true);
        profile_input_focus_ = true;
        app_.changed();
        return;
    }
    if (inside({r.x + 260, top + 34, 108, 32}, x, y)) {
        try {
            if (catalog.archive(id, !item->archived)) {
                app_.profile.profiles_root = utf8_path(catalog.root);
                app_.changed(true);
            }
        } catch (const std::exception &e) {
            profile_error_ = app_.tr(e.what());
            app_.changed();
        }
    } else if (inside({r.x + 376, top + 34, 92, 32}, x, y))
        app_.open_directory(catalog.data_directory(id));
}
std::string BrowserUi::profiles_report() const {
    if (panel_ != Profiles)
        return "null";
    const auto r = panel_bounds();
    const auto &catalog = app_.state()->profiles;
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out =
        "{\"input\":" + rect({r.x + 28, r.y + 108, r.width - 240, 36}) +
        ",\"save\":" + rect({r.x + r.width - 200, r.y + 108, 172, 36}) +
        ",\"toggle\":" + rect({r.x + 28, r.y + r.height - 48, 180, 32}) +
        ",\"input_length\":" + std::to_string(profile_input_.value.size()) +
        ",\"error\":" + json_quote(profile_error_.empty() ? catalog.error : profile_error_) +
        ",\"archived\":" + (profiles_archived_ ? "true" : "false") + ",\"items\":[";
    const auto ids = listed_profiles();
    const int capacity = std::max(1, (r.height - 236) / 84);
    for (int i = profiles_scroll_; i < int(ids.size()) && i < profiles_scroll_ + capacity; i++) {
        if (i != profiles_scroll_)
            out += ",";
        const int id = ids[i], y = r.y + 184 + (i - profiles_scroll_) * 84;
        out += "{\"id\":" + std::to_string(id) +
               ",\"name\":" + json_quote(catalog.name(id, app_.profile.language)) +
               ",\"directory\":" + json_quote(utf8_path(catalog.data_directory(id))) +
               ",\"open\":" + rect({r.x + 28, y + 34, 104, 32}) +
               ",\"rename\":" + rect({r.x + 140, y + 34, 112, 32}) +
               ",\"archive\":" + rect({r.x + 260, y + 34, 108, 32}) + "}";
    }
    return out + "]}";
}

void BrowserUi::open_commands() {
    move_window_tab_ = 0;
    app_.close_menu();
    panel(Commands);
    command_query_.set("");
    command_selected_ = command_scroll_ = 0;
    if (auto tab = app_.current(); tab && tab->browser)
        tab->browser->GetHost()->SetFocus(false);
    app_.changed();
}
std::vector<BrowserUi::CommandResult> BrowserUi::command_results() const {
    std::vector<CommandResult> results;
    const auto query = fold_settings(command_query_.value);
    auto add = [&](CommandResult result, const std::string &english) {
        const auto title = fold_settings(result.title), alternate = fold_settings(english);
        const auto haystack =
            title + " " + alternate + " " + fold_settings(result.detail + " " + result.id);
        std::size_t begin = 0;
        while (begin < query.size()) {
            begin = query.find_first_not_of(" \t", begin);
            if (begin == std::string::npos)
                break;
            const auto end = query.find_first_of(" \t", begin);
            const auto word = query.substr(begin, end == std::string::npos ? end : end - begin);
            if (haystack.find(word) == std::string::npos)
                return;
            begin = end;
        }
        result.score =
            query.empty()                                                ? 0
            : title == query || alternate == query || result.id == query ? 0
            : title.starts_with(query) || alternate.starts_with(query)   ? 1
            : title.find(query) != std::string::npos || alternate.find(query) != std::string::npos
                ? 2
                : 3;
        results.push_back(std::move(result));
    };
    if (move_window_tab_) {
        const auto source = app_.tab(move_window_tab_);
        if (!source)
            return results;
        for (auto *target : app_.state()->windows) {
            if (target == &app_ || target->quitting ||
                target->private_window != source->private_tab)
                continue;
            const auto problem = app_.tab_transfer_problem(move_window_tab_, *target);
            const auto title =
                app_.tr("Window") + " " + std::to_string(target->window_uid) + ": " +
                (target->current() ? target->current()->page.title : app_.tr("New tab"));
            add({"window_move", std::to_string(target->window_uid), title,
                 problem.empty() ? app_.tr("Move this tab without reloading") : app_.tr(problem),
                 "", move_window_tab_, 0, problem.empty()},
                "Move to window");
        }
        return results;
    }
    const auto current = app_.current();
    const bool private_mode = current && current->private_tab;
    for (const auto &tab : app_.tabs) {
        if (tab->closing || tab->private_tab != private_mode)
            continue;
        std::string detail = app_.tr("Tab") + " · " + app_.profile.workspace_label(tab->workspace);
        if (tab->container)
            detail += " · " + app_.profile.container_label(tab->container);
        if (!tab->ntp)
            detail += " · " + tab->page.url;
        add({"tab", std::to_string(tab->id), tab->ntp ? app_.tr("New tab") : tab->page.title,
             detail, "", tab->id},
            tab->ntp ? "New tab" : tab->page.title);
    }
    const bool page = current && current->browser && !current->ntp;
    const struct {
        const char *id, *title, *shortcut;
        bool enabled;
    } actions[] = {
        {"new_tab", "New tab", "Ctrl+T", true},
        {"private_tab", "Private tab", "Ctrl+Alt+N", true},
        {"new_window", "New window", "Ctrl+N", true},
        {"private_window", "New private window", "Ctrl+Shift+N", true},
        {"move_window", "Move to window", "", current != nullptr},
        {"back", "Back", "Alt+Left", current && current->back},
        {"forward", "Forward", "Alt+Right", current && current->forward},
        {"reload", "Reload", "Ctrl+R", page},
        {"bookmark", "Save bookmark", "Ctrl+D", page},
        {"find", "Find in page", "Ctrl+F", page},
        {"bookmarks", "Bookmarks", "Ctrl+B", true},
        {"history", "History", "Ctrl+H", true},
        {"downloads", "Downloads", "Ctrl+J", true},
        {"settings", "Settings", "", true},
        {"hotkeys", "Keyboard shortcuts", "", true},
        {"workspaces", "Workspaces", "Ctrl+Shift+E", !app_.private_window},
        {"groups", "Tab groups", "Ctrl+Alt+G", !app_.private_window},
        {"containers", "Site containers", "Ctrl+Alt+C", !private_mode},
        {"profiles", "Profiles", "Ctrl+Alt+P", true},
        {"extensions", "Manage extensions", "", true},
        {"passwords", "Passwords", "Ctrl+Shift+Y", true},
        {"filters", "Filter lists", "", true},
        {"import", "Import browser data", "", true},
        {"diagnostics", "Diagnostics", "", true},
        {"updates", "About Aura and updates", "", true},
        {"quick_links", "Manage quick links", "", current && current->ntp},
        {"split", app_.split_enabled() ? "Exit split view" : "Two pages side by side", "Ctrl+Alt+V",
         page},
        {"sleep", "Sleep tab", "Ctrl+Alt+S", page && !private_mode && !current->pinned},
        {"reader", current && current->reader_active ? "Exit reader" : "Reader mode", "Ctrl+Alt+R",
         page},
        {"translate_page", "Translate page", "Ctrl+Alt+T", page},
        {"translate_selection", "Translate selection", "", page && !current->selected_text.empty()},
        {"permissions", "Site permissions", "Ctrl+Alt+I", true},
        {"site_data", "Site data", "Ctrl+Alt+D", true},
        {"sessions", "Session snapshots", "Ctrl+Alt+E", true},
        {"screenshot", "Screenshot", "Ctrl+Shift+S", page},
        {"devtools", "Developer tools", "F12", page},
        {"task_manager", "Task manager", "Shift+Esc", true},
        {"read_later", "Read later", "", true},
        {"spelling_dictionary", "Spelling dictionary", "", true},
        {"search_providers", "Search engines", "", true},
        {"duplicates", "Duplicate tabs", "", true},
        {"tree_toggle", current && current->tree_collapsed ? "Expand branch" : "Collapse branch",
         "",
         current && app_.profile.tree_tabs &&
             std::any_of(app_.tabs.begin(), app_.tabs.end(),
                         [&](const auto &t) { return t->parent == current->id; })},
        {"tree_detach", "Detach tab", "",
         current && app_.profile.tree_tabs && current->parent != 0},
        {"tree_child", "New child tab", "", current && app_.profile.tree_tabs},
        {"save_article", "Save article offline", "", page && !private_mode},
    };
    for (const auto &action : actions) {
        const std::string_view id = action.id;
        const bool paused =
            app_.safe_mode() &&
            (id == "filters" || id == "sleep" || id == "reader" || id == "screenshot" ||
             id == "spelling_dictionary" || id == "duplicates" || id == "save_article");
        add({"action", action.id, app_.tr(action.title),
             app_.tr(paused ? "This tool is paused in safe mode." : "Action"),
             shortcut_hint(action.id), 0, 0, action.enabled && !paused},
            action.title);
    }
    for (const auto &spec : settings_catalog())
        add({"setting", spec.id, app_.tr(spec.title),
             app_.tr("Setting") + " · " + app_.tr(spec.section) + " · " +
                 app_.tr(spec.description)},
            spec.title + " " + spec.description);
    std::stable_sort(results.begin(), results.end(),
                     [](const auto &a, const auto &b) { return a.score < b.score; });
    if (results.size() > 256)
        results.resize(256);
    return results;
}
void BrowserUi::move_command(int delta) {
    const int count = int(command_results().size());
    command_selected_ = std::clamp(command_selected_ + delta, 0, std::max(0, count - 1));
    keyboard_focus_ = -1;
    app_.changed();
}
void BrowserUi::draw_commands(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    label(c, regular_,
          app_.tr(move_window_tab_ ? "Choose a window for this tab"
                                   : "Search tabs in this window, settings and actions"),
          r.x + 28, r.y + 74, 11, t.muted, r.width - 56);
    const Rect input{r.x + 28, r.y + 86, r.width - 56, 38};
    fill(c, input, 8, t.background);
    border(c, input, 8, t.accent);
    draw_editor(c, command_query_, {input.x + 12, input.y + 3, input.width - 24, 32}, 13, true);
    const auto results = command_results();
    const int capacity = std::max(1, (r.height - 180) / 52);
    command_selected_ = std::clamp(command_selected_, 0, std::max(0, int(results.size()) - 1));
    command_scroll_ = std::clamp(command_scroll_, std::max(0, command_selected_ - capacity + 1),
                                 command_selected_);
    if (results.empty())
        label(c, regular_,
              app_.tr(move_window_tab_ ? "Open another window with the same browsing mode."
                                       : "No matches. Try a tab title, setting or action."),
              r.x + 28, r.y + 165, 13, t.muted, r.width - 56);
    for (int i = command_scroll_; i < int(results.size()) && i < command_scroll_ + capacity; i++) {
        const auto &result = results[i];
        Rect row{r.x + 20, r.y + 136 + (i - command_scroll_) * 52, r.width - 40, 48};
        if (i == command_selected_) {
            fill(c, row, 8, SkColorSetA(t.accent, 25));
            border(c, row, 8, SkColorSetA(t.accent, 140));
        } else if (inside(row, hover_x_, hover_y_))
            fill(c, row, 8, SkColorSetA(t.accent, 10));
        const int shortcut_width = result.shortcut.empty() ? 0 : 120;
        label(c, medium_, result.title, row.x + 12, row.y + 19, 13,
              result.enabled ? t.text : t.muted, row.width - 24 - shortcut_width);
        label(c, regular_, result.detail, row.x + 12, row.y + 38, 10, t.muted, row.width - 24);
        if (!result.shortcut.empty())
            label(c, regular_, result.shortcut, row.x + row.width - 116, row.y + 19, 10, t.muted,
                  104);
    }
    label(c, regular_, app_.tr("Arrows / Tab: select    Enter: run    Esc: close"), r.x + 28,
          r.y + r.height - 18, 10, t.muted, r.width - 56);
}
void BrowserUi::commands_pointer(Rect r, int x, int y) {
    const Rect input{r.x + 28, r.y + 86, r.width - 56, 38};
    if (inside(input, x, y)) {
        command_query_.select_all();
        app_.changed();
        return;
    }
    const int index = command_scroll_ + (y - r.y - 136) / 52,
              capacity = std::max(1, (r.height - 180) / 52);
    if (y >= r.y + 136 && y < r.y + 136 + capacity * 52 && x >= r.x + 20 &&
        x < r.x + r.width - 20) {
        command_selected_ = index;
        run_command(index);
    }
}
void BrowserUi::run_command(int index) {
    const auto results = command_results();
    if (index < 0 || index >= int(results.size()) || !results[index].enabled)
        return;
    const auto result = results[index];
    dismiss();
    command_query_.set("");
    if (result.kind == "window_move") {
        for (auto *target : app_.state()->windows)
            if (std::to_string(target->window_uid) == result.id) {
                app_.move_tab_to_window(result.tab, *target);
                break;
            }
        return;
    }
    if (result.kind == "tab") {
        for (int i = 0; i < int(app_.tabs.size()); i++)
            if (app_.tabs[i]->id == result.tab) {
                app_.activate(i);
                return;
            }
        return;
    }
    if (result.kind == "setting") {
        panel(Settings);
        settings_search_.set(result.id);
        settings_scroll_ = 0;
        settings_search_focus_ = false;
        return;
    }
    const auto &id = result.id;
    if (id == "new_tab")
        app_.open_new_tab(app_.current() && app_.current()->private_tab);
    else if (id == "private_tab")
        app_.open_new_tab(true);
    else if (id == "new_window")
        app_.new_window();
    else if (id == "private_window")
        app_.new_window(true);
    else if (id == "move_window" && app_.current())
        open_tab_windows(app_.current()->id);
    else if (id == "back")
        app_.back();
    else if (id == "forward")
        app_.forward();
    else if (id == "reload")
        app_.reload(false);
    else if (id == "bookmark")
        app_.bookmark();
    else if (id == "find") {
        find_open = true;
        find.select_all();
    } else if (id == "bookmarks")
        panel(Bookmarks);
    else if (id == "history")
        panel(History);
    else if (id == "downloads")
        panel(Downloads);
    else if (id == "settings") {
        panel(Settings);
        settings_search_.set("");
        settings_scroll_ = 0;
    } else if (id == "workspaces")
        open_workspaces();
    else if (id == "groups")
        open_groups();
    else if (id == "containers")
        open_containers();
    else if (id == "profiles")
        open_profiles();
    else if (id == "extensions")
        panel(Extensions);
    else if (id == "passwords")
        panel(Passwords);
    else if (id == "filters")
        panel(Filters);
    else if (id == "import")
        panel(Import);
    else if (id == "diagnostics")
        open_diagnostics();
    else if (id == "updates")
        panel(Updates);
    else if (id == "quick_links")
        panel(Shortcuts);
    else if (id == "split")
        app_.toggle_split();
    else if (id == "sleep") {
        if (auto tab = app_.current())
            app_.sleep_tab(tab->id);
    } else if (id == "reader")
        app_.toggle_reader();
    else if (id == "translate_page")
        open_translation(false);
    else if (id == "translate_selection")
        open_translation(true);
    else if (id == "permissions")
        open_permissions();
    else if (id == "site_data")
        open_site_data();
    else if (id == "sessions")
        open_sessions();
    else if (id == "screenshot")
        open_screenshot();
    else if (id == "tree_toggle") {
        if (auto t = app_.current())
            app_.toggle_tree(t->id);
    } else if (id == "tree_detach") {
        if (auto t = app_.current())
            app_.detach_tree_tab(t->id);
    } else if (id == "tree_child") {
        if (auto t = app_.current())
            app_.new_tab("aura://newtab", t->private_tab, true, false, t->workspace, t->group,
                         t->container, t->id);
    } else if (id == "duplicates")
        open_duplicates();
    else if (id == "spelling_dictionary")
        open_spelling_dictionary();
    else if (id == "search_providers")
        open_search_providers();
    else if (id == "read_later")
        open_reading_list();
    else if (id == "save_article")
        app_.save_reading_article();
    else if (id == "task_manager")
        open_task_manager();
    else if (id == "hotkeys")
        open_hotkeys();
    else if (id == "devtools")
        app_.devtools();
    app_.changed();
}
std::string BrowserUi::commands_report() const {
    if (panel_ != Commands)
        return "null";
    const auto r = panel_bounds();
    const auto results = command_results();
    std::string out = "{\"query\":" + json_quote(command_query_.value) +
                      ",\"selected\":" + std::to_string(command_selected_) +
                      ",\"count\":" + std::to_string(results.size()) + ",\"items\":[";
    const int capacity = std::max(1, (r.height - 180) / 52);
    for (int i = command_scroll_; i < int(results.size()) && i < command_scroll_ + capacity; i++) {
        if (i != command_scroll_)
            out += ",";
        const auto &result = results[i];
        out += "{\"kind\":" + json_quote(result.kind) + ",\"id\":" + json_quote(result.id) +
               ",\"title\":" + json_quote(result.title) +
               ",\"enabled\":" + (result.enabled ? "true" : "false") + ",\"bounds\":[" +
               std::to_string(r.x + 20) + "," +
               std::to_string(r.y + 136 + (i - command_scroll_) * 52) + "," +
               std::to_string(r.width - 40) + ",48]}";
    }
    return out + "]}";
}

namespace {
Rect translation_choice(Rect r, int group, int index) {
    const int columns = group == 1 ? 4 : 2;
    const int width = (r.width - 56 - (columns - 1) * 8) / columns;
    const int top = group == 0 ? 108 : group == 1 ? 174 : 280;
    return {r.x + 28 + (index % columns) * (width + 8), r.y + top + (index / columns) * 38, width,
            32};
}
} // namespace
void BrowserUi::open_translation(bool selection, int source_tab) {
    auto t = source_tab ? app_.tab(source_tab) : app_.current();
    if (!t || t->ntp || !t->browser) {
        toast(app_.tr("Translation is available for HTTP and HTTPS pages."));
        return;
    }
    translation_tab_ = t->id;
    translation_serial_ = t->navigation_serial;
    translation_text_ = t->selected_text;
    translation_source_ = t->page.url;
    translation_selection_ = selection;
    app_.close_menu();
    panel(Translation);
    t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
std::string BrowserUi::translation_problem() const {
    auto t = app_.tab(translation_tab_);
    if (!t || t->closing || t->navigation_serial != translation_serial_)
        return app_.tr("The source page changed. Reopen translation.");
    try {
        translation_url(
            app_.profile.translation_provider,
            translation_target(app_.profile.translation_language, app_.profile.language),
            !translation_selection_,
            translation_selection_ ? translation_text_ : translation_source_);
    } catch (const std::exception &e) {
        return app_.tr(e.what());
    }
    return {};
}
void BrowserUi::send_translation() {
    if (!translation_problem().empty()) {
        app_.changed();
        return;
    }
    auto t = app_.tab(translation_tab_);
    if (!t)
        return;
    const bool private_tab = t->private_tab;
    const int workspace = t->workspace, group = t->group, container = t->container;
    const auto url = translation_url(
        app_.profile.translation_provider,
        translation_target(app_.profile.translation_language, app_.profile.language),
        !translation_selection_, translation_selection_ ? translation_text_ : translation_source_);
    dismiss();
    ++app_.audit["translation_requests"];
    app_.new_tab(url, private_tab, true, false, workspace, group, container);
}
void BrowserUi::draw_translation(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    label(c, regular_, app_.tr("Nothing is sent until you choose Open translation."), r.x + 28,
          r.y + 73, 11, t.muted, r.width - 56);
    auto choice = [&](Rect b, const std::string &title, bool selected, bool enabled = true,
                      bool mark = true) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8,
             SkColorSetA(t.accent, selected                        ? 30
                                   : inside(b, hover_x_, hover_y_) ? 18
                                                                   : 6));
        border(c, b, 8, SkColorSetA(selected ? t.accent : t.muted, enabled ? 190 : 65));
        if (selected && mark)
            icon(c, "check", b.x + 7, b.y + 10, 12, t.accent);
        centered_label(c, medium_, app_.tr(title),
                       {b.x + (mark ? 18 : 0), b.y, b.width - (mark ? 18 : 0), b.height}, 12,
                       enabled ? t.text : t.muted, 6, 6);
    };
    const auto &p = app_.profile;
    label(c, regular_, app_.tr("Translation provider"), r.x + 28, r.y + 99, 11, t.muted);
    for (int i = 0; i < 2; i++) {
        const auto &option = find_setting("translation_provider")->choices[i];
        choice(translation_choice(r, 0, i), option.label, p.translation_provider == option.value);
    }
    label(c, regular_, app_.tr("Target language"), r.x + 28, r.y + 164, 11, t.muted);
    const auto &languages = find_setting("translation_language")->choices;
    for (int i = 0; i < int(languages.size()); i++)
        choice(translation_choice(r, 1, i), languages[i].label,
               p.translation_language == languages[i].value);
    label(c, regular_, app_.tr("Content to send"), r.x + 28, r.y + 270, 11, t.muted);
    choice(translation_choice(r, 2, 0), "Public page (URL)", !translation_selection_,
           p.translation_provider == "google");
    choice(translation_choice(r, 2, 1), "Selected text", translation_selection_);
    const auto problem = translation_problem();
    label(c, medium_, app_.tr(translation_selection_ ? "Selected text" : "Page address"), r.x + 28,
          r.y + 340, 11, t.muted);
    const auto preview = problem.empty()
                             ? (translation_selection_ ? translation_text_ : translation_source_)
                             : problem;
    const auto lines = wrap_text(font(regular_, 12), preview, r.width - 56, 2);
    for (int i = 0; i < int(lines.size()); i++)
        label(c, regular_, lines[i], r.x + 28, r.y + 360 + i * 18, 12, t.text, r.width - 56);
    const auto disclosure =
        app_.tr(translation_selection_
                    ? "Sends the selected text to the chosen provider in a new tab."
                    : "Sends this address to Google Translate. Signed-in pages may not translate.");
    const auto info = wrap_text(font(regular_, 10), disclosure, r.width - 56, 2);
    for (int i = 0; i < int(info.size()); i++)
        label(c, regular_, info[i], r.x + 28, r.y + r.height - 88 + i * 14, 10, t.muted,
              r.width - 56);
    choice({r.x + 28, r.y + r.height - 52, 132, 34}, "Cancel", false, true, false);
    choice({r.x + r.width - 228, r.y + r.height - 52, 200, 34}, "Open translation", true,
           problem.empty(), false);
}
void BrowserUi::translation_pointer(Rect r, int x, int y) {
    if (inside({r.x + 28, r.y + r.height - 52, 132, 34}, x, y)) {
        dismiss();
        return;
    }
    if (inside({r.x + r.width - 228, r.y + r.height - 52, 200, 34}, x, y)) {
        send_translation();
        return;
    }
    for (int group = 0; group < 3; group++) {
        const auto spec =
            find_setting(group == 0 ? "translation_provider" : "translation_language");
        const int count = group == 2 ? 2 : int(spec->choices.size());
        for (int i = 0; i < count; i++)
            if (inside(translation_choice(r, group, i), x, y)) {
                if (group == 2) {
                    if (i == 1 || app_.profile.translation_provider == "google")
                        translation_selection_ = i == 1;
                } else
                    set_setting(app_.profile, *spec, spec->choices[i].value);
                app_.changed(group != 2);
                return;
            }
    }
}
std::string BrowserUi::translation_report() const {
    if (panel_ != Translation)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"selection\":" + std::string(translation_selection_ ? "true" : "false") +
                      ",\"text_bytes\":" + std::to_string(translation_text_.size()) +
                      ",\"problem\":" + json_quote(translation_problem()) +
                      ",\"send\":" + rect({r.x + r.width - 228, r.y + r.height - 52, 200, 34}) +
                      ",\"cancel\":" + rect({r.x + 28, r.y + r.height - 52, 132, 34}) +
                      ",\"options\":[";
    bool first = true;
    for (int group = 0; group < 3; group++) {
        const auto spec =
            find_setting(group == 0 ? "translation_provider" : "translation_language");
        const int count = group == 2 ? 2 : int(spec->choices.size());
        for (int i = 0; i < count; i++) {
            if (!first)
                out += ",";
            first = false;
            out += "{\"group\":" + std::to_string(group) + ",\"value\":" +
                   json_quote(group == 2 ? (i == 0 ? "page" : "text") : spec->choices[i].value) +
                   ",\"bounds\":" + rect(translation_choice(r, group, i)) + "}";
        }
    }
    return out + "]}";
}
std::string BrowserUi::menu_report() const {
    if (app_.menu.empty())
        return "null";
    std::string out = "[";
    for (int i = 0; i < int(app_.menu.size()); i++) {
        if (i)
            out += ",";
        const auto &item = app_.menu[i];
        out += "{\"id\":" + std::to_string(item.id) + ",\"label\":" + json_quote(item.label) +
               ",\"enabled\":" + (item.enabled ? "true" : "false") + ",\"bounds\":[" +
               std::to_string(app_.menu_x + 5) + "," + std::to_string(app_.menu_y + 6 + i * 32) +
               "," + std::to_string(app_.menu_width - 10) + ",31]}";
    }
    return out + "]";
}

std::vector<BrowserUi::PermissionSite> BrowserUi::permission_sites() const {
    std::vector<PermissionSite> result;
    std::set<std::pair<int, std::string>> seen;
    const auto query = fold_settings(permissions_search_.value);
    auto add = [&](const std::string &origin, int container) {
        if (!valid_permission_origin(origin) || !app_.profile.container(container) ||
            result.size() >= 4096)
            return;
        if (!query.empty() &&
            fold_settings(origin + " " + app_.profile.container_label(container)).find(query) ==
                std::string::npos)
            return;
        if (seen.insert({container, origin}).second)
            result.push_back({origin, container});
    };
    if (auto tab = app_.current(); tab && tab->private_tab == permissions_private_) {
        auto origin = permission_origin(
            tab->browser ? tab->browser->GetMainFrame()->GetURL().ToString() : tab->page.url);
        if (origin.empty() && !tab->media_capture)
            origin = permission_origin(tab->page.url);
        add(origin, tab->container);
    }
    const auto &rules = permissions_private_ ? app_.state()->private_site_permissions
                                             : app_.profile.site_permissions;
    for (const auto &rule : rules)
        add(rule.origin, rule.container);
    for (const auto *window : app_.state()->windows)
        for (const auto &tab : window->tabs)
            if (tab->private_tab == permissions_private_ && !tab->closing)
                for (const auto &origin : tab->permission_origins)
                    add(origin, tab->container);
    return result;
}
void BrowserUi::open_permissions() {
    permissions_error_.clear();
    permissions_pick_selected_ = permissions_scroll_ = 0;
    permissions_search_.set("");
    permissions_search_focus_ = false;
    permissions_origin_.clear();
    permissions_container_ = 0;
    auto tab = app_.current();
    permissions_private_ = tab && tab->private_tab;
    if (tab) {
        permissions_container_ = tab->container;
        permissions_origin_ = permission_origin(
            tab->browser ? tab->browser->GetMainFrame()->GetURL().ToString() : tab->page.url);
        if (permissions_origin_.empty() && !tab->media_capture)
            permissions_origin_ = permission_origin(tab->page.url);
    }
    permissions_picker_ = permissions_origin_.empty();
    permissions_search_focus_ = permissions_picker_;
    app_.close_menu();
    panel(Permissions);
    if (tab && tab->browser)
        tab->browser->GetHost()->SetFocus(false);
    app_.changed();
}
void BrowserUi::draw_permissions(SkCanvas &c, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    const int capacity = std::max(1, (r.height - 274) / 52);
    auto button = [&](Rect b, const std::string &title, bool enabled = true, bool primary = false) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8,
             SkColorSetA(theme.accent, primary                                    ? 30
                                       : enabled && inside(b, hover_x_, hover_y_) ? 24
                                                                                  : 8));
        border(c, b, 8, SkColorSetA(enabled ? theme.accent : theme.muted, enabled ? 140 : 55));
        centered_label(c, medium_, app_.tr(title), b, 12, enabled ? theme.text : theme.muted, 7, 7);
    };
    label(c, regular_,
          app_.tr(permissions_picker_ ? "Find a site in this permission context."
                                      : "Choose a permission or revoke access for this site."),
          r.x + 28, r.y + 73, 11, theme.muted, r.width - 56);
    if (permissions_picker_) {
        label(c, regular_, app_.tr("Search sites"), r.x + 28, r.y + 101, 11, theme.muted);
        Rect input{r.x + 28, r.y + 112, r.width - 56, 36};
        focus_target(input);
        fill(c, input, 8, theme.background);
        border(c, input, 8, SkColorSetA(theme.accent, permissions_search_focus_ ? 255 : 110));
        draw_editor(c, permissions_search_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                    permissions_search_focus_);
        const auto sites = permission_sites();
        permissions_scroll_ =
            std::clamp(permissions_scroll_, 0, std::max(0, int(sites.size()) - capacity));
        if (sites.empty())
            label(c, regular_, app_.tr("No saved sites. Open a website to manage its permissions."),
                  r.x + 28, r.y + 200, 13, theme.muted, r.width - 56);
        for (int i = permissions_scroll_;
             i < int(sites.size()) && i < permissions_scroll_ + capacity; i++) {
            const auto &site = sites[i];
            Rect row{r.x + 20, r.y + 174 + (i - permissions_scroll_) * 52, r.width - 40, 48};
            focus_target(row);
            if (permissions_search_focus_ && i == permissions_pick_selected_) {
                fill(c, row, 8, SkColorSetA(theme.accent, 20));
                border(c, row, 8, theme.accent);
            } else if (inside(row, hover_x_, hover_y_))
                fill(c, row, 8, SkColorSetA(theme.accent, 16));
            label(c, medium_, site.origin, row.x + 8, row.y + 19, 13, theme.text, row.width - 24);
            label(c, regular_, app_.profile.container_label(site.container), row.x + 8, row.y + 38,
                  10, theme.muted, row.width - 24);
        }
        button({r.x + 28, r.y + r.height - 68, 140, 34},
               permissions_origin_.empty() ? "Close" : "Back");
    } else {
        label(c, medium_, permissions_origin_, r.x + 28, r.y + 102, 13, theme.text, r.width - 56);
        label(c, regular_,
              app_.profile.container_label(permissions_container_) +
                  (permissions_private_ ? " · " + app_.tr("Private browsing") : ""),
              r.x + 28, r.y + 123, 10, theme.muted, r.width - 56);
        button({r.x + 28, r.y + 134, 180, 30}, "Saved sites");
        if (auto tab = app_.current(); tab && tab->media_capture)
            label(c, regular_,
                  app_.tr(tab->media_video && tab->media_audio
                              ? "Current tab: camera and microphone active"
                          : tab->media_video ? "Current tab: camera active"
                                             : "Current tab: microphone active"),
                  r.x + 224, r.y + 154, 10, theme.accent, r.width - 252);
        const auto specs = permission_catalog();
        permissions_scroll_ =
            std::clamp(permissions_scroll_, 0, std::max(0, int(specs.size()) - capacity));
        const auto &rules = permissions_private_ ? app_.state()->private_site_permissions
                                                 : app_.profile.site_permissions;
        for (int i = permissions_scroll_;
             i < int(specs.size()) && i < permissions_scroll_ + capacity; i++) {
            const auto &spec = specs[i];
            const int y = r.y + 174 + (i - permissions_scroll_) * 52;
            const auto override =
                permission_override(rules, permissions_origin_, permissions_container_, spec.id);
            const auto native = app_.native_permission_value(
                permissions_origin_, permissions_container_, permissions_private_, spec.id);
            const std::string state = native == "allow"   ? "Allowed"
                                      : native == "block" ? "Blocked"
                                      : native == "ask"   ? "Ask"
                                                          : "Applies when this site opens";
            label(c, medium_, app_.tr(spec.title), r.x + 28, y + 18, 13, theme.text, r.width - 340);
            label(c, regular_,
                  app_.tr(permissions_private_ && std::string(spec.id) == "notifications"
                              ? "Blocked in private mode"
                          : !trustworthy_permission_origin(permissions_origin_)
                              ? "Requires HTTPS or localhost"
                              : state),
                  r.x + 28, y + 37, 10, theme.muted, r.width - 340);
            button({r.x + r.width - 304, y + 7, 180, 32}, override == "allow"   ? "Allow"
                                                          : override == "block" ? "Block"
                                                          : override == "ask"   ? "Ask"
                                                                                : "Default");
            button({r.x + r.width - 116, y + 7, 88, 32}, "Revoke", native != "block");
        }
        button({r.x + 28, r.y + r.height - 68, 132, 34}, "Close");
        button({r.x + r.width - 228, r.y + r.height - 68, 200, 34}, "Reset permissions", true,
               true);
    }
    const int total =
        permissions_picker_ ? int(permission_sites().size()) : int(permission_catalog().size());
    if (total > capacity) {
        const int track = capacity * 52 - 4, thumb = std::max(24, track * capacity / total);
        fill(c, {r.x + r.width - 13, r.y + 174, 3, track}, 1, SkColorSetA(theme.muted, 35));
        fill(c,
             {r.x + r.width - 13,
              r.y + 174 + (track - thumb) * permissions_scroll_ / (total - capacity), 3, thumb},
             1, SkColorSetA(theme.accent, 180));
    }
    label(c, regular_,
          permissions_error_.empty()
              ? app_.tr(permissions_private_
                            ? "Private rules are cleared when Aura closes."
                            : "Permissions apply to this site in the selected container.")
              : permissions_error_,
          r.x + 28, r.y + r.height - 92, 10, theme.muted, r.width - 56);
}
void BrowserUi::choose_permission(const std::string &id) {
    const auto spec = find_permission(id);
    if (!spec)
        return;
    const auto r = panel_bounds();
    const auto specs = permission_catalog();
    const int index = int(spec - specs.data());
    const int y = r.y + 174 + (index - permissions_scroll_) * 52;
    const auto &rules = permissions_private_ ? app_.state()->private_site_permissions
                                             : app_.profile.site_permissions;
    const auto current =
        permission_override(rules, permissions_origin_, permissions_container_, id);
    app_.close_menu();
    app_.menu.clear();
    const char *values[] = {"default", "ask", "allow", "block"},
               *labels[] = {"Default", "Ask", "Allow", "Block"};
    for (int i = 0; i < 4; i++) {
        bool enabled = true;
        if (i == 1 && !spec->ask)
            enabled = false;
        if ((i == 1 || i == 2) && (!trustworthy_permission_origin(permissions_origin_) ||
                                   (permissions_private_ && id == "notifications")))
            enabled = false;
        app_.menu.push_back(
            {i, app_.tr(labels[i]), enabled, current == values[i] ? app_.tr("Current") : ""});
    }
    app_.menu_width = 280;
    app_.menu_x = std::clamp(r.x + r.width - 304, 8, width_ - 288);
    app_.menu_y = std::clamp(y + 42, 32, std::max(32, height_ - 152));
    const auto origin = permissions_origin_;
    const int container = permissions_container_;
    const bool private_mode = permissions_private_;
    app_.menu_action = [this, id, origin, container, private_mode](int choice) {
        const char *values[] = {"default", "ask", "allow", "block"};
        if (choice >= 0 && choice < 4) {
            permissions_error_ =
                app_.change_site_permission(origin, container, private_mode, id, values[choice])
                    ? ""
                    : app_.tr("Could not save this site rule.");
            app_.changed();
        }
    };
    app_.changed();
}
void BrowserUi::permissions_pointer(Rect r, int x, int y) {
    if (permissions_picker_) {
        if (inside({r.x + 28, r.y + 112, r.width - 56, 36}, x, y)) {
            permissions_search_focus_ = true;
            permissions_search_.select_all();
            return;
        }
        if (inside({r.x + 28, r.y + r.height - 68, 140, 34}, x, y)) {
            if (permissions_origin_.empty())
                dismiss();
            else {
                permissions_picker_ = false;
                permissions_search_focus_ = false;
                permissions_scroll_ = 0;
                app_.changed();
            }
            return;
        }
        const auto sites = permission_sites();
        const int index = permissions_scroll_ + (y - r.y - 174) / 52;
        if (y >= r.y + 174 && y < r.y + 174 + std::max(1, (r.height - 274) / 52) * 52 &&
            index >= 0 && index < int(sites.size())) {
            permissions_origin_ = sites[index].origin;
            permissions_container_ = sites[index].container;
            permissions_picker_ = false;
            permissions_search_focus_ = false;
            permissions_scroll_ = 0;
            permissions_error_.clear();
            app_.changed();
        }
        return;
    }
    if (inside({r.x + 28, r.y + 134, 180, 30}, x, y)) {
        permissions_picker_ = true;
        permissions_search_focus_ = true;
        permissions_search_.set("");
        permissions_pick_selected_ = permissions_scroll_ = 0;
        app_.changed();
        return;
    }
    if (inside({r.x + 28, r.y + r.height - 68, 132, 34}, x, y)) {
        dismiss();
        return;
    }
    if (inside({r.x + r.width - 228, r.y + r.height - 68, 200, 34}, x, y)) {
        app_.reset_site_permissions(permissions_origin_, permissions_container_,
                                    permissions_private_);
        permissions_error_.clear();
        app_.changed();
        return;
    }
    const auto specs = permission_catalog();
    const int capacity = std::max(1, (r.height - 274) / 52);
    for (int i = permissions_scroll_; i < int(specs.size()) && i < permissions_scroll_ + capacity;
         i++) {
        const int top = r.y + 174 + (i - permissions_scroll_) * 52;
        if (inside({r.x + r.width - 304, top + 7, 180, 32}, x, y)) {
            choose_permission(specs[i].id);
            return;
        }
        if (inside({r.x + r.width - 116, top + 7, 88, 32}, x, y)) {
            permissions_error_ =
                app_.change_site_permission(permissions_origin_, permissions_container_,
                                            permissions_private_, specs[i].id, "block")
                    ? ""
                    : app_.tr("Could not save this site rule.");
            app_.changed();
            return;
        }
    }
}
std::string BrowserUi::permissions_report() const {
    if (panel_ != Permissions)
        return "null";
    const auto r = panel_bounds();
    const int capacity = std::max(1, (r.height - 274) / 52);
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"origin\":" + json_quote(permissions_origin_) +
                      ",\"container\":" + std::to_string(permissions_container_) +
                      ",\"private\":" + (permissions_private_ ? "true" : "false") +
                      ",\"picker\":" + (permissions_picker_ ? "true" : "false") +
                      ",\"query\":" + json_quote(permissions_search_.value) +
                      ",\"sites_button\":" + rect({r.x + 28, r.y + 134, 180, 30}) +
                      ",\"search\":" + rect({r.x + 28, r.y + 112, r.width - 56, 36}) +
                      ",\"reset\":" + rect({r.x + r.width - 228, r.y + r.height - 68, 200, 34}) +
                      ",\"error\":" + json_quote(permissions_error_) + ",\"rows\":[";
    const auto &rules = permissions_private_ ? app_.state()->private_site_permissions
                                             : app_.profile.site_permissions;
    const auto specs = permission_catalog();
    if (!permissions_picker_)
        for (int i = permissions_scroll_;
             i < int(specs.size()) && i < permissions_scroll_ + capacity; i++) {
            if (i != permissions_scroll_)
                out += ",";
            const auto &spec = specs[i];
            const int y = r.y + 174 + (i - permissions_scroll_) * 52;
            out +=
                "{\"id\":" + json_quote(spec.id) + ",\"value\":" +
                json_quote(permission_override(rules, permissions_origin_, permissions_container_,
                                               spec.id)) +
                ",\"native\":" +
                json_quote(app_.native_permission_value(permissions_origin_, permissions_container_,
                                                        permissions_private_, spec.id)) +
                ",\"choice\":" + rect({r.x + r.width - 304, y + 7, 180, 32}) +
                ",\"revoke\":" + rect({r.x + r.width - 116, y + 7, 88, 32}) + "}";
        }
    out += "],\"sites\":[";
    const auto sites = permission_sites();
    if (permissions_picker_)
        for (int i = permissions_scroll_;
             i < int(sites.size()) && i < permissions_scroll_ + capacity; i++) {
            if (i != permissions_scroll_)
                out += ",";
            out += "{\"origin\":" + json_quote(sites[i].origin) +
                   ",\"container\":" + std::to_string(sites[i].container) + ",\"bounds\":" +
                   rect({r.x + 20, r.y + 174 + (i - permissions_scroll_) * 52, r.width - 40, 48}) +
                   "}";
        }
    return out + "]}";
}
void BrowserUi::open_site_data() {
    data_picker_ = data_search_focus_ = false;
    data_selected_ = data_scroll_ = 0;
    data_search_.set("");
    app_.close_menu();
    app_.open_site_data();
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
}
std::vector<std::string> BrowserUi::data_sites() const {
    std::vector<std::string> result;
    std::set<std::string> seen;
    const auto query = fold_settings(data_search_.value);
    const auto &data = app_.site_data;
    auto add = [&](const std::string &url) {
        const auto origin = permission_origin(url);
        if (!origin.empty() && seen.insert(origin).second &&
            (query.empty() || fold_settings(origin).find(query) != std::string::npos) &&
            result.size() < 4096)
            result.push_back(origin);
    };
    add(data.origin);
    for (const auto *window : app_.state()->windows)
        for (const auto &tab : window->tabs)
            if (!tab->closing && tab->container == data.container &&
                tab->private_tab == data.private_mode) {
                add(tab->page.url);
                for (const auto &[id, origin] : tab->protection_contexts)
                    add(origin);
            }
    if (!data.private_mode) {
        for (auto it = app_.profile.history.rbegin(); it != app_.profile.history.rend(); ++it)
            add(it->url);
        for (const auto &rule : app_.profile.site_permissions)
            if (rule.container == data.container)
                add(rule.origin);
    }
    return result;
}
void BrowserUi::draw_site_data(SkCanvas &c, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    const auto &data = app_.site_data;
    const bool busy = data.pending || data.clearing,
               ready = !busy && valid_permission_origin(data.origin) && data.error.empty();
    auto button = [&](Rect b, const std::string &text, bool enabled = true) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8, SkColorSetA(theme.accent, enabled && inside(b, hover_x_, hover_y_) ? 24 : 8));
        border(c, b, 8, SkColorSetA(theme.accent, enabled ? 140 : 45));
        centered_label(c, medium_, app_.tr(text), b, 12, enabled ? theme.text : theme.muted, 8, 8);
    };
    label(c, regular_,
          app_.tr(data_picker_
                      ? "Open and recent sites. This is not a complete disk inventory."
                      : "Inspect storage in this container. Cookies are managed separately."),
          r.x + 28, r.y + 73, 11, theme.muted, r.width - 56);
    if (data_picker_) {
        label(c, regular_, app_.tr("Search sites"), r.x + 28, r.y + 101, 11, theme.muted);
        Rect input{r.x + 28, r.y + 112, r.width - 56, 36};
        focus_target(input);
        fill(c, input, 8, theme.background);
        border(c, input, 8, SkColorSetA(theme.accent, data_search_focus_ ? 255 : 110));
        draw_editor(c, data_search_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                    data_search_focus_);
        const auto sites = data_sites();
        const int capacity = std::max(1, (r.height - 230) / 52);
        data_scroll_ = std::clamp(data_scroll_, 0, std::max(0, int(sites.size()) - capacity));
        if (sites.empty())
            label(c, regular_, app_.tr("No matching sites. Open a website to inspect its data."),
                  r.x + 28, r.y + 200, 13, theme.muted, r.width - 56);
        for (int i = data_scroll_; i < int(sites.size()) && i < data_scroll_ + capacity; ++i) {
            Rect row{r.x + 20, r.y + 166 + (i - data_scroll_) * 52, r.width - 40, 48};
            focus_target(row);
            if (data_search_focus_ && i == data_selected_) {
                fill(c, row, 8, SkColorSetA(theme.accent, 20));
                border(c, row, 8, theme.accent);
            } else if (inside(row, hover_x_, hover_y_))
                fill(c, row, 8, SkColorSetA(theme.accent, 16));
            label(c, medium_, sites[i], row.x + 8, row.y + 29, 13, theme.text, row.width - 16);
        }
        if (int(sites.size()) > capacity) {
            const int h = capacity * 52, thumb = std::max(24, h * capacity / int(sites.size()));
            fill(c, {r.x + r.width - 13, r.y + 166, 3, h}, 1, SkColorSetA(theme.muted, 35));
            fill(c,
                 {r.x + r.width - 13,
                  r.y + 166 + (h - thumb) * data_scroll_ / (int(sites.size()) - capacity), 3,
                  thumb},
                 1, theme.accent);
        }
        button({r.x + 28, r.y + r.height - 56, 132, 32}, "Back");
        return;
    }
    label(c, medium_, data.origin.empty() ? app_.tr("No website selected") : data.origin, r.x + 28,
          r.y + 102, 13, theme.text, r.width - 56);
    label(c, regular_,
          app_.profile.container_label(data.container) +
              (data.private_mode ? " / " + app_.tr("Private browsing") : ""),
          r.x + 28, r.y + 123, 10, theme.muted, r.width - 56);
    button({r.x + 28, r.y + 134, 180, 30}, "Choose site", !busy);
    const int half = (r.width - 72) / 2;
    label(c, regular_, app_.tr("Storage estimate"), r.x + 28, r.y + 190, 12, theme.muted, half);
    label(c, regular_, app_.tr("Matching cookies"), r.x + 44 + half, r.y + 190, 12, theme.muted,
          half);
    auto size = [](uint64_t n) {
        char b[64];
        if (n >= 1024 * 1024)
            std::snprintf(b, sizeof(b), "%.1f MiB", double(n) / (1024 * 1024));
        else if (n >= 1024)
            std::snprintf(b, sizeof(b), "%.1f KiB", double(n) / 1024);
        else
            std::snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(n));
        return std::string(b);
    };
    label(c, medium_,
          busy ? app_.tr(data.clearing ? "Deleting..." : "Measuring...") : size(data.usage),
          r.x + 28, r.y + 217, 20, theme.text, half);
    label(c, medium_, busy ? app_.tr("Measuring...") : std::to_string(data.cookies),
          r.x + 44 + half, r.y + 217, 20, theme.text, half);
    const auto info = wrap_text(
        font(regular_, 10),
        app_.tr(
            "Estimate covers quota-managed storage. Local storage and cookies are not included."),
        r.width - 56, 2);
    for (size_t i = 0; i < info.size(); ++i)
        label(c, regular_, info[i], r.x + 28, r.y + 241 + int(i) * 15, 10, theme.muted,
              r.width - 56);
    const std::pair<const char *, const char *> types[] = {{"indexeddb", "Databases"},
                                                           {"cache_storage", "Cache storage"},
                                                           {"service_workers", "Service workers"},
                                                           {"file_systems", "File storage"},
                                                           {"other", "Other storage"}};
    std::vector<std::pair<std::string, uint64_t>> rows;
    uint64_t shown = 0;
    for (const auto &[id, title] : types) {
        const auto found = data.types.find(id);
        const uint64_t amount = std::string(id) == "other"
                                    ? (data.usage > shown ? data.usage - shown : 0)
                                : found == data.types.end() ? 0
                                                            : found->second;
        shown += amount;
        if (amount)
            rows.emplace_back(title, amount);
    }
    const int capacity = std::max(1, (r.height - 469) / 26 + 1);
    if (int(rows.size()) > capacity) {
        uint64_t rest = 0;
        for (size_t i = capacity - 1; i < rows.size(); ++i)
            rest += rows[i].second;
        rows.resize(capacity);
        rows.back() = {"Other storage", rest};
    }
    int y = r.y + 279;
    for (const auto &[title, amount] : rows) {
        label(c, regular_, app_.tr(title), r.x + 28, y, 12, theme.text, r.width - 180);
        label(c, regular_, size(amount), r.x + r.width - 150, y, 12, theme.muted, 122);
        y += 26;
    }
    button({r.x + 28, r.y + r.height - 162, half, 34}, "Delete storage", ready);
    button({r.x + 44 + half, r.y + r.height - 162, half, 34}, "Delete cookies", ready);
    const std::string message =
        !data.error.empty() ? data.error
        : !data.message.empty()
            ? data.message
            : app_.tr(
                  "Open pages can recreate data. Other containers and permissions are unchanged.");
    const auto lines = wrap_text(font(regular_, 11), message, r.width - 56, 3);
    for (size_t i = 0; i < lines.size(); ++i)
        label(c, regular_, lines[i], r.x + 28, r.y + r.height - 103 + int(i) * 15, 11, theme.muted,
              r.width - 56);
    button({r.x + 28, r.y + r.height - 56, 132, 32}, "Refresh", !busy);
    button({r.x + r.width - 160, r.y + r.height - 56, 132, 32}, "Close");
}
void BrowserUi::site_data_pointer(Rect r, int x, int y) {
    auto inside = [&](Rect b) {
        return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
    };
    if (data_picker_) {
        if (inside({r.x + 28, r.y + 112, r.width - 56, 36})) {
            data_search_focus_ = true;
            data_search_.select_all();
            return;
        }
        if (inside({r.x + 28, r.y + r.height - 56, 132, 32})) {
            data_picker_ = data_search_focus_ = false;
            app_.changed();
            return;
        }
        const auto sites = data_sites();
        const int n = data_scroll_ + (y - r.y - 166) / 52, cap = std::max(1, (r.height - 230) / 52);
        if (y >= r.y + 166 && y < r.y + 166 + cap * 52 && n >= 0 && n < int(sites.size())) {
            data_picker_ = data_search_focus_ = false;
            app_.site_data.message.clear();
            app_.refresh_site_data(sites[n]);
        }
        return;
    }
    const bool busy = app_.site_data.pending || app_.site_data.clearing;
    if (inside({r.x + r.width - 160, r.y + r.height - 56, 132, 32})) {
        dismiss();
        return;
    }
    if (busy)
        return;
    if (inside({r.x + 28, r.y + 134, 180, 30})) {
        data_picker_ = data_search_focus_ = true;
        data_search_.set("");
        data_selected_ = data_scroll_ = 0;
        app_.changed();
        return;
    }
    if (inside({r.x + 28, r.y + r.height - 56, 132, 32})) {
        app_.refresh_site_data();
        return;
    }
    if (!app_.site_data.error.empty())
        return;
    const int half = (r.width - 72) / 2;
    if (inside({r.x + 28, r.y + r.height - 162, half, 34}))
        app_.clear_site_data(false);
    else if (inside({r.x + 44 + half, r.y + r.height - 162, half, 34}))
        app_.clear_site_data(true);
}
std::string BrowserUi::site_data_report() const {
    if (panel_ != SiteData)
        return "null";
    const auto &data = app_.site_data;
    const auto r = panel_bounds();
    const int half = (r.width - 72) / 2;
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out =
        "{\"origin\":" + json_quote(data.origin) +
        ",\"container\":" + std::to_string(data.container) +
        ",\"private\":" + (data.private_mode ? "true" : "false") +
        ",\"busy\":" + (data.pending || data.clearing ? "true" : "false") +
        ",\"usage\":" + std::to_string(data.usage) +
        ",\"cookies\":" + std::to_string(data.cookies) + ",\"error\":" + json_quote(data.error) +
        ",\"message\":" + json_quote(data.message) +
        ",\"picker\":" + (data_picker_ ? "true" : "false") +
        ",\"query\":" + json_quote(data_search_.value) +
        ",\"choose\":" + rect({r.x + 28, r.y + 134, 180, 30}) +
        ",\"search\":" + rect({r.x + 28, r.y + 112, r.width - 56, 36}) +
        ",\"storage_delete\":" + rect({r.x + 28, r.y + r.height - 162, half, 34}) +
        ",\"cookies_delete\":" + rect({r.x + 44 + half, r.y + r.height - 162, half, 34}) +
        ",\"refresh\":" + rect({r.x + 28, r.y + r.height - 56, 132, 32}) + ",\"sites\":[";
    const auto sites = data_sites();
    const int cap = std::max(1, (r.height - 230) / 52);
    if (data_picker_)
        for (int i = data_scroll_; i < int(sites.size()) && i < data_scroll_ + cap; ++i) {
            if (i != data_scroll_)
                out += ",";
            out += "{\"origin\":" + json_quote(sites[i]) + ",\"bounds\":" +
                   rect({r.x + 20, r.y + 166 + (i - data_scroll_) * 52, r.width - 40, 48}) + "}";
        }
    return out + "]}";
}
const std::vector<size_t> &BrowserUi::bookmark_results() const {
    const auto query = fold_settings(bookmark_search_.value);
    if (bookmark_result_revision_ == app_.state()->profile_revision &&
        bookmark_result_filter_ == bookmark_filter_ && bookmark_result_query_ == query)
        return bookmark_matches_;
    bookmark_result_revision_ = app_.state()->profile_revision;
    bookmark_result_filter_ = bookmark_filter_;
    bookmark_result_query_ = query;
    auto &result = bookmark_matches_;
    result.clear();
    for (size_t i = 0; i < app_.profile.bookmarks.size(); ++i) {
        const auto &b = app_.profile.bookmarks[i];
        if (bookmark_filter_ >= 0 && b.folder != bookmark_filter_)
            continue;
        std::string text =
            b.title + " " + b.url + " " + app_.profile.bookmark_folder_label(b.folder);
        for (const auto &tag : b.tags)
            text += " " + tag;
        const auto haystack = fold_settings(text);
        bool matches = true;
        size_t begin = 0;
        while (begin < query.size()) {
            const auto end = query.find(' ', begin);
            const auto token = query.substr(begin, end == std::string::npos ? end : end - begin);
            if (!token.empty() && haystack.find(token) == std::string::npos) {
                matches = false;
                break;
            }
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        if (matches)
            result.push_back(i);
    }
    return result;
}
std::vector<int> BrowserUi::bookmark_folder_results() const {
    std::vector<int> result;
    const auto query = fold_settings(bookmark_search_.value);
    auto add = [&](int id, const std::string &name) {
        if (query.empty() || fold_settings(name).find(query) != std::string::npos)
            result.push_back(id);
    };
    if (bookmark_folder_action_ == 0)
        add(-1, app_.tr("All folders"));
    add(0, app_.tr("Unfiled"));
    for (const auto &folder : app_.profile.bookmark_folders)
        add(folder.id, folder.name);
    return result;
}
void BrowserUi::bookmark_folder_menu(bool move, bool edit) {
    bookmark_folder_action_ = edit ? 2 : move ? 1 : 0;
    bookmark_previous_query_ = bookmark_search_.value;
    bookmark_search_.set("");
    bookmark_mode_ = 5;
    bookmark_focus_ = 0;
    bookmark_scroll_ = bookmark_folder_selected_ = 0;
    keyboard_focus_ = -1;
    app_.changed();
}
void BrowserUi::choose_bookmark_folder(int id) {
    if (id != -999) {
        if (bookmark_folder_action_ == 0)
            bookmark_filter_ = id;
        else if (app_.profile.has_bookmark_folder(id)) {
            if (bookmark_folder_action_ == 2)
                bookmark_edit_folder_ = id;
            else {
                for (auto &b : app_.profile.bookmarks)
                    if (bookmark_selection_.contains(b.url))
                        b.folder = id;
                bookmark_selection_.clear();
                app_.changed(true);
            }
        }
    }
    bookmark_mode_ = bookmark_folder_action_ == 2 ? 1 : 0;
    bookmark_search_.set(bookmark_previous_query_);
    bookmark_focus_ = -1;
    bookmark_scroll_ = 0;
    app_.changed();
}
void BrowserUi::bookmark_snapshot() {
    bookmark_undo_.clear();
    bookmark_undo_positions_.clear();
    for (size_t i = 0; i < app_.profile.bookmarks.size(); ++i) {
        const auto &b = app_.profile.bookmarks[i];
        if (bookmark_selection_.contains(b.url)) {
            bookmark_undo_.push_back(b);
            bookmark_undo_positions_.push_back(i);
        }
    }
    bookmark_can_undo_ = !bookmark_undo_.empty();
}
void BrowserUi::save_bookmark_edit() {
    bool ok = false;
    if (bookmark_mode_ == 1)
        ok = app_.profile.edit_bookmark(bookmark_edit_url_, bookmark_title_.value,
                                        bookmark_url_.value, bookmark_edit_folder_,
                                        bookmark_tags_.value);
    else if (bookmark_mode_ == 2) {
        const int id = app_.profile.add_bookmark_folder(bookmark_folder_name_.value);
        ok = id > 0;
        if (ok)
            bookmark_filter_ = id;
    } else if (bookmark_mode_ == 3)
        ok = app_.profile.rename_bookmark_folder(bookmark_filter_, bookmark_folder_name_.value);
    else if (bookmark_mode_ == 4) {
        if (auto tags = bookmark_tags(bookmark_tags_.value)) {
            for (auto &b : app_.profile.bookmarks)
                if (bookmark_selection_.contains(b.url))
                    b.tags = *tags;
            ok = true;
        }
    }
    if (!ok) {
        bookmark_error_ = app_.tr(
            bookmark_mode_ == 2 || bookmark_mode_ == 3
                ? "Use a short unique folder name. Up to 128 folders."
            : bookmark_mode_ == 4 ? "Use up to 32 short tags separated by commas."
                                  : "Check the name, HTTP(S) address, folder and comma-separated "
                                    "tags. Names and addresses must be unique.");
        app_.changed();
        return;
    }
    bookmark_mode_ = 0;
    bookmark_focus_ = -1;
    bookmark_error_.clear();
    bookmark_selection_.clear();
    bookmark_scroll_ = 0;
    app_.changed(true);
}
std::array<Rect, 5> BrowserUi::bookmark_actions(Rect r) const {
    const int desired[] = {108, 100, 72, 88, 100};
    const double factor = std::min(1., double(r.width - 88) / 468);
    std::array<Rect, 5> result;
    int x = r.x + 28;
    for (int i = 0; i < 5; ++i) {
        result[i] = {x, r.y + r.height - 116, int(desired[i] * factor), 32};
        x += result[i].width + 8;
    }
    return result;
}
void BrowserUi::draw_bookmarks(SkCanvas &c, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &title, bool enabled = true, bool primary = false) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8,
             SkColorSetA(theme.accent, primary                                    ? 30
                                       : enabled && inside(b, hover_x_, hover_y_) ? 24
                                                                                  : 8));
        border(c, b, 8, SkColorSetA(theme.accent, enabled ? 140 : 45));
        centered_label(c, medium_, title, b, 12, enabled ? theme.text : theme.muted, 8, 8);
    };
    auto input = [&](const std::string &title, TextEditor &editor, int focus, int y) {
        label(c, regular_, app_.tr(title), r.x + 28, r.y + y, 11, theme.muted, r.width - 56);
        Rect b{r.x + 28, r.y + y + 10, r.width - 56, 36};
        focus_target(b);
        fill(c, b, 8, theme.background);
        border(c, b, 8, SkColorSetA(theme.accent, bookmark_focus_ == focus ? 255 : 110));
        draw_editor(c, editor, {b.x + 10, b.y + 2, b.width - 20, 32}, 13, bookmark_focus_ == focus);
    };
    if (bookmark_mode_ == 5) {
        input("Search folders", bookmark_search_, 0, 82);
        const auto folders = bookmark_folder_results();
        const int capacity = std::max(1, (r.height - 236) / 44);
        bookmark_scroll_ =
            std::clamp(bookmark_scroll_, 0, std::max(0, int(folders.size()) - capacity));
        for (int i = bookmark_scroll_; i < int(folders.size()) && i < bookmark_scroll_ + capacity;
             ++i) {
            const int id = folders[i];
            Rect row{r.x + 28, r.y + 160 + (i - bookmark_scroll_) * 44, r.width - 56, 38};
            button(row, id < 0 ? app_.tr("All folders") : app_.profile.bookmark_folder_label(id));
            if (bookmark_focus_ == 0 && i == bookmark_folder_selected_)
                border(c, row, 8, theme.accent);
        }
        if (folders.empty())
            label(c, regular_, app_.tr("No matching folders."), r.x + 28, r.y + 190, 13,
                  theme.muted, r.width - 56);
        button({r.x + 28, r.y + r.height - 64, 132, 34}, app_.tr("Back"));
        return;
    }
    if (bookmark_mode_) {
        if (bookmark_mode_ == 1) {
            input("Title", bookmark_title_, 1, 82);
            input("Web address", bookmark_url_, 2, 148);
            input("Tags separated by commas", bookmark_tags_, 3, 214);
            label(c, regular_, app_.tr("Folder"), r.x + 28, r.y + 280, 11, theme.muted);
            button({r.x + 28, r.y + 290, r.width - 56, 34},
                   app_.profile.bookmark_folder_label(bookmark_edit_folder_));
        } else if (bookmark_mode_ == 4) {
            input("Tags separated by commas", bookmark_tags_, 3, 100);
            label(c, regular_, app_.tr("Replaces tags on the selected bookmarks."), r.x + 28,
                  r.y + 180, 12, theme.muted, r.width - 56);
        } else
            input("Folder name", bookmark_folder_name_, 4, 100);
        const auto lines = wrap_text(font(regular_, 11), bookmark_error_, r.width - 56, 3);
        for (size_t i = 0; i < lines.size(); ++i)
            label(c, regular_, lines[i], r.x + 28, r.y + r.height - 112 + int(i) * 15, 11,
                  theme.muted, r.width - 56);
        button({r.x + 28, r.y + r.height - 64, 132, 34}, app_.tr("Cancel"));
        button({r.x + r.width - 200, r.y + r.height - 64, 172, 34}, app_.tr("Save changes"), true,
               true);
        return;
    }
    input("Search bookmarks", bookmark_search_, 0, 78);
    const auto filter = bookmark_filter_ < 0 ? app_.tr("All folders")
                                             : app_.profile.bookmark_folder_label(bookmark_filter_);
    button({r.x + 28, r.y + 136, r.width - 296, 32}, filter);
    button({r.x + r.width - 260, r.y + 136, 136, 32}, app_.tr("Rename"), bookmark_filter_ > 0);
    button({r.x + r.width - 116, r.y + 136, 88, 32}, app_.tr("Remove"), bookmark_filter_ > 0);
    const auto &matches = bookmark_results();
    const int capacity = std::max(1, (r.height - 320) / 52);
    bookmark_scroll_ = std::clamp(bookmark_scroll_, 0, std::max(0, int(matches.size()) - capacity));
    if (matches.empty())
        label(c, regular_, app_.tr("No bookmarks here. Save a page or change the search."),
              r.x + 28, r.y + 210, 13, theme.muted, r.width - 56);
    for (int i = bookmark_scroll_; i < int(matches.size()) && i < bookmark_scroll_ + capacity;
         ++i) {
        const auto &b = app_.profile.bookmarks[matches[i]];
        const int y = r.y + 180 + (i - bookmark_scroll_) * 52;
        const bool selected = bookmark_selection_.contains(b.url);
        Rect check{r.x + 28, y + 9, 30, 30};
        focus_target(check);
        border(c, {check.x + 6, check.y + 6, 18, 18}, 4, SkColorSetA(theme.accent, 180));
        if (selected)
            icon(c, "check", check.x + 6, check.y + 6, 18, theme.accent);
        Rect open{r.x + 66, y, r.width - 202, 50};
        focus_target(open);
        if (inside(open, hover_x_, hover_y_))
            fill(c, open, 6, SkColorSetA(theme.accent, 12));
        label(c, medium_, b.title, r.x + 68, y + 17, 13, theme.text, r.width - 214);
        label(c, regular_, b.url, r.x + 68, y + 32, 10, theme.muted, r.width - 214);
        std::string tags = app_.profile.bookmark_folder_label(b.folder);
        for (const auto &tag : b.tags)
            tags += " / " + tag;
        label(c, regular_, tags, r.x + 68, y + 46, 10, theme.muted, r.width - 214);
        button({r.x + r.width - 124, y + 10, 96, 30}, app_.tr("Edit"));
    }
    if (int(matches.size()) > capacity) {
        const int h = capacity * 52, thumb = std::max(24, h * capacity / int(matches.size()));
        fill(c, {r.x + r.width - 13, r.y + 180, 3, h}, 1, SkColorSetA(theme.muted, 35));
        fill(c,
             {r.x + r.width - 13,
              r.y + 180 + (h - thumb) * bookmark_scroll_ / (int(matches.size()) - capacity), 3,
              thumb},
             1, theme.accent);
    }
    label(c, regular_,
          !bookmark_error_.empty() ? bookmark_error_
          : !save_error_.empty()
              ? save_error_
              : app_.tr("Selected") + ": " + std::to_string(bookmark_selection_.size()),
          r.x + 28, r.y + r.height - 140, 11, theme.muted, r.width - 56);
    const bool selection = !bookmark_selection_.empty();
    const auto actions = bookmark_actions(r);
    const char *titles[] = {"Select all", "Move", "Set tags", "Delete", "Undo"};
    const bool enabled[] = {!matches.empty(), selection, selection, selection, bookmark_can_undo_};
    for (int i = 0; i < 5; ++i)
        button(actions[i], app_.tr(titles[i]), enabled[i]);
    button({r.x + 28, r.y + r.height - 64, 140, 34}, app_.tr("New folder"));
    button({r.x + 184, r.y + r.height - 64, 180, 34}, app_.tr("Export all as HTML"),
           !app_.profile.bookmarks.empty());
    button({r.x + r.width - 128, r.y + r.height - 64, 100, 34}, app_.tr("Close"));
}
void BrowserUi::bookmarks_pointer(Rect r, int x, int y) {
    auto hit = [&](Rect b) { return inside(b, x, y); };
    auto field = [&](int top, int focus, TextEditor &editor) {
        if (hit({r.x + 28, r.y + top + 10, r.width - 56, 36})) {
            bookmark_focus_ = focus;
            editor.select_all();
            return true;
        }
        return false;
    };
    if (bookmark_mode_ == 5) {
        if (field(82, 0, bookmark_search_))
            return;
        if (hit({r.x + 28, r.y + r.height - 64, 132, 34})) {
            choose_bookmark_folder(-999);
            return;
        }
        const auto folders = bookmark_folder_results();
        const int cap = std::max(1, (r.height - 236) / 44),
                  n = bookmark_scroll_ + (y - r.y - 160) / 44;
        if (y >= r.y + 160 && y < r.y + 160 + cap * 44 && n >= 0 && n < int(folders.size()))
            choose_bookmark_folder(folders[n]);
        return;
    }
    if (bookmark_mode_) {
        if (bookmark_mode_ == 1) {
            if (field(82, 1, bookmark_title_) || field(148, 2, bookmark_url_) ||
                field(214, 3, bookmark_tags_))
                return;
            if (hit({r.x + 28, r.y + 290, r.width - 56, 34})) {
                bookmark_folder_menu(false, true);
                return;
            }
        } else if (bookmark_mode_ == 4) {
            if (field(100, 3, bookmark_tags_))
                return;
        } else if (field(100, 4, bookmark_folder_name_))
            return;
        if (hit({r.x + 28, r.y + r.height - 64, 132, 34})) {
            bookmark_mode_ = 0;
            bookmark_focus_ = -1;
            bookmark_error_.clear();
            app_.changed();
        } else if (hit({r.x + r.width - 200, r.y + r.height - 64, 172, 34}))
            save_bookmark_edit();
        return;
    }
    if (field(78, 0, bookmark_search_))
        return;
    if (hit({r.x + 28, r.y + 136, r.width - 296, 32})) {
        bookmark_folder_menu();
        return;
    }
    if (bookmark_filter_ > 0 && hit({r.x + r.width - 260, r.y + 136, 136, 32})) {
        bookmark_mode_ = 3;
        bookmark_folder_name_.set(app_.profile.bookmark_folder_label(bookmark_filter_), true);
        bookmark_focus_ = 4;
        app_.changed();
        return;
    }
    if (bookmark_filter_ > 0 && hit({r.x + r.width - 116, r.y + 136, 88, 32})) {
        app_.profile.remove_bookmark_folder(bookmark_filter_);
        bookmark_filter_ = 0;
        toast(app_.tr("Folder removed. Bookmarks moved to Unfiled."));
        app_.changed(true);
        return;
    }
    const auto &matches = bookmark_results();
    const int cap = std::max(1, (r.height - 320) / 52), n = bookmark_scroll_ + (y - r.y - 180) / 52;
    if (y >= r.y + 180 && y < r.y + 180 + cap * 52 && n >= 0 && n < int(matches.size())) {
        const auto &b = app_.profile.bookmarks[matches[n]];
        if (x < r.x + 64) {
            if (!bookmark_selection_.erase(b.url))
                bookmark_selection_.insert(b.url);
            bookmark_focus_ = -1;
            app_.changed();
        } else if (x >= r.x + r.width - 132) {
            bookmark_mode_ = 1;
            bookmark_edit_url_ = b.url;
            bookmark_edit_folder_ = b.folder;
            bookmark_title_.set(b.title, true);
            bookmark_url_.set(b.url);
            std::string tags;
            for (const auto &t : b.tags) {
                if (!tags.empty())
                    tags += ", ";
                tags += t;
            }
            bookmark_tags_.set(tags);
            bookmark_focus_ = 1;
            bookmark_error_.clear();
            app_.changed();
        } else
            app_.navigate(b.url);
        return;
    }
    const auto actions = bookmark_actions(r);
    for (int action = 0; action < 5; ++action) {
        if (hit(actions[action])) {
            if (action == 0) {
                for (auto i : matches)
                    bookmark_selection_.insert(app_.profile.bookmarks[i].url);
                app_.changed();
            } else if (action == 1 && !bookmark_selection_.empty())
                bookmark_folder_menu(true);
            else if (action == 2 && !bookmark_selection_.empty()) {
                bookmark_mode_ = 4;
                bookmark_tags_.set("");
                bookmark_focus_ = 3;
                app_.changed();
            } else if (action == 3 && !bookmark_selection_.empty()) {
                bookmark_snapshot();
                std::erase_if(app_.profile.bookmarks,
                              [&](const auto &b) { return bookmark_selection_.contains(b.url); });
                bookmark_selection_.clear();
                app_.changed(true);
            } else if (action == 4 && bookmark_can_undo_) {
                for (size_t i = 0; i < bookmark_undo_.size(); ++i)
                    if (!app_.profile.bookmarked(bookmark_undo_[i].url)) {
                        auto b = bookmark_undo_[i];
                        if (!app_.profile.has_bookmark_folder(b.folder))
                            b.folder = 0;
                        app_.profile.bookmarks.insert(app_.profile.bookmarks.begin() +
                                                          std::min(bookmark_undo_positions_[i],
                                                                   app_.profile.bookmarks.size()),
                                                      std::move(b));
                    }
                bookmark_undo_.clear();
                bookmark_undo_positions_.clear();
                bookmark_can_undo_ = false;
                app_.changed(true);
            }
            return;
        }
    }
    if (hit({r.x + 28, r.y + r.height - 64, 140, 34})) {
        bookmark_mode_ = 2;
        bookmark_folder_name_.set("");
        bookmark_focus_ = 4;
        bookmark_error_.clear();
        app_.changed();
    } else if (hit({r.x + 184, r.y + r.height - 64, 180, 34}) && !app_.profile.bookmarks.empty()) {
        size_t skipped = 0;
        auto payload =
            bookmarks_html(app_.profile.bookmarks, app_.profile.bookmark_folders, &skipped);
        auto save = [this, payload = std::move(payload)] {
            save_export(payload, "aura-bookmarks.html", "HTML | *.html");
        };
        if (skipped)
            app_.confirm_action(app_.tr("Export bookmarks"),
                                app_.tr("Unsupported bookmark addresses will be skipped: ") +
                                    std::to_string(skipped),
                                std::move(save));
        else
            save();
    } else if (hit({r.x + r.width - 128, r.y + r.height - 64, 100, 34}))
        dismiss();
}
std::string BrowserUi::bookmarks_report() const {
    if (panel_ != Bookmarks)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string draft = "{\"title\":" + json_quote(bookmark_title_.value) +
                        ",\"url\":" + json_quote(bookmark_url_.value) +
                        ",\"tags\":" + json_quote(bookmark_tags_.value) +
                        ",\"name\":" + json_quote(bookmark_folder_name_.value) + "}";
    std::string out =
        "{\"folder_selected\":" + std::to_string(bookmark_folder_selected_) +
        ",\"draft\":" + draft + ",\"rename\":" + rect({r.x + r.width - 260, r.y + 136, 136, 32}) +
        ",\"remove_folder\":" + rect({r.x + r.width - 116, r.y + 136, 88, 32}) +
        ",\"mode\":" + std::to_string(bookmark_mode_) +
        ",\"query\":" + json_quote(bookmark_search_.value) +
        ",\"filter\":" + std::to_string(bookmark_filter_) +
        ",\"selected\":" + std::to_string(bookmark_selection_.size()) +
        ",\"undo\":" + (bookmark_can_undo_ ? "true" : "false") +
        ",\"error\":" + json_quote(bookmark_error_.empty() ? save_error_ : bookmark_error_) +
        ",\"last_export\":" + json_quote(last_export_) +
        ",\"picker_pid\":" + std::to_string(picker_pid_) +
        ",\"search\":" + rect({r.x + 28, r.y + (bookmark_mode_ == 5 ? 92 : 88), r.width - 56, 36}) +
        ",\"folder\":" + rect({r.x + 28, r.y + 136, r.width - 296, 32}) +
        ",\"new_folder\":" + rect({r.x + 28, r.y + r.height - 64, 140, 34}) +
        ",\"export\":" + rect({r.x + 184, r.y + r.height - 64, 180, 34}) +
        ",\"save\":" + rect({r.x + r.width - 200, r.y + r.height - 64, 172, 34}) +
        ",\"title_input\":" + rect({r.x + 28, r.y + 92, r.width - 56, 36}) +
        ",\"url_input\":" + rect({r.x + 28, r.y + 158, r.width - 56, 36}) + ",\"tags_input\":" +
        rect({r.x + 28, r.y + (bookmark_mode_ == 1 ? 224 : 110), r.width - 56, 36}) +
        ",\"name_input\":" + rect({r.x + 28, r.y + 110, r.width - 56, 36}) +
        ",\"folder_input\":" + rect({r.x + 28, r.y + 290, r.width - 56, 34}) + ",\"actions\":[";
    const auto actions = bookmark_actions(r);
    const char *ids[] = {"select_all", "move", "tags", "delete", "undo"};
    for (int i = 0; i < 5; ++i) {
        if (i)
            out += ",";
        out += "{\"id\":" + json_quote(ids[i]) + ",\"bounds\":" + rect(actions[i]) + "}";
    }
    out += "],\"items\":[";
    if (bookmark_mode_ == 0) {
        const auto &matches = bookmark_results();
        const int cap = std::max(1, (r.height - 320) / 52);
        for (int i = bookmark_scroll_; i < int(matches.size()) && i < bookmark_scroll_ + cap; ++i) {
            if (i != bookmark_scroll_)
                out += ",";
            const auto &b = app_.profile.bookmarks[matches[i]];
            const int y = r.y + 180 + (i - bookmark_scroll_) * 52;
            out += "{\"url\":" + json_quote(b.url) + ",\"title\":" + json_quote(b.title) +
                   ",\"folder\":" + std::to_string(b.folder) +
                   ",\"select\":" + rect({r.x + 28, y + 9, 30, 30}) +
                   ",\"edit\":" + rect({r.x + r.width - 124, y + 10, 96, 30}) + "}";
        }
    }
    out += "],\"folders\":[";
    if (bookmark_mode_ == 5) {
        const auto folders = bookmark_folder_results();
        const int cap = std::max(1, (r.height - 236) / 44);
        for (int i = bookmark_scroll_; i < int(folders.size()) && i < bookmark_scroll_ + cap; ++i) {
            if (i != bookmark_scroll_)
                out += ",";
            const int id = folders[i];
            out += "{\"id\":" + std::to_string(id) + ",\"name\":" +
                   json_quote(id < 0 ? app_.tr("All folders")
                                     : app_.profile.bookmark_folder_label(id)) +
                   ",\"bounds\":" +
                   rect({r.x + 28, r.y + 160 + (i - bookmark_scroll_) * 44, r.width - 56, 38}) +
                   "}";
        }
    }
    return out + "]}";
}
void BrowserUi::open_sessions() {
    app_.close_menu();
    panel(Sessions);
    session_edit_ = session_scroll_ = 0;
    session_name_.set("");
    session_error_.clear();
    session_input_focus_ = true;
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
std::array<Rect, 4> BrowserUi::session_buttons(Rect r, int top) const {
    const int widths[] = {120, 136, 112, 96};
    const double factor = std::min(1., double(r.width - 80) / 464);
    std::array<Rect, 4> result;
    int x = r.x + 28;
    for (int i = 0; i < 4; ++i) {
        result[i] = {x, top, int(widths[i] * factor), 30};
        x += result[i].width + 8;
    }
    return result;
}
void BrowserUi::save_named_session() {
    bool ok = false;
    if (session_edit_)
        ok = app_.profile.rename_named_session(session_edit_, session_name_.value);
    else
        ok = app_.profile.save_named_session(session_name_.value, app_.capture_session()) > 0;
    if (!ok) {
        session_error_ = app_.tr("Use a short unique name and normal windows. Limits: 32 "
                                 "snapshots, 200 tabs per snapshot.");
        app_.changed();
        return;
    }
    session_name_.set("");
    session_edit_ = 0;
    session_error_.clear();
    session_input_focus_ = true;
    ++app_.audit["named_sessions_saved"];
    app_.changed(true);
}
void BrowserUi::draw_sessions(SkCanvas &c, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &text, bool enabled = true, bool primary = false) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8,
             SkColorSetA(theme.accent, primary                                    ? 30
                                       : enabled && inside(b, hover_x_, hover_y_) ? 24
                                                                                  : 8));
        border(c, b, 8, SkColorSetA(theme.accent, enabled ? 140 : 45));
        centered_label(c, medium_, app_.tr(text), b, 12, enabled ? theme.text : theme.muted, 8, 8);
    };
    label(c, regular_,
          app_.tr("Save URLs and layout of normal windows. Private tabs are excluded."), r.x + 28,
          r.y + 73, 11, theme.muted, r.width - 56);
    label(c, regular_, app_.tr("Snapshot name"), r.x + 28, r.y + 101, 11, theme.muted);
    Rect input{r.x + 28, r.y + 112, r.width - 240, 36};
    focus_target(input);
    fill(c, input, 8, theme.background);
    border(c, input, 8, SkColorSetA(theme.accent, session_input_focus_ ? 255 : 110));
    draw_editor(c, session_name_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                session_input_focus_);
    button({r.x + r.width - 200, r.y + 112, 172, 36},
           session_edit_ ? "Save changes" : "Save snapshot", !session_name_.value.empty(), true);
    const auto &items = app_.profile.named_sessions;
    const int cap = std::max(1, (r.height - 268) / 90);
    session_scroll_ = std::clamp(session_scroll_, 0, std::max(0, int(items.size()) - cap));
    if (items.empty())
        label(c, regular_, app_.tr("No snapshots yet. Enter a name to save your normal windows."),
              r.x + 28, r.y + 200, 13, theme.muted, r.width - 56);
    for (int i = session_scroll_; i < int(items.size()) && i < session_scroll_ + cap; ++i) {
        const auto &item = items[i];
        const int top = r.y + 176 + (i - session_scroll_) * 90;
        size_t tabs = 0;
        for (const auto &w : item.windows)
            tabs += w.tabs.size();
        const auto time = local_time(std::time_t(item.updated));
        char date[40]{};
        std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &time);
        label(c, medium_, item.name, r.x + 28, top + 18, 13, theme.text, r.width - 230);
        label(c, regular_, date, r.x + r.width - 184, top + 18, 10, theme.muted, 156);
        const bool available = app_.profile.named_session_available(item.id);
        const auto detail = app_.tr("Windows") + ": " + std::to_string(item.windows.size()) +
                            " / " + app_.tr("Tabs") + ": " + std::to_string(tabs) +
                            (available ? "" : " / " + app_.tr("Missing container"));
        label(c, regular_, detail, r.x + 28, top + 36, 10, theme.muted, r.width - 56);
        const auto buttons = session_buttons(r, top + 44);
        button(buttons[0], "Restore", available);
        button(buttons[1], "Rename");
        button(buttons[2], "Update snapshot");
        button(buttons[3], "Delete");
    }
    if (int(items.size()) > cap) {
        const int h = cap * 90, thumb = std::max(24, h * cap / int(items.size()));
        fill(c, {r.x + r.width - 13, r.y + 176, 3, h}, 1, SkColorSetA(theme.muted, 35));
        fill(c,
             {r.x + r.width - 13,
              r.y + 176 + (h - thumb) * session_scroll_ / (int(items.size()) - cap), 3, thumb},
             1, theme.accent);
    }
    const auto lines = wrap_text(font(regular_, 11), session_error_, r.width - 56, 2);
    for (size_t i = 0; i < lines.size(); ++i)
        label(c, regular_, lines[i], r.x + 28, r.y + r.height - 98 + int(i) * 15, 11, theme.muted,
              r.width - 56);
    button({r.x + 28, r.y + r.height - 56, 172, 32}, "Undo deletion", session_undo_.has_value());
    button({r.x + r.width - 160, r.y + r.height - 56, 132, 32}, "Close");
}
void BrowserUi::sessions_pointer(Rect r, int x, int y) {
    if (inside({r.x + 28, r.y + 112, r.width - 240, 36}, x, y)) {
        session_input_focus_ = true;
        session_name_.select_all();
        return;
    }
    if (inside({r.x + r.width - 200, r.y + 112, 172, 36}, x, y)) {
        if (!session_name_.value.empty())
            save_named_session();
        return;
    }
    if (inside({r.x + r.width - 160, r.y + r.height - 56, 132, 32}, x, y)) {
        dismiss();
        return;
    }
    if (inside({r.x + 28, r.y + r.height - 56, 172, 32}, x, y) && session_undo_) {
        const auto &old = *session_undo_;
        const bool duplicate = std::any_of(
            app_.profile.named_sessions.begin(), app_.profile.named_sessions.end(),
            [&](const auto &item) {
                return item.id == old.id || fold_settings(item.name) == fold_settings(old.name);
            });
        if (!duplicate && app_.profile.named_sessions.size() < 32) {
            app_.profile.named_sessions.push_back(old);
            app_.profile.next_named_session = std::max(app_.profile.next_named_session, old.id + 1);
            session_undo_.reset();
            session_error_.clear();
            app_.changed(true);
        } else {
            session_error_ =
                app_.tr("A snapshot with this name already exists, or the list is full.");
            app_.changed();
        }
        return;
    }
    const int cap = std::max(1, (r.height - 268) / 90),
              index = session_scroll_ + (y - r.y - 176) / 90;
    if (y < r.y + 176 || y >= r.y + 176 + cap * 90 || index < 0 ||
        index >= int(app_.profile.named_sessions.size()))
        return;
    const auto item = app_.profile.named_sessions[index];
    const auto buttons = session_buttons(r, r.y + 176 + (index - session_scroll_) * 90 + 44);
    if (inside(buttons[0], x, y)) {
        if (app_.profile.named_session_available(item.id))
            app_.restore_named_session(item.id);
    } else if (inside(buttons[1], x, y)) {
        session_edit_ = item.id;
        session_name_.set(item.name, true);
        session_input_focus_ = true;
        session_error_.clear();
        app_.changed();
    } else if (inside(buttons[2], x, y)) {
        BrowserDialog dialog;
        dialog.owned = true;
        dialog.origin = app_.tr("Replace this snapshot?");
        dialog.accept_label = "Update snapshot";
        dialog.message = item.name + "\n" +
                         app_.tr("Replaces its saved URLs and layout with your current normal "
                                 "windows. Private tabs are excluded.");
        const auto weak = std::weak_ptr<void>(app_.callback_lifetime);
        dialog.action = [this, weak, id = item.id, revision = item.revision](bool accept,
                                                                             const std::string &) {
            if (!accept || weak.expired())
                return;
            const auto current = app_.profile.named_session(id);
            if (!current || current->revision != revision) {
                session_error_ = app_.tr("The snapshot changed. Open it again.");
                app_.changed();
                return;
            }
            session_error_ =
                app_.profile.save_named_session(current->name, app_.capture_session(), id) > 0
                    ? ""
                    : app_.tr("Could not update this snapshot. Check the tab limit.");
            app_.changed(true);
        };
        app_.dialogs.push_back(std::move(dialog));
        app_.changed();
    } else if (inside(buttons[3], x, y)) {
        session_undo_ = item;
        app_.profile.remove_named_session(item.id);
        session_error_.clear();
        if (session_edit_ == item.id) {
            session_edit_ = 0;
            session_name_.set("");
        }
        app_.changed(true);
    }
}
std::string BrowserUi::sessions_report() const {
    if (panel_ != Sessions)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"name\":" + json_quote(session_name_.value) +
                      ",\"editing\":" + std::to_string(session_edit_) +
                      ",\"error\":" + json_quote(session_error_) +
                      ",\"input\":" + rect({r.x + 28, r.y + 112, r.width - 240, 36}) +
                      ",\"save\":" + rect({r.x + r.width - 200, r.y + 112, 172, 36}) +
                      ",\"undo\":" + rect({r.x + 28, r.y + r.height - 56, 172, 32}) +
                      ",\"can_undo\":" + (session_undo_ ? "true" : "false") + ",\"items\":[";
    const int cap = std::max(1, (r.height - 268) / 90);
    const auto &items = app_.profile.named_sessions;
    for (int i = session_scroll_; i < int(items.size()) && i < session_scroll_ + cap; ++i) {
        if (i != session_scroll_)
            out += ",";
        const auto &item = items[i];
        size_t count = 0;
        for (const auto &w : item.windows)
            count += w.tabs.size();
        const auto buttons = session_buttons(r, r.y + 176 + (i - session_scroll_) * 90 + 44);
        out += "{\"id\":" + std::to_string(item.id) + ",\"name\":" + json_quote(item.name) +
               ",\"windows\":" + std::to_string(item.windows.size()) +
               ",\"tabs\":" + std::to_string(count) + ",\"available\":" +
               (app_.profile.named_session_available(item.id) ? "true" : "false") +
               ",\"restore\":" + rect(buttons[0]) + ",\"rename\":" + rect(buttons[1]) +
               ",\"update\":" + rect(buttons[2]) + ",\"delete\":" + rect(buttons[3]) + "}";
    }
    return out + "]}";
}
void BrowserUi::open_screenshot() {
    if (app_.safe_mode()) {
        toast(app_.tr("This tool is paused in safe mode."));
        return;
    }
    app_.cancel_screenshot();
    app_.close_menu();
    panel(Screenshot);
    keyboard_focus_ = -1;
    auto &shot = app_.screenshot;
    const auto t = app_.current();
    shot.tab = t ? t->id : 0;
    shot.navigation = t ? t->navigation_serial : 0;
    shot.source = t ? t->page.url : "";
    shot.mode = 0;
    shot.width = shot.height = 0;
    shot.reduced = false;
    shot.error.clear();
    if (!export_dialog_open()) {
        last_export_.clear();
        save_error_.clear();
    }
    if (t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
void BrowserUi::draw_screenshot(SkCanvas &c, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    const auto &shot = app_.screenshot;
    const auto t = app_.tab(shot.tab);
    const bool busy = shot.pending || shot.native_running || export_dialog_open();
    const bool ready = t && t->browser && !t->ntp && !t->loading && t->page.loaded &&
                       !t->page.failed && app_.tab_visible(t->id) &&
                       t->navigation_serial == shot.navigation && t->page.url == shot.source &&
                       (shot.source.starts_with("http://") || shot.source.starts_with("https://") ||
                        shot.source.starts_with("file://"));
    auto button = [&](Rect b, const std::string &title, bool enabled = true, bool primary = false) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8,
             SkColorSetA(theme.accent, primary                                    ? 30
                                       : enabled && inside(b, hover_x_, hover_y_) ? 24
                                                                                  : 8));
        border(c, b, 8, SkColorSetA(theme.accent, enabled ? 140 : 45));
        centered_label(c, medium_, app_.tr(title), b, 12, enabled ? theme.text : theme.muted, 8, 8);
    };
    label(c, regular_, app_.tr("Capture this page without the Aura interface."), r.x + 28, r.y + 73,
          11, theme.muted, r.width - 56);
    label(c, medium_, shot.source, r.x + 28, r.y + 101, 13, theme.text, r.width - 56);
    label(c, regular_,
          t ? (app_.profile.container_label(t->container) +
               (t->private_tab ? " / " + app_.tr("Private browsing") : ""))
            : "",
          r.x + 28, r.y + 122, 10, theme.muted, r.width - 56);
    const char *titles[] = {"Visible area", "Select area", "Full page"};
    const char *details[] = {"The visible document, without scrollbars.",
                             "Choose a region using the mouse or keyboard.",
                             "The whole document. Large pages are reduced to fit one PNG."};
    for (int i = 0; i < 3; ++i) {
        Rect row{r.x + 28, r.y + 140 + i * 72, r.width - 56, 60};
        const bool enabled = !busy && !(i == 2 && t && t->reader_active);
        if (enabled)
            focus_target(row);
        fill(c, row, 8,
             SkColorSetA(theme.accent, shot.mode == i                               ? 20
                                       : enabled && inside(row, hover_x_, hover_y_) ? 12
                                                                                    : 0));
        border(c, row, 8, SkColorSetA(theme.accent, shot.mode == i ? 180 : 55));
        if (shot.mode == i)
            icon(c, "check", row.x + 12, row.y + 19, 20, theme.accent);
        label(c, medium_, app_.tr(titles[i]), row.x + 44, row.y + 23, 13,
              enabled ? theme.text : theme.muted, row.width - 60);
        label(c, regular_,
              app_.tr(!enabled && i == 2 && t && t->reader_active
                          ? "Exit reader view to capture the full page."
                          : details[i]),
              row.x + 44, row.y + 44, 10, theme.muted, row.width - 60);
    }
    std::string status = shot.pending           ? app_.tr("Capturing...")
                         : shot.native_running  ? app_.tr("Finishing the previous capture...")
                         : export_dialog_open() ? app_.tr("Choose a location in the file dialog.")
                         : shot.width
                             ? std::to_string(shot.width) + " × " + std::to_string(shot.height) +
                                   " px" + (shot.reduced ? " / " + app_.tr("Reduced to fit") : "")
                         : !ready ? app_.tr("Open a loaded web page first.")
                                  : app_.tr("PNG is saved only after you choose a location.");
    label(c, regular_, status, r.x + 28, r.y + r.height - 128, 11, theme.muted, r.width - 56);
    const auto lines = wrap_text(font(regular_, 11), shot.error.empty() ? save_error_ : shot.error,
                                 r.width - 56, 2);
    for (size_t i = 0; i < lines.size(); ++i)
        label(c, regular_, lines[i], r.x + 28, r.y + r.height - 106 + int(i) * 15, 11, theme.muted,
              r.width - 56);
    button({r.x + 28, r.y + r.height - 56, 120, 34}, "Close");
    button({r.x + 160, r.y + r.height - 56, 140, 34}, "Use current page", !shot.pending);
    button({r.x + r.width - 184, r.y + r.height - 56, 156, 34},
           shot.mode == 1 ? "Select area" : "Capture",
           ready && !busy && !(shot.mode == 2 && t && t->reader_active), true);
}
void BrowserUi::screenshot_pointer(Rect r, int x, int y) {
    auto &shot = app_.screenshot;
    auto t = app_.tab(shot.tab);
    const bool busy = shot.pending || shot.native_running || export_dialog_open();
    if (inside({r.x + 28, r.y + r.height - 56, 120, 34}, x, y)) {
        dismiss();
        return;
    }
    if (inside({r.x + 160, r.y + r.height - 56, 140, 34}, x, y) && !shot.pending) {
        open_screenshot();
        return;
    }
    if (busy)
        return;
    for (int i = 0; i < 3; ++i)
        if (inside({r.x + 28, r.y + 140 + i * 72, r.width - 56, 60}, x, y) &&
            !(i == 2 && t && t->reader_active)) {
            shot.mode = i;
            shot.error.clear();
            app_.changed();
            return;
        }
    if (inside({r.x + r.width - 184, r.y + r.height - 56, 156, 34}, x, y))
        app_.capture_screenshot();
}
void BrowserUi::begin_screenshot_area() {
    const auto t = app_.tab(app_.screenshot.tab);
    if (!t || !t->browser)
        return;
    const auto page = page_bounds(*t);
    auto &shot = app_.screenshot;
    screenshot_area_ = page;
    screenshot_area_.width = std::clamp(
        int(std::lround(page.width * shot.client_width * shot.area_zoom / t->page.view_width)), 1,
        page.width);
    screenshot_area_.height = std::clamp(
        int(std::lround(page.height * shot.client_height * shot.area_zoom / t->page.view_height)),
        1, page.height);
    if (screenshot_area_.width < 16 || screenshot_area_.height < 100) {
        shot.error = app_.tr("The visible page is too small to select an area.");
        app_.changed();
        return;
    }
    screenshot_region_ = {screenshot_area_.x + screenshot_area_.width / 4,
                          screenshot_area_.y + screenshot_area_.height / 4,
                          screenshot_area_.width / 2, screenshot_area_.height / 2};
    screenshot_drag_ = screenshot_cancel_focus_ = false;
    app_.window->cursor(CT_CROSS);
    shot.selecting = true;
    panel_ = None;
    editing = false;
    keyboard_focus_ = -1;
    t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
void BrowserUi::finish_screenshot_area(bool accept) {
    auto &shot = app_.screenshot;
    if (!shot.selecting)
        return;
    shot.selecting = false;
    app_.window->cursor(CT_POINTER);
    screenshot_drag_ = false;
    panel_ = Screenshot;
    keyboard_focus_ = -1;
    if (!accept) {
        app_.cancel_screenshot();
        app_.changed();
        return;
    }
    if (screenshot_region_.width < 2 || screenshot_region_.height < 2) {
        shot.error = app_.tr("Select a larger area.");
        app_.changed();
        return;
    }
    app_.capture_screenshot(
        {double(screenshot_region_.x - screenshot_area_.x) / screenshot_area_.width,
         double(screenshot_region_.y - screenshot_area_.y) / screenshot_area_.height,
         double(screenshot_region_.width) / screenshot_area_.width,
         double(screenshot_region_.height) / screenshot_area_.height});
}
void BrowserUi::draw_screenshot_area(SkCanvas &c) {
    const auto &theme = interface_theme(app_.profile);
    const auto a = screenshot_area_, r = screenshot_region_, toolbar_area = web_bounds();
    focus_targets_.clear();
    fill(c, {a.x, a.y, a.width, std::max(0, r.y - a.y)}, 0, 0x99070B16);
    fill(c, {a.x, r.y, std::max(0, r.x - a.x), r.height}, 0, 0x99070B16);
    fill(c, {r.x + r.width, r.y, std::max(0, a.x + a.width - r.x - r.width), r.height}, 0,
         0x99070B16);
    fill(c, {a.x, r.y + r.height, a.width, std::max(0, a.y + a.height - r.y - r.height)}, 0,
         0x99070B16);
    border(c, r, 0, theme.accent);
    const auto bar = toolbar_area;
    Rect toolbar{bar.x + 8, bar.y + 8, bar.width - 16, 72};
    fill(c, toolbar, 9, theme.surface);
    const auto lines = wrap_text(
        font(regular_, 11),
        app_.tr("Drag to select. Arrows move; Shift+arrows resize. Enter saves; Escape cancels."),
        std::max(80, bar.width - 260), 3);
    for (size_t i = 0; i < lines.size(); ++i)
        label(c, regular_, lines[i], bar.x + 20, bar.y + 29 + int(i) * 15, 11, theme.text,
              std::max(80, bar.width - 260));
    Rect save{bar.x + bar.width - 228, bar.y + 24, 108, 34},
        cancel{bar.x + bar.width - 112, bar.y + 24, 96, 34};
    for (const auto &[b, title, focus] : std::vector<std::tuple<Rect, std::string, bool>>{
             {save, "Save PNG", !screenshot_cancel_focus_},
             {cancel, "Cancel", screenshot_cancel_focus_}}) {
        fill(c, b, 8, SkColorSetA(theme.accent, focus ? 36 : 10));
        border(c, b, 8, focus ? theme.accent : SkColorSetA(theme.accent, 90));
        centered_label(c, medium_, app_.tr(title), b, 12, theme.text, 8, 8);
    }
    const auto &shot = app_.screenshot;
    auto t = app_.tab(shot.tab);
    auto plan = screenshot_plan({0, 0, double(r.width) * shot.client_width / a.width,
                                 double(r.height) * shot.client_height / a.height},
                                shot.area_zoom, t ? t->page.scale : 1);
    if (plan) {
        Rect badge{std::clamp(r.x, bar.x + 8, std::max(bar.x + 8, bar.x + bar.width - 168)),
                   std::clamp(r.y + r.height + 8, std::min(bar.y + 90, bar.y + bar.height - 30),
                              bar.y + bar.height - 30),
                   160, 24};
        fill(c, badge, 6, theme.surface);
        centered_label(c, medium_,
                       std::to_string(plan->width) + " × " + std::to_string(plan->height) + " px",
                       badge, 11, theme.text, 6, 6);
    }
}
void BrowserUi::screenshot_area_pointer(const PointerInput &input) {
    const auto a = screenshot_area_, bar = web_bounds();
    const int x = std::clamp(input.point.x, a.x, a.x + a.width),
              y = std::clamp(input.point.y, a.y, a.y + a.height);
    if (input.kind == PointerInput::Down && input.button == 1) {
        if (inside({bar.x + bar.width - 228, bar.y + 24, 108, 34}, input.point.x, input.point.y)) {
            finish_screenshot_area(true);
            return;
        }
        if (inside({bar.x + bar.width - 112, bar.y + 24, 96, 34}, input.point.x, input.point.y)) {
            finish_screenshot_area(false);
            return;
        }
        if (!inside(a, input.point.x, input.point.y))
            return;
        screenshot_start_x_ = x;
        screenshot_start_y_ = y;
        screenshot_drag_ = true;
        screenshot_cancel_focus_ = false;
        screenshot_region_ = {x, y, 0, 0};
    }
    if (screenshot_drag_ && (input.kind == PointerInput::Move || input.kind == PointerInput::Up)) {
        screenshot_region_ = {std::min(x, screenshot_start_x_), std::min(y, screenshot_start_y_),
                              std::abs(x - screenshot_start_x_), std::abs(y - screenshot_start_y_)};
        if (input.kind == PointerInput::Up) {
            screenshot_drag_ = false;
            screenshot_region_.x = std::clamp(screenshot_region_.x, a.x, a.x + a.width - 8);
            screenshot_region_.y = std::clamp(screenshot_region_.y, a.y, a.y + a.height - 8);
            screenshot_region_.width =
                std::clamp(screenshot_region_.width, 8, a.x + a.width - screenshot_region_.x);
            screenshot_region_.height =
                std::clamp(screenshot_region_.height, 8, a.y + a.height - screenshot_region_.y);
        }
    }
    app_.changed();
}
std::string BrowserUi::screenshot_report() const {
    const auto &shot = app_.screenshot;
    if (panel_ != Screenshot && !shot.selecting)
        return "null";
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    const auto r = panel_bounds();
    std::string out =
        "{\"source\":" + json_quote(shot.source) + ",\"mode\":" + std::to_string(shot.mode) +
        ",\"busy\":" + (shot.pending || shot.native_running ? "true" : "false") +
        ",\"selecting\":" + (shot.selecting ? "true" : "false") +
        ",\"width\":" + std::to_string(shot.width) + ",\"height\":" + std::to_string(shot.height) +
        ",\"reduced\":" + (shot.reduced ? "true" : "false") +
        ",\"error\":" + json_quote(shot.error.empty() ? save_error_ : shot.error) +
        ",\"last_export\":" + json_quote(last_export_) +
        ",\"picker_pid\":" + std::to_string(picker_pid_) +
        ",\"capture\":" + rect({r.x + r.width - 184, r.y + r.height - 56, 156, 34}) +
        ",\"refresh\":" + rect({r.x + 160, r.y + r.height - 56, 140, 34}) +
        ",\"area\":" + rect(screenshot_area_) + ",\"selection\":" + rect(screenshot_region_) +
        ",\"options\":[";
    for (int i = 0; i < 3; ++i) {
        if (i)
            out += ",";
        out += "{\"id\":" + std::to_string(i) +
               ",\"bounds\":" + rect({r.x + 28, r.y + 140 + i * 72, r.width - 56, 60}) + "}";
    }
    return out + "]}";
}
void BrowserUi::open_hotkeys() {
    app_.close_menu();
    panel(Hotkeys);
    hotkey_search_.set("");
    hotkey_search_focus_ = true;
    hotkey_recording_.clear();
    hotkey_error_.clear();
    hotkey_scroll_ = 0;
    keyboard_focus_ = -1;
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
std::string BrowserUi::shortcut_hint(const std::string &id) const {
    if (id == "back")
        return "Alt+Left";
    if (id == "forward")
        return "Alt+Right";
    auto h = effective_hotkey(app_.profile.hotkeys, id);
    return h.key ? hotkey_label(h) : "";
}
std::vector<int> BrowserUi::hotkey_results() const {
    std::vector<int> result;
    const auto query = fold_settings(hotkey_search_.value);
    auto specs = hotkey_catalog();
    for (int i = 0; i < int(specs.size()); ++i)
        if (query.empty() ||
            fold_settings(std::string(specs[i].id) + " " + specs[i].title + " " +
                          app_.tr(specs[i].title) + " " + shortcut_hint(specs[i].id))
                    .find(query) != std::string::npos)
            result.push_back(i);
    return result;
}
void BrowserUi::save_hotkey(const std::string &id, Hotkey chord) {
    const auto error = assign_hotkey(app_.profile.hotkeys, id, chord);
    if (error.empty()) {
        hotkey_recording_.clear();
        hotkey_error_.clear();
        app_.changed(true);
        return;
    }
    if (error == "reserved" || error == "unknown")
        hotkey_error_ = app_.tr("This combination is reserved. Choose another.");
    else {
        hotkey_error_ = app_.tr("Already used by:") + " ";
        for (const auto &s : hotkey_catalog())
            if (error == s.id)
                hotkey_error_ += app_.tr(s.title);
    }
    app_.changed();
}
void BrowserUi::draw_hotkeys(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &title, bool enabled = true) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8, SkColorSetA(t.accent, enabled && inside(b, hover_x_, hover_y_) ? 24 : 8));
        border(c, b, 8, SkColorSetA(t.accent, enabled ? 140 : 45));
        centered_label(c, medium_, title, b, 12, enabled ? t.text : t.muted, 8, 8);
    };
    label(c, regular_, app_.tr("Choose a command, then press a new combination."), r.x + 28,
          r.y + 73, 11, t.muted, r.width - 56);
    label(c, medium_, app_.tr("Search commands"), r.x + 28, r.y + 98, 11, t.muted, r.width - 56);
    Rect search{r.x + 28, r.y + 108, r.width - 56, 36};
    focus_target(search);
    fill(c, search, 8, t.background);
    border(c, search, 8, hotkey_search_focus_ ? t.accent : SkColorSetA(t.accent, 80));
    draw_editor(c, hotkey_search_, search, 13, hotkey_search_focus_);
    const std::string status = !hotkey_recording_.empty()
                                   ? app_.tr("Press a combination. Escape cancels.")
                                   : app_.tr("System and text editing shortcuts stay reserved.");
    label(c, regular_, status, r.x + 28, r.y + 166, 11, t.muted, r.width - 56);
    label(c, regular_, hotkey_error_, r.x + 28, r.y + 184, 11, t.text, r.width - 56);
    const auto rows = hotkey_results();
    const int cap = std::max(1, (r.height - 280) / 56);
    hotkey_scroll_ = std::clamp(hotkey_scroll_, 0, std::max(0, int(rows.size()) - cap));
    if (rows.empty())
        label(c, regular_, app_.tr("No matching commands. Change the search."), r.x + 28, r.y + 227,
              13, t.muted, r.width - 56);
    for (int i = hotkey_scroll_; i < int(rows.size()) && i < hotkey_scroll_ + cap; ++i) {
        const auto &s = hotkey_catalog()[rows[i]];
        const int y = r.y + 204 + (i - hotkey_scroll_) * 56;
        const bool recording = hotkey_recording_ == s.id;
        if (recording)
            fill(c, {r.x + 20, y - 4, r.width - 40, 48}, 6, SkColorSetA(t.accent, 16));
        label(c, medium_, app_.tr(s.title), r.x + 28, y + 22, 13, t.text, r.width - 438);
        const auto h = effective_hotkey(app_.profile.hotkeys, s.id);
        button({r.x + r.width - 398, y, 164, 34},
               recording ? app_.tr("Press keys...") : app_.tr(hotkey_label(h).c_str()));
        button({r.x + r.width - 226, y, 88, 34}, app_.tr("Reset"), h != s.standard);
        button({r.x + r.width - 130, y, 102, 34}, app_.tr("Disable"), h.key != 0);
    }
    button({r.x + 28, r.y + r.height - 56, 232, 34}, app_.tr("Restore default shortcuts"));
    button({r.x + r.width - 148, r.y + r.height - 56, 120, 34}, app_.tr("Close"));
}
void BrowserUi::hotkeys_pointer(Rect r, int x, int y) {
    if (inside({r.x + 28, r.y + 108, r.width - 56, 36}, x, y)) {
        hotkey_search_focus_ = true;
        hotkey_recording_.clear();
        hotkey_search_.select_all();
        app_.changed();
        return;
    }
    hotkey_search_focus_ = false;
    keyboard_focus_ = -1;
    if (inside({r.x + 28, r.y + r.height - 56, 232, 34}, x, y)) {
        app_.profile.hotkeys.clear();
        hotkey_recording_.clear();
        hotkey_error_.clear();
        app_.changed(true);
        return;
    }
    if (inside({r.x + r.width - 148, r.y + r.height - 56, 120, 34}, x, y)) {
        dismiss();
        return;
    }
    auto rows = hotkey_results();
    int index = (y - r.y - 204) / 56 + hotkey_scroll_;
    if (y < r.y + 204 || y >= r.y + r.height - 76 || index < 0 || index >= int(rows.size()))
        return;
    const auto &s = hotkey_catalog()[rows[index]];
    const int top = r.y + 204 + (index - hotkey_scroll_) * 56;
    if (inside({r.x + r.width - 398, top, 164, 34}, x, y)) {
        hotkey_recording_ = s.id;
        hotkey_error_.clear();
        app_.changed();
    } else if (inside({r.x + r.width - 226, top, 88, 34}, x, y))
        save_hotkey(s.id, s.standard);
    else if (inside({r.x + r.width - 130, top, 102, 34}, x, y))
        save_hotkey(s.id, {});
}
std::string BrowserUi::hotkeys_report() const {
    if (panel_ != Hotkeys)
        return "null";
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    const auto r = panel_bounds();
    const auto rows = hotkey_results();
    const int cap = std::max(1, (r.height - 280) / 56);
    std::string out = "{\"query\":" + json_quote(hotkey_search_.value) +
                      ",\"recording\":" + json_quote(hotkey_recording_) +
                      ",\"error\":" + json_quote(hotkey_error_) +
                      ",\"search\":" + rect({r.x + 28, r.y + 108, r.width - 56, 36}) +
                      ",\"reset_all\":" + rect({r.x + 28, r.y + r.height - 56, 232, 34}) +
                      ",\"items\":[";
    for (int i = hotkey_scroll_; i < int(rows.size()) && i < hotkey_scroll_ + cap; ++i) {
        if (i != hotkey_scroll_)
            out += ",";
        const auto &s = hotkey_catalog()[rows[i]];
        const int y = r.y + 204 + (i - hotkey_scroll_) * 56;
        const auto h = effective_hotkey(app_.profile.hotkeys, s.id);
        out += "{\"id\":" + json_quote(s.id) + ",\"key\":" + std::to_string(h.key) +
               ",\"modifiers\":" + std::to_string(h.modifiers) +
               ",\"bind\":" + rect({r.x + r.width - 398, y, 164, 34}) +
               ",\"reset\":" + rect({r.x + r.width - 226, y, 88, 34}) +
               ",\"disable\":" + rect({r.x + r.width - 130, y, 102, 34}) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::draw_container_rules(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &text, bool dropdown = false) {
        focus_target(b);
        fill(c, b, 8, SkColorSetA(t.accent, inside(b, hover_x_, hover_y_) ? 35 : 16));
        border(c, b, 8, SkColorSetA(t.accent, 120));
        centered_label(c, medium_, text, b, 12, t.text, 5, dropdown ? 28 : 5);
        if (dropdown)
            icon(c, "collapse", b.x + b.width - 22, b.y + 12, 12, t.text);
    };
    label(c, regular_, app_.tr("Confirm before opening a matching site in its assigned container."),
          r.x + 28, r.y + 73, 12, t.muted, r.width - 56);
    label(c, regular_, app_.tr("Hostname (example.org)"), r.x + 28, r.y + 99, 11, t.muted);
    Rect input{r.x + 28, r.y + 108, r.width - 196, 36};
    focus_target(input);
    fill(c, input, 8, t.background);
    border(c, input, 8, SkColorSetA(t.accent, container_input_focus_ ? 220 : 85));
    draw_editor(c, container_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                container_input_focus_);
    button({r.x + r.width - 156, r.y + 108, 128, 36}, app_.tr("Save rule"));
    const int half = (r.width - 64) / 2;
    button({r.x + 28, r.y + 154, half, 36},
           app_.tr("Container") + ": " + app_.profile.container_label(container_rule_target_), true);
    button({r.x + 36 + half, r.y + 154, half, 36},
           std::string(container_rule_subdomains_ ? "✓ " : "○ ") + app_.tr("Include subdomains"));
    label(c, regular_, container_error_, r.x + 28, r.y + 205, 11, t.text, r.width - 56);
    const int capacity = std::max(1, (r.height - 260) / 88);
    container_scroll_ = std::clamp(
        container_scroll_, 0, std::max(0, int(app_.profile.container_rules.size()) - capacity));
    if (app_.profile.container_rules.empty())
        label(c, regular_, app_.tr("Add a hostname above to create the first rule."), r.x + 28,
              r.y + 242, 13, t.muted, r.width - 56);
    for (int i = container_scroll_;
         i < int(app_.profile.container_rules.size()) && i < container_scroll_ + capacity; ++i) {
        const auto &rule = app_.profile.container_rules[i];
        const int y = r.y + 212 + (i - container_scroll_) * 88;
        label(c, medium_, rule.host, r.x + 28, y + 22, 13, t.text, r.width - 310);
        label(c, regular_,
              app_.profile.container_label(rule.container) +
                  (rule.subdomains ? " · " + app_.tr("Include subdomains") : ""),
              r.x + 28, y + 51, 12, t.muted, r.width - 310);
        button({r.x + r.width - 292, y + 12, 128, 36}, app_.tr("Edit"));
        button({r.x + r.width - 156, y + 12, 128, 36}, app_.tr("Remove"));
    }
    label(c, regular_,
          app_.tr("Private tabs ignore rules. Forms are never resubmitted across containers."),
          r.x + 28, r.y + r.height - 22, 10, t.muted, r.width - 56);
}
void BrowserUi::container_rules_pointer(Rect r, int x, int y) {
    if (inside({r.x + 28, r.y + 108, r.width - 196, 36}, x, y)) {
        container_input_focus_ = true;
        keyboard_focus_ = -1;
        return;
    }
    container_input_focus_ = false;
    if (inside({r.x + r.width - 156, r.y + 108, 128, 36}, x, y)) {
        save_container();
        return;
    }
    const int half = (r.width - 64) / 2;
    if (inside({r.x + 28, r.y + 154, half, 36}, x, y)) {
        container_rule_picker(r);
        return;
    }
    if (inside({r.x + 36 + half, r.y + 154, half, 36}, x, y)) {
        container_rule_subdomains_ = !container_rule_subdomains_;
        app_.changed();
        return;
    }
    const int capacity = std::max(1, (r.height - 260) / 88);
    for (int i = container_scroll_;
         i < int(app_.profile.container_rules.size()) && i < container_scroll_ + capacity; ++i) {
        const int top = r.y + 212 + (i - container_scroll_) * 88;
        if (inside({r.x + r.width - 292, top + 12, 128, 36}, x, y)) {
            const auto rule = app_.profile.container_rules[i];
            container_input_.set(rule.host, true);
            container_rule_target_ = rule.container;
            container_rule_subdomains_ = rule.subdomains;
            container_input_focus_ = true;
            container_error_.clear();
            app_.changed();
            return;
        }
        if (inside({r.x + r.width - 156, top + 12, 128, 36}, x, y)) {
            app_.profile.container_rules.erase(app_.profile.container_rules.begin() + i);
            app_.changed(true);
            return;
        }
    }
}
void BrowserUi::container_rule_picker(Rect r, int page) {
    app_.close_menu();
    keyboard_focus_ = -1;
    const int half = (r.width - 64) / 2;
    app_.menu_x = r.x + 28;
    app_.menu_y = r.y + 194;
    app_.menu_width = half;
    const auto &items = app_.profile.containers;
    page = std::clamp(page, 0, std::max(0, (int(items.size()) - 1) / 6));
    if (page)
        app_.menu.push_back({-1, app_.tr("Previous")});
    for (int i = page * 6; i < std::min(int(items.size()), page * 6 + 6); ++i)
        app_.menu.push_back({items[i].id + 1, app_.profile.container_label(items[i].id)});
    if ((page + 1) * 6 < int(items.size()))
        app_.menu.push_back({-2, app_.tr("Next")});
    app_.menu_action = [this, r, page](int id) {
        if (id < 0) {
            container_rule_picker(r, page + (id == -1 ? -1 : 1));
            return;
        }
        if (app_.profile.container(id - 1))
            container_rule_target_ = id - 1;
        app_.changed();
    };
    app_.changed();
}
std::string BrowserUi::container_rules_report() const {
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    const int half = (r.width - 64) / 2;
    std::string out =
        "{\"rules_mode\":true,\"input\":" + rect({r.x + 28, r.y + 108, r.width - 196, 36}) +
        ",\"save\":" + rect({r.x + r.width - 156, r.y + 108, 128, 36}) +
        ",\"selector\":" + rect({r.x + 28, r.y + 154, half, 36}) +
        ",\"subdomains_button\":" + rect({r.x + 36 + half, r.y + 154, half, 36}) +
        ",\"target\":" + std::to_string(container_rule_target_) +
        ",\"subdomains\":" + (container_rule_subdomains_ ? "true" : "false") +
        ",\"input_length\":" + std::to_string(container_input_.value.size()) +
        ",\"error\":" + json_quote(container_error_) + ",\"items\":[";
    const int capacity = std::max(1, (r.height - 260) / 88);
    for (int i = container_scroll_;
         i < int(app_.profile.container_rules.size()) && i < container_scroll_ + capacity; ++i) {
        if (i != container_scroll_)
            out += ",";
        const auto &rule = app_.profile.container_rules[i];
        out += "{\"host\":" + json_quote(rule.host) +
               ",\"container\":" + std::to_string(rule.container) +
               ",\"subdomains\":" + (rule.subdomains ? "true" : "false") + ",\"edit\":" +
               rect({r.x + r.width - 292, r.y + 224 + (i - container_scroll_) * 88, 128, 36}) +
               ",\"remove\":" +
               rect({r.x + r.width - 156, r.y + 224 + (i - container_scroll_) * 88, 128, 36}) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
std::string BrowserUi::downloads_report() const {
    if (panel_ != Downloads)
        return "null";
    auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"items\":[";
    const auto visible = app_.visible_downloads();
    bool first = true;
    for (int i = panel_scroll_; i < int(visible.size()); ++i) {
        const int y = r.y + 141 + (i - panel_scroll_) * 75;
        if (y + 50 > r.y + r.height - 14)
            break;
        if (!first)
            out += ",";
        first = false;
        const auto &d = app_.downloads[visible[i]];
        out += "{\"id\":" + std::to_string(d.id) +
               ",\"source_window\":" + std::to_string(d.source_window) +
               ",\"action\":" + rect({r.x + r.width - 149, y + 5, 124, 34}) +
               ",\"pause\":" + rect({r.x + r.width - 98, y + 5, 36, 36}) +
               ",\"cancel\":" + rect({r.x + r.width - 57, y + 5, 36, 36}) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::open_task_manager() {
    app_.close_menu();
    panel(TaskManager);
    if (!app_.state()->task_manager)
        app_.state()->task_manager = CefTaskManager::GetTaskManager();
    process_scroll_ = 0;
    keyboard_focus_ = -1;
    refresh_task_manager();
    app_.changed();
}
void BrowserUi::refresh_task_manager() {
    process_rows_ = app_.process_tasks(&process_available_);
    process_refresh_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}
void BrowserUi::draw_task_manager(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    label(c, regular_, app_.tr("CPU 100% = one core."), r.x + 28, r.y + 76, 11, t.muted,
          r.width - 190);
    label(c, regular_, app_.tr("Shared tabs count memory once per process."), r.x + 28, r.y + 95,
          11, t.muted, r.width - 190);
    auto button = [&](Rect b, const char *text, bool enabled = true) {
        if (enabled)
            focus_target(b);
        fill(c, b, 8,
             SkColorSetA(t.accent, enabled && inside(b, hover_x_, hover_y_) ? 35
                                   : enabled                                ? 16
                                                                            : 7));
        border(c, b, 8, SkColorSetA(t.accent, enabled ? 120 : 35));
        centered_label(c, medium_, app_.tr(text), b, 11, enabled ? t.text : t.muted, 5, 5);
    };
    button({r.x + r.width - 140, r.y + 66, 112, 34}, "Refresh");
    label(c, medium_, app_.tr("Process / tabs"), r.x + 28, r.y + 124, 11, t.muted, r.width - 390);
    label(c, medium_, "CPU", r.x + r.width - 336, r.y + 124, 11, t.muted, 75);
    label(c, medium_, app_.tr("Memory"), r.x + r.width - 250, r.y + 124, 11, t.muted, 100);
    const int cap = std::max(1, (r.height - 194) / 82);
    process_scroll_ = std::clamp(process_scroll_, 0, std::max(0, int(process_rows_.size()) - cap));
    if (process_rows_.empty())
        label(c, regular_,
              app_.tr(process_available_ ? "Collecting process data…"
                                         : "Process metrics are unavailable."),
              r.x + 28, r.y + 169, 13, t.muted, r.width - 56);
    for (int i = process_scroll_; i < int(process_rows_.size()) && i < process_scroll_ + cap; ++i) {
        const auto &row = process_rows_[i];
        const int y = r.y + 136 + (i - process_scroll_) * 82;
        const auto title = row.tabs.empty() ? app_.tr(row.kind) : row.tabs.front().title;
        label(c, medium_, title, r.x + 28, y + 20, 13, t.text, r.width - 394);
        std::string detail = row.tabs.empty() ? "" : app_.tr(row.kind);
        if (!row.tabs.empty())
            detail += "  " + app_.tr("Tabs: ") + std::to_string(row.tabs.size());
        label(c, regular_, detail, r.x + 28, y + 42, 10, t.muted, r.width - 394);
        if (row.tabs.size() > 1) {
            std::string others;
            for (size_t n = 1; n < row.tabs.size(); ++n) {
                if (!others.empty())
                    others += ", ";
                others += row.tabs[n].title;
            }
            label(c, regular_, others, r.x + 28, y + 62, 10, t.muted, r.width - 394);
        }
        std::ostringstream cpu;
        if (row.cpu < 0)
            cpu << "—";
        else
            cpu << std::fixed << std::setprecision(1) << row.cpu << "%";
        label(c, regular_, cpu.str(), r.x + r.width - 336, y + 25, 12, t.text, 80);
        label(c, regular_, row.memory < 0 ? app_.tr("Unavailable") : bytes(row.memory),
              r.x + r.width - 250, y + 25, 12, t.text, 102);
        button({r.x + r.width - 140, y + 8, 112, 34}, row.killable ? "End process" : "Protected",
               row.killable);
        fill(c, {r.x + 28, y + 76, r.width - 56, 1}, 0, SkColorSetA(t.muted, 24));
    }
    label(c, regular_,
          app_.tr("Only identified page processes can be ended. All their tabs stop together."),
          r.x + 28, r.y + r.height - 22, 10, t.muted, r.width - 56);
}
void BrowserUi::task_manager_pointer(Rect r, int x, int y) {
    if (inside({r.x + r.width - 140, r.y + 66, 112, 34}, x, y)) {
        refresh_task_manager();
        app_.changed();
        return;
    }
    const int cap = std::max(1, (r.height - 194) / 82);
    for (int i = process_scroll_; i < int(process_rows_.size()) && i < process_scroll_ + cap; ++i) {
        const int top = r.y + 136 + (i - process_scroll_) * 82;
        if (inside({r.x + r.width - 140, top + 8, 112, 34}, x, y) && process_rows_[i].killable) {
            app_.end_process(process_rows_[i]);
            return;
        }
    }
}
std::string BrowserUi::task_manager_report() const {
    if (panel_ != TaskManager)
        return "null";
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    const auto r = panel_bounds();
    const int cap = std::max(1, (r.height - 194) / 82);
    std::string out = "{\"available\":" + std::string(process_available_ ? "true" : "false") +
                      ",\"refresh\":" + rect({r.x + r.width - 140, r.y + 66, 112, 34}) +
                      ",\"rows\":[";
    for (size_t i = 0; i < process_rows_.size(); ++i) {
        if (i)
            out += ",";
        const auto &row = process_rows_[i];
        out += "{\"task\":" + std::to_string(row.task) +
               ",\"process\":" + std::to_string(row.process) + ",\"kind\":" + json_quote(row.kind) +
               ",\"cpu\":" + std::to_string(row.cpu) + ",\"memory\":" + std::to_string(row.memory) +
               ",\"killable\":" + (row.killable ? "true" : "false") + ",\"end\":";
        if (int(i) >= process_scroll_ && int(i) < process_scroll_ + cap)
            out +=
                rect({r.x + r.width - 140, r.y + 144 + (int(i) - process_scroll_) * 82, 112, 34});
        else
            out += "null";
        out += ",\"tabs\":[";
        for (size_t n = 0; n < row.tabs.size(); ++n) {
            if (n)
                out += ",";
            const auto &tab = row.tabs[n];
            out += "{\"window\":" + std::to_string(tab.window) +
                   ",\"tab\":" + std::to_string(tab.tab) + ",\"title\":" + json_quote(tab.title) +
                   "}";
        }
        out += "]}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
const std::vector<int> &BrowserUi::reading_results() const {
    const auto &library = app_.state()->reading_list;
    if (reading_revision_ != library.revision() || reading_query_ != reading_search_.value) {
        reading_ids_ = library.search(reading_search_.value);
        reading_query_ = reading_search_.value;
        reading_revision_ = library.revision();
    }
    return reading_ids_;
}
void BrowserUi::open_reading_list() {
    app_.close_menu();
    panel(ReadingListPanel);
    reading_search_.set("");
    reading_focus_ = true;
    reading_scroll_ = 0;
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
std::array<Rect, 3> BrowserUi::reading_buttons(Rect r, int y) const {
    int width = std::min(160, (r.width - 72) / 3);
    return {{{r.x + 28, y, width, 30},
             {r.x + 36 + width, y, width, 30},
             {r.x + 44 + width * 2, y, width, 30}}};
}
void BrowserUi::draw_reading_list(SkCanvas &c, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &title, bool enabled = true) {
        if (enabled)
            focus_target(b);
        if (enabled && inside(b, hover_x_, hover_y_))
            fill(c, b, 8, SkColorSetA(theme.accent, 28));
        border(c, b, 8, SkColorSetA(theme.accent, enabled ? 140 : 45));
        centered_label(c, medium_, app_.tr(title), b, 12, enabled ? theme.text : theme.muted, 8, 8);
    };
    const auto tab = app_.current();
    const bool pending = tab && (tab->reading_pending || tab->reader_pending);
    button({r.x + 28, r.y + 66, r.width - 56, 34},
           pending ? "Saving article…" : "Save article offline", !pending);
    label(c, regular_, app_.tr("Search saved text"), r.x + 28, r.y + 123, 11, theme.muted);
    Rect search{r.x + 28, r.y + 132, r.width - 56, 36};
    focus_target(search);
    fill(c, search, 8, theme.background);
    border(c, search, 8, SkColorSetA(theme.accent, reading_focus_ ? 255 : 110));
    draw_editor(c, reading_search_, {search.x + 10, search.y + 2, search.width - 20, 32}, 13,
                reading_focus_);
    auto &library = app_.state()->reading_list;
    const auto ids = reading_results();
    int cap = std::max(1, (r.height - 228) / 104);
    reading_scroll_ = std::clamp(reading_scroll_, 0, std::max(0, int(ids.size()) - cap));
    if (ids.empty())
        label(c, regular_,
              app_.tr(library.available()
                          ? "No saved articles match. Open an article and save it here."
                          : "Reading list is damaged or unavailable. The file has been preserved."),
              r.x + 28, r.y + 210, 12, theme.muted, r.width - 56);
    for (int i = reading_scroll_; i < int(ids.size()) && i < reading_scroll_ + cap; ++i) {
        const auto a = library.find(ids[i]);
        int top = r.y + 188 + (i - reading_scroll_) * 104;
        label(c, medium_, a->title, r.x + 28, top + 16, 13, theme.text, r.width - 56);
        const auto time = local_time(std::time_t(a->saved));
        char date[32]{};
        std::strftime(date, sizeof(date), "%Y-%m-%d", &time);
        label(c, regular_, app_.tr(a->read ? "Read" : "Unread") + " / " + date + " / " + a->url,
              r.x + 28, top + 34, 11, theme.muted, r.width - 56);
        const auto buttons = reading_buttons(r, top + 44);
        button(buttons[0], "Open offline");
        button(buttons[1], a->read ? "Mark unread" : "Mark read");
        button(buttons[2], "Delete");
    }
    label(c, regular_,
          std::to_string(ids.size()) + " / " + std::to_string(library.items().size()) + "  " +
              app_.tr("Saved articles"),
          r.x + 28, r.y + r.height - 26, 11, theme.muted, r.width - 56);
    if (int(ids.size()) > cap) {
        int h = cap * 104, thumb = std::max(20, h * cap / int(ids.size()));
        fill(c,
             {r.x + r.width - 14,
              r.y + 188 + (h - thumb) * reading_scroll_ / (int(ids.size()) - cap), 3, thumb},
             1, theme.accent);
    }
}
void BrowserUi::reading_pointer(Rect r, int x, int y) {
    provider_focus_ = -1;
    reading_focus_ = false;
    spelling_focus_ = false;
    extension_site_focus_ = false;
    if (inside({r.x + 28, r.y + 66, r.width - 56, 34}, x, y)) {
        app_.save_reading_article();
        return;
    }
    if (inside({r.x + 28, r.y + 132, r.width - 56, 36}, x, y)) {
        reading_focus_ = true;
        reading_search_.select_all();
        return;
    }
    const auto ids = reading_results();
    int index = reading_scroll_ + (y - r.y - 188) / 104, cap = std::max(1, (r.height - 228) / 104);
    if (y < r.y + 188 || index < reading_scroll_ || index >= int(ids.size()) ||
        index >= reading_scroll_ + cap)
        return;
    const int id = ids[index];
    const auto a = app_.state()->reading_list.find(id);
    if (!a)
        return;
    const auto buttons = reading_buttons(r, r.y + 188 + (index - reading_scroll_) * 104 + 44);
    if (inside(buttons[0], x, y))
        app_.open_reading_article(id);
    else if (inside(buttons[1], x, y)) {
        if (!app_.state()->reading_list.mark(id, !a->read))
            app_.ui->toast(
                app_.tr("Could not update the reading list. Check available disk space."));
    } else if (inside(buttons[2], x, y)) {
        BrowserDialog dialog;
        dialog.origin = app_.tr("Delete offline article");
        dialog.owned = true;
        dialog.accept_label = app_.tr("Delete");
        dialog.message =
            a->title + "\n" + app_.tr("This removes the saved text from your reading list.");
        const auto weak = std::weak_ptr<void>(app_.callback_lifetime);
        const auto saved = *a;
        dialog.action = [this, weak, saved](bool accept, const std::string &) {
            if (weak.expired() || !accept)
                return;
            const auto current = app_.state()->reading_list.find(saved.id);
            if (current && *current == saved && !app_.state()->reading_list.remove(saved.id))
                toast(app_.tr("Could not update the reading list. Check available disk space."));
            app_.changed();
        };
        app_.dialogs.push_back(std::move(dialog));
    }
    app_.changed();
}
std::string BrowserUi::reading_report() const {
    if (panel_ != ReadingListPanel)
        return "null";
    Rect r = panel_bounds();
    auto ids = reading_results();
    int cap = std::max(1, (r.height - 228) / 104);
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"query\":" + json_quote(reading_search_.value) +
                      ",\"count\":" + std::to_string(ids.size()) +
                      ",\"save\":" + rect({r.x + 28, r.y + 66, r.width - 56, 34}) +
                      ",\"search\":" + rect({r.x + 28, r.y + 132, r.width - 56, 36}) +
                      ",\"items\":[";
    for (int i = reading_scroll_; i < int(ids.size()) && i < reading_scroll_ + cap; ++i) {
        if (i != reading_scroll_)
            out += ",";
        const auto a = app_.state()->reading_list.find(ids[i]);
        auto buttons = reading_buttons(r, r.y + 188 + (i - reading_scroll_) * 104 + 44);
        out += "{\"id\":" + std::to_string(a->id) + ",\"title\":" + json_quote(a->title) +
               ",\"read\":" + (a->read ? "true" : "false") + ",\"open\":" + rect(buttons[0]) +
               ",\"mark\":" + rect(buttons[1]) + ",\"delete\":" + rect(buttons[2]) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::open_search_providers() {
    app_.close_menu();
    panel(SearchProviders);
    provider_editing_ = false;
    provider_id_ = provider_scroll_ = 0;
    provider_focus_ = -1;
    provider_error_.clear();
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
std::array<Rect, 3> BrowserUi::provider_buttons(Rect r, int y) const {
    int width = std::min(172, (r.width - 72) / 3);
    return {{{r.x + 28, y, width, 30},
             {r.x + 36 + width, y, width, 30},
             {r.x + 44 + width * 2, y, width, 30}}};
}
void BrowserUi::save_search_provider() {
    SearchProvider p{provider_id_, provider_fields_[0].value, provider_fields_[1].value,
                     provider_fields_[2].value};
    if (!app_.profile.save_search_provider(std::move(p))) {
        provider_error_ =
            app_.tr("Use a unique keyword (a-z, 0-9, _ or -; start with a letter) and an HTTP(S) "
                    "URL containing {searchTerms} once after the host. Limit: 32 engines.");
        app_.changed();
        return;
    }
    provider_editing_ = false;
    provider_focus_ = -1;
    provider_error_.clear();
    app_.changed(true);
}
void BrowserUi::draw_search_providers(SkCanvas &c, Rect r) {
    const auto &t = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &name, bool enabled = true) {
        if (enabled)
            focus_target(b);
        if (enabled && inside(b, hover_x_, hover_y_))
            fill(c, b, 8, SkColorSetA(t.accent, 24));
        if (enabled)
            border(c, b, 8, SkColorSetA(t.accent, 140));
        centered_label(c, medium_, app_.tr(name), b, 12, enabled ? t.text : t.muted, 8, 8);
    };
    if (provider_editing_) {
        const char *names[] = {"Engine name", "Keyword command", "Search URL with {searchTerms}"};
        for (int i = 0; i < 3; ++i) {
            const int y = r.y + 100 + i * 66;
            label(c, regular_, app_.tr(names[i]), r.x + 28, y, 11, t.muted, r.width - 56);
            Rect input{r.x + 28, y + 10, r.width - 56, 36};
            focus_target(input);
            fill(c, input, 8, t.background);
            border(c, input, 8, SkColorSetA(t.accent, provider_focus_ == i ? 255 : 110));
            draw_editor(c, provider_fields_[i], {input.x + 10, input.y + 2, input.width - 20, 32},
                        13, provider_focus_ == i);
        }
        button({r.x + 28, r.y + 310, 168, 34}, "Save search engine");
        button({r.x + 204, r.y + 310, 132, 34}, "Cancel");
        const auto lines = wrap_text(font(regular_, 11), provider_error_, r.width - 56, 5);
        for (size_t i = 0; i < lines.size(); ++i)
            label(c, regular_, lines[i], r.x + 28, r.y + 375 + int(i) * 16, 11, t.muted,
                  r.width - 56);
        return;
    }
    label(c, regular_, app_.tr("Default search") + ": " + app_.profile.current_search_name(),
          r.x + 28, r.y + 78, 12, t.text, r.width - 56);
    button({r.x + 28, r.y + 94, 192, 34}, "Add search engine",
           app_.profile.search_providers.size() < 32);
    const auto &items = app_.profile.search_providers;
    int cap = std::max(1, (r.height - 200) / 88);
    provider_scroll_ = std::clamp(provider_scroll_, 0, std::max(0, int(items.size()) - cap));
    if (items.empty())
        label(c, regular_, app_.tr("Add a URL such as https://example.org/search?q={searchTerms}"),
              r.x + 28, r.y + 180, 12, t.muted, r.width - 56);
    for (int i = provider_scroll_; i < int(items.size()) && i < provider_scroll_ + cap; ++i) {
        const auto &p = items[i];
        int y = r.y + 154 + (i - provider_scroll_) * 88;
        label(c, medium_, p.name + "  [" + p.keyword + "]", r.x + 28, y + 16, 13, t.text,
              r.width - 56);
        label(c, regular_, p.url, r.x + 28, y + 34, 10, t.muted, r.width - 56);
        auto b = provider_buttons(r, y + 42);
        button(b[0],
               app_.profile.default_search_provider == p.id ? "Default search" : "Use as default",
               app_.profile.default_search_provider != p.id);
        button(b[1], "Edit");
        button(b[2], "Delete");
    }
    button({r.x + 28, r.y + r.height - 46, r.width - 56, 30}, "Use built-in search engine",
           app_.profile.default_search_provider != 0);
    if (int(items.size()) > cap) {
        int h = cap * 88, thumb = std::max(20, h * cap / int(items.size()));
        fill(c,
             {r.x + r.width - 14,
              r.y + 154 + (h - thumb) * provider_scroll_ / (int(items.size()) - cap), 3, thumb},
             1, t.accent);
    }
}
void BrowserUi::search_providers_pointer(Rect r, int x, int y) {
    provider_focus_ = -1;
    if (provider_editing_) {
        for (int i = 0; i < 3; ++i)
            if (inside({r.x + 28, r.y + 110 + i * 66, r.width - 56, 36}, x, y)) {
                provider_focus_ = i;
                provider_fields_[i].select_all();
                return;
            }
        if (inside({r.x + 28, r.y + 310, 168, 34}, x, y))
            save_search_provider();
        else if (inside({r.x + 204, r.y + 310, 132, 34}, x, y)) {
            provider_editing_ = false;
            provider_error_.clear();
        }
        app_.changed();
        return;
    }
    if (inside({r.x + 28, r.y + 94, 192, 34}, x, y) && app_.profile.search_providers.size() < 32) {
        provider_editing_ = true;
        provider_id_ = 0;
        provider_focus_ = 0;
        provider_error_.clear();
        for (auto &f : provider_fields_)
            f.set("");
        app_.changed();
        return;
    }
    if (inside({r.x + 28, r.y + r.height - 46, r.width - 56, 30}, x, y)) {
        app_.profile.default_search_provider = 0;
        app_.changed(true);
        return;
    }
    auto &items = app_.profile.search_providers;
    int cap = std::max(1, (r.height - 200) / 88), index = provider_scroll_ + (y - r.y - 154) / 88;
    if (y < r.y + 154 || index < provider_scroll_ || index >= int(items.size()) ||
        index >= provider_scroll_ + cap)
        return;
    auto p = items[index];
    auto b = provider_buttons(r, r.y + 154 + (index - provider_scroll_) * 88 + 42);
    if (inside(b[0], x, y)) {
        app_.profile.default_search_provider = p.id;
        app_.changed(true);
    } else if (inside(b[1], x, y)) {
        provider_editing_ = true;
        provider_id_ = p.id;
        provider_focus_ = 0;
        provider_error_.clear();
        provider_fields_[0].set(p.name);
        provider_fields_[1].set(p.keyword);
        provider_fields_[2].set(p.url);
        app_.changed();
    } else if (inside(b[2], x, y)) {
        BrowserDialog d;
        d.owned = true;
        d.origin = app_.tr("Delete search engine");
        d.accept_label = app_.tr("Delete");
        d.message =
            p.name + "\n" + app_.tr("Removing the default returns search to the built-in engine.");
        auto weak = std::weak_ptr<void>(app_.callback_lifetime);
        d.action = [this, weak, p](bool accept, const std::string &) {
            if (weak.expired() || !accept)
                return;
            const auto current = find_search_provider(app_.profile.search_providers, p.id);
            if (current && current->name == p.name && current->keyword == p.keyword &&
                current->url == p.url) {
                app_.profile.remove_search_provider(p.id);
                app_.changed(true);
            }
        };
        app_.dialogs.push_back(std::move(d));
        app_.changed();
    }
}
std::string BrowserUi::search_providers_report() const {
    if (panel_ != SearchProviders)
        return "null";
    Rect r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"editing\":" + std::string(provider_editing_ ? "true" : "false") +
                      ",\"error\":" + json_quote(provider_error_) +
                      ",\"default\":" + std::to_string(app_.profile.default_search_provider) +
                      ",\"name\":" + json_quote(app_.profile.current_search_name()) +
                      ",\"add\":" + rect({r.x + 28, r.y + 94, 192, 34}) +
                      ",\"builtin\":" + rect({r.x + 28, r.y + r.height - 46, r.width - 56, 30}) +
                      ",\"save\":" + rect({r.x + 28, r.y + 310, 168, 34}) +
                      ",\"cancel\":" + rect({r.x + 204, r.y + 310, 132, 34}) + ",\"fields\":[";
    for (int i = 0; i < 3; ++i) {
        if (i)
            out += ",";
        out += "{\"value\":" + json_quote(provider_fields_[i].value) +
               ",\"bounds\":" + rect({r.x + 28, r.y + 110 + i * 66, r.width - 56, 36}) + "}";
    }
    out += "],\"items\":[";
    int cap = std::max(1, (r.height - 200) / 88);
    for (int i = provider_scroll_;
         i < int(app_.profile.search_providers.size()) && i < provider_scroll_ + cap; ++i) {
        if (i != provider_scroll_)
            out += ",";
        auto p = app_.profile.search_providers[i];
        auto b = provider_buttons(r, r.y + 154 + (i - provider_scroll_) * 88 + 42);
        out += "{\"id\":" + std::to_string(p.id) + ",\"name\":" + json_quote(p.name) +
               ",\"keyword\":" + json_quote(p.keyword) + ",\"default\":" + rect(b[0]) +
               ",\"edit\":" + rect(b[1]) + ",\"delete\":" + rect(b[2]) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::open_duplicates() {
    if (app_.safe_mode()) {
        toast(app_.tr("This tool is paused in safe mode."));
        return;
    }
    app_.close_menu();
    panel(DuplicateTabs);
    duplicate_selected_.reset();
    duplicate_scroll_ = 0;
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    refresh_duplicates();
    app_.changed();
}
void BrowserUi::refresh_duplicates() {
    duplicate_groups_ = group_duplicate_tabs(app_.duplicate_tabs());
    if (duplicate_selected_) {
        auto old = *duplicate_selected_;
        const auto found =
            std::find_if(duplicate_groups_.begin(), duplicate_groups_.end(), [&](const auto &g) {
                return g.url == old.url && g.container == old.container &&
                       g.private_window == old.private_window;
            });
        if (found == duplicate_groups_.end()) {
            duplicate_selected_.reset();
            duplicate_scroll_ = 0;
        } else {
            duplicate_selected_ = *found;
            if (std::none_of(found->tabs.begin(), found->tabs.end(),
                             [&](const auto &t) { return t.ref == duplicate_keep_; }))
                duplicate_keep_ = preferred_duplicate(*found);
        }
    }
    duplicate_refresh_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}
std::array<Rect, 2> BrowserUi::duplicate_buttons(Rect r, int top) const {
    int width = std::min(192, (r.width - 64) / 2);
    return {{{r.x + 28, top, width, 30}, {r.x + 36 + width, top, width, 30}}};
}
void BrowserUi::draw_duplicates(SkCanvas &c, Rect r) {
    const auto &theme = interface_theme(app_.profile);
    auto button = [&](Rect b, const std::string &title, bool enabled = true) {
        if (enabled)
            focus_target(b);
        if (enabled && inside(b, hover_x_, hover_y_))
            fill(c, b, 8, SkColorSetA(theme.accent, 24));
        if (enabled)
            border(c, b, 8, SkColorSetA(theme.accent, 140));
        centered_label(c, medium_, app_.tr(title), b, 12, enabled ? theme.text : theme.muted, 8, 8);
    };
    const auto &merge = app_.state()->duplicate_merge;
    const bool own = merge.requester == app_.window_uid;
    button({r.x + 28, r.y + 66, 160, 32}, duplicate_selected_ ? "Back to matches" : "Refresh");
    std::string message =
        app_.tr("Exact URLs and matching containers; private windows stay separate.");
    if (merge.active && !own)
        message = app_.tr("Duplicate cleanup is running in another window.");
    else if (own && !merge.status.empty())
        message = app_.tr(merge.status) + " " + app_.tr("Closed:") + " " +
                  std::to_string(merge.closed) + " / " + app_.tr("Skipped:") + " " +
                  std::to_string(merge.skipped);
    label(c, regular_, message, r.x + 28, r.y + 119, 11, theme.muted, r.width - 56);
    if (duplicate_selected_)
        label(c, regular_, duplicate_selected_->url, r.x + 28, r.y + 137, 10, theme.muted,
              r.width - 56);
    else
        label(c, regular_,
              app_.tr("Matching groups:") + " " + std::to_string(duplicate_groups_.size()),
              r.x + 28, r.y + 137, 10, theme.muted, r.width - 56);
    int count = duplicate_selected_ ? duplicate_selected_->tabs.size() : duplicate_groups_.size();
    int cap = std::max(1, (r.height - 232) / 92);
    duplicate_scroll_ = std::clamp(duplicate_scroll_, 0, std::max(0, count - cap));
    if (!count)
        label(c, regular_, app_.tr("No duplicate pages found in this context."), r.x + 28,
              r.y + 182, 13, theme.muted, r.width - 56);
    for (int i = duplicate_scroll_; i < count && i < duplicate_scroll_ + cap; ++i) {
        int top = r.y + 152 + (i - duplicate_scroll_) * 92;
        auto b = duplicate_buttons(r, top + 44);
        if (!duplicate_selected_) {
            const auto &g = duplicate_groups_[i];
            label(c, medium_, g.url, r.x + 28, top + 16, 13, theme.text, r.width - 56);
            label(c, regular_,
                  app_.tr("Tabs:") + " " + std::to_string(g.tabs.size()) + " / " +
                      app_.profile.container_label(g.container) +
                      (g.private_window ? " / " + app_.tr("Private tab") : ""),
                  r.x + 28, top + 34, 11, theme.muted, r.width - 56);
            button(b[0], "Review duplicates");
        } else {
            const auto &t = duplicate_selected_->tabs[i];
            label(c, medium_, t.title.empty() ? t.url : t.title, r.x + 28, top + 16, 13, theme.text,
                  r.width - 56);
            std::string detail = app_.tr("Window") + " " + std::to_string(t.ref.window) + " / " +
                                 app_.tr(app_.profile.workspace_label(t.workspace));
            if (t.protection & DuplicatePinned)
                detail += " / " + app_.tr("Pinned");
            if (t.protection & DuplicateBusy)
                detail += " / " + app_.tr("Busy");
            if (t.protection & DuplicateMedia)
                detail += " / " + app_.tr("Media");
            if (t.protection & DuplicateDownload)
                detail += " / " + app_.tr("Active download");
            if (t.protection & DuplicateDialog)
                detail += " / " + app_.tr("Dialog open");
            label(c, regular_, detail, r.x + 28, top + 34, 11, theme.muted, r.width - 56);
            button(b[0], t.ref == duplicate_keep_ ? "Kept tab" : "Keep this tab",
                   !merge.active && t.ref != duplicate_keep_);
            button(b[1], "Show tab");
        }
    }
    if (duplicate_selected_) {
        auto plan = duplicate_close_plan(*duplicate_selected_, duplicate_keep_);
        auto keeper =
            std::find_if(duplicate_selected_->tabs.begin(), duplicate_selected_->tabs.end(),
                         [&](const auto &t) { return t.ref == duplicate_keep_; });
        button({r.x + 28, r.y + r.height - 48, std::min(240, r.width - 232), 32},
               app_.tr("Close duplicates") + " (" + std::to_string(plan.size()) + ")",
               !merge.active && !plan.empty() && keeper != duplicate_selected_->tabs.end() &&
                   !(keeper->protection & DuplicateBusy));
    }
    if (merge.active && own)
        button({r.x + r.width - 188, r.y + r.height - 48, 160, 32}, "Stop remaining");
    if (count > cap) {
        int h = cap * 92, thumb = std::max(20, h * cap / count);
        fill(c,
             {r.x + r.width - 14, r.y + 152 + (h - thumb) * duplicate_scroll_ / (count - cap), 3,
              thumb},
             1, theme.accent);
    }
}
void BrowserUi::duplicates_pointer(Rect r, int x, int y) {
    if (inside({r.x + 28, r.y + 66, 160, 32}, x, y)) {
        if (duplicate_selected_) {
            duplicate_selected_.reset();
            duplicate_scroll_ = 0;
        } else
            refresh_duplicates();
        app_.changed();
        return;
    }
    auto &merge = app_.state()->duplicate_merge;
    if (merge.active && merge.requester == app_.window_uid &&
        inside({r.x + r.width - 188, r.y + r.height - 48, 160, 32}, x, y)) {
        app_.stop_duplicate_merge();
        return;
    }
    if (duplicate_selected_ && !merge.active &&
        inside({r.x + 28, r.y + r.height - 48, std::min(240, r.width - 232), 32}, x, y)) {
        app_.merge_duplicates(*duplicate_selected_, duplicate_keep_);
        return;
    }
    int cap = std::max(1, (r.height - 232) / 92), index = duplicate_scroll_ + (y - r.y - 152) / 92;
    int count = duplicate_selected_ ? duplicate_selected_->tabs.size() : duplicate_groups_.size();
    if (y < r.y + 152 || index < duplicate_scroll_ || index >= count ||
        index >= duplicate_scroll_ + cap)
        return;
    auto b = duplicate_buttons(r, r.y + 152 + (index - duplicate_scroll_) * 92 + 44);
    if (!duplicate_selected_) {
        if (inside(b[0], x, y)) {
            duplicate_selected_ = duplicate_groups_[index];
            if (!merge.active && merge.requester == app_.window_uid)
                merge.status.clear();
            duplicate_keep_ = preferred_duplicate(*duplicate_selected_);
            duplicate_scroll_ = 0;
            app_.changed();
        }
    } else {
        const auto t = duplicate_selected_->tabs[index];
        if (!merge.active && inside(b[0], x, y)) {
            duplicate_keep_ = t.ref;
            app_.changed();
        } else if (inside(b[1], x, y))
            app_.show_duplicate_tab(t.ref);
    }
}
std::string BrowserUi::duplicates_report() const {
    if (panel_ != DuplicateTabs)
        return "null";
    Rect r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    const auto &m = app_.state()->duplicate_merge;
    bool own = m.requester == app_.window_uid;
    std::string out =
        "{\"closable\":" +
        std::to_string(duplicate_selected_
                           ? duplicate_close_plan(*duplicate_selected_, duplicate_keep_).size()
                           : 0) +
        ",\"review\":" + std::string(duplicate_selected_ ? "true" : "false") +
        ",\"groups\":" + std::to_string(duplicate_groups_.size()) +
        ",\"busy\":" + (m.active ? "true" : "false") +
        ",\"status\":" + json_quote(own ? m.status : "") +
        ",\"closed\":" + std::to_string(own ? m.closed : 0) +
        ",\"kept\":" + std::to_string(own ? m.skipped : 0) +
        ",\"keep_window\":" + std::to_string(duplicate_keep_.window) +
        ",\"keep_tab\":" + std::to_string(duplicate_keep_.tab) +
        ",\"back\":" + rect({r.x + 28, r.y + 66, 160, 32}) +
        ",\"close\":" + rect({r.x + 28, r.y + r.height - 48, std::min(240, r.width - 232), 32}) +
        ",\"stop\":" + rect({r.x + r.width - 188, r.y + r.height - 48, 160, 32}) + ",\"items\":[";
    int cap = std::max(1, (r.height - 232) / 92),
        count = duplicate_selected_ ? duplicate_selected_->tabs.size() : duplicate_groups_.size();
    for (int i = duplicate_scroll_; i < count && i < duplicate_scroll_ + cap; ++i) {
        if (i != duplicate_scroll_)
            out += ",";
        auto b = duplicate_buttons(r, r.y + 152 + (i - duplicate_scroll_) * 92 + 44);
        if (!duplicate_selected_) {
            const auto &g = duplicate_groups_[i];
            out +=
                "{\"url\":" + json_quote(g.url) + ",\"container\":" + std::to_string(g.container) +
                ",\"private\":" + (g.private_window ? "true" : "false") +
                ",\"count\":" + std::to_string(g.tabs.size()) + ",\"review\":" + rect(b[0]) + "}";
        } else {
            const auto &t = duplicate_selected_->tabs[i];
            out += "{\"window\":" + std::to_string(t.ref.window) +
                   ",\"tab\":" + std::to_string(t.ref.tab) +
                   ",\"workspace\":" + std::to_string(t.workspace) +
                   ",\"title\":" + json_quote(t.title) +
                   ",\"protection\":" + std::to_string(t.protection) + ",\"keep\":" + rect(b[0]) +
                   ",\"show\":" + rect(b[1]) + "}";
        }
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::open_tab_menu(int index, int x, int y) {
    const int id = app_.tabs[index]->id;
    app_.menu_width = 280;
    app_.menu = {
        {1, app_.tabs[index]->pinned ? app_.tr("Unpin tab") : app_.tr("Pin tab")},
        {2, app_.tabs[index]->muted ? app_.tr("Unmute tab") : app_.tr("Mute tab")},
        {3, app_.tr("Duplicate tab")},
        {4, app_.tr("Close tab")},
        {5, app_.tr("Reopen closed tab")},
        {6, app_.tr("Move to workspace"), !app_.private_window},
        {7, app_.tr("Tab groups"), !app_.private_window},
        {8, app_.tr("Sleep tab"),
         !app_.tabs[index]->pinned && !app_.tabs[index]->private_tab && !app_.tabs[index]->ntp},
        {9, app_.tr("Open in container"), !app_.tabs[index]->private_tab}};
    app_.menu.push_back({13, app_.tr("Move to window")});
    if (app_.profile.tree_tabs) {
        const auto rows = app_.tree_rows(false);
        const auto row =
            std::find_if(rows.begin(), rows.end(), [&](const auto &r) { return r.index == index; });
        app_.menu.push_back(
            {10, app_.tr(app_.tabs[index]->tree_collapsed ? "Expand branch" : "Collapse branch"),
             row != rows.end() && row->descendants > 0});
        app_.menu.push_back({11, app_.tr("Detach tab"), app_.tabs[index]->parent != 0});
        app_.menu.push_back({12, app_.tr("New child tab")});
    }
    app_.menu_x = std::clamp(x + 8, 8, width_ - app_.menu_width - 8);
    app_.menu_y = std::clamp(y, 32, std::max(32, height_ - int(app_.menu.size()) * 32 - 20));
    app_.menu_action = [this, id](int command) {
        auto tab = app_.tab(id);
        if (!tab)
            return;
        int index = 0;
        while (index < static_cast<int>(app_.tabs.size()) && app_.tabs[index]->id != id)
            ++index;
        app_.activate(index);
        if (command == 1)
            app_.pin();
        else if (command == 2)
            app_.mute();
        else if (command == 3)
            app_.new_tab(tab->page.url, tab->private_tab, true, false, tab->workspace, tab->group,
                         tab->container, tab->parent);
        else if (command == 4)
            app_.close_tab(index);
        else if (command == 6)
            open_workspaces(id);
        else if (command == 7)
            open_groups(id);
        else if (command == 8)
            app_.sleep_tab(id);
        else if (command == 9)
            open_containers(id);
        else if (command == 10)
            app_.toggle_tree(id);
        else if (command == 11)
            app_.detach_tree_tab(id);
        else if (command == 13)
            open_tab_windows(id);
        else if (command == 12)
            app_.new_tab("aura://newtab", tab->private_tab, true, false, tab->workspace, tab->group,
                         tab->container, id);
        else
            app_.reopen();
    };
}
void BrowserUi::open_aura_menu(bool tools) {
    auto t = app_.current();
    app_.close_menu();
    app_.menu = {{1, app_.tr("New window"), true, "Ctrl+N"},
                 {2, app_.tr("New private window"), true, "Ctrl+Shift+N"},
                 {3, app_.tr("Close window"), true, "Ctrl+Shift+W"},
                 {5, app_.tr("Restore previous session"),
                  !app_.state()->previous_restored && !app_.state()->previous_windows.empty()},
                 {4, app_.tr("Exit Aura"), true, "Ctrl+Q"},
                 {6, app_.tr("Workspaces"), !app_.private_window, "Ctrl+Shift+E"},
                 {7, app_.tr("Tab groups"), !app_.private_window, "Ctrl+Alt+G"},
                 {8, app_.tr(app_.split_enabled() ? "Exit split view" : "Two pages side by side"),
                  true, "Ctrl+Alt+V"},
                 {9, app_.tr("Site containers"), !app_.private_window, "Ctrl+Alt+C"},
                 {10, app_.tr("Profiles"), true, "Ctrl+Alt+P"},
                 {11, app_.tr("Command palette"), true, "Ctrl+K"},
                 {12, app_.tr(t && t->reader_active ? "Exit reader" : "Reader mode"),
                  t && t->browser && !t->ntp, "Ctrl+Alt+R"},
                 {13, app_.tr("Translate"), t && t->browser && !t->ntp, "Ctrl+Alt+T"},
                 {14, app_.tr("Site permissions"), true, "Ctrl+Alt+I"},
                 {15, app_.tr("Site data"), true, "Ctrl+Alt+D"},
                 {16, app_.tr("Session snapshots"), true, "Ctrl+Alt+E"},
                 {17, app_.tr("Screenshot"), true, "Ctrl+Shift+S"},
                 {18, app_.tr("Keyboard shortcuts"), true},
                 {19, app_.tr("Task manager"), true}};
    if (horizontal_tabs() && !tools)
        app_.menu = {{1, app_.tr("New window")},
                     {2, app_.tr("New private window")},
                     {21, app_.tr("Bookmarks")},
                     {22, app_.tr("History")},
                     {23, app_.tr("Downloads")},
                     {24, app_.tr("Settings")},
                     {6, app_.tr("Workspaces"), !app_.private_window},
                     {7, app_.tr("Tab groups"), !app_.private_window},
                     {9, app_.tr("Site containers"), !app_.private_window},
                     {10, app_.tr("Profiles")},
                     {25, app_.tr("Manage extensions")},
                     {11, app_.tr("Command palette")},
                     {26, app_.tr("More tools")},
                     {4, app_.tr("Exit Aura")}};
    const std::map<int, std::string> actions = {
        {1, "new_window"},   {2, "private_window"}, {3, "close_window"}, {4, "quit"},
        {6, "workspaces"},   {7, "groups"},         {8, "split"},        {9, "containers"},
        {10, "profiles"},    {11, "commands"},      {12, "reader"},      {13, "translate_page"},
        {14, "permissions"}, {15, "site_data"},     {16, "sessions"},    {17, "screenshot"},
        {18, "hotkeys"},     {19, "task_manager"},  {21, "bookmarks"},   {22, "history"},
        {23, "downloads"},   {24, "settings"}};
    for (auto &item : app_.menu)
        if (auto found = actions.find(item.id); found != actions.end())
            item.shortcut = shortcut_hint(found->second);
    app_.menu_width = 320;
    app_.menu_x = 16;
    app_.menu_y = std::clamp(76, 0, std::max(0, height_ - int(app_.menu.size()) * 32 - 12));
    app_.menu_action = [this](int command) {
        if (command == 21)
            panel(Bookmarks);
        else if (command == 22)
            panel(History);
        else if (command == 23)
            panel(Downloads);
        else if (command == 24)
            panel(Settings);
        else if (command == 25)
            panel(Extensions);
        else if (command == 26)
            open_aura_menu(true);
        else if (command == 19)
            open_task_manager();
        else if (command == 1 || command == 2)
            app_.new_window(command == 2);
        else if (command == 3)
            app_.request_quit();
        else if (command == 5)
            app_.restore_previous_session();
        else if (command == 6)
            open_workspaces();
        else if (command == 7)
            open_groups();
        else if (command == 8)
            app_.toggle_split();
        else if (command == 9)
            open_containers();
        else if (command == 10)
            open_profiles();
        else if (command == 11)
            open_commands();
        else if (command == 12)
            app_.toggle_reader();
        else if (command == 13)
            open_translation(app_.current() && !app_.current()->selected_text.empty());
        else if (command == 14)
            open_permissions();
        else if (command == 15)
            open_site_data();
        else if (command == 16)
            open_sessions();
        else if (command == 17)
            open_screenshot();
        else if (command == 18)
            open_hotkeys();
        else
            app_.request_exit();
    };
    return;
}
} // namespace aura
namespace aura {
bool BrowserUi::horizontal_tabs() const { return app_.profile.tab_layout == "horizontal"; }
TabStripLayout BrowserUi::horizontal_layout() const {
    return tab_strip_layout(width_, int(sidebar_rows().size()), strip_scroll_);
}
bool BrowserUi::caption_hit(int x, int y) const {
    if ((app_.menu_action || app_.menu_callback) &&
        inside({app_.menu_x, app_.menu_y, app_.menu_width, int(app_.menu.size()) * 32 + 12}, x, y))
        return false;
    return horizontal_tabs() ? tab_strip_caption(horizontal_layout(), x, y)
                             : y >= 0 && y < 30 && x >= 240 && x < width_ - 144;
}
bool BrowserUi::sync_tab_layout() {
    if (strip_mode_ == horizontal_tabs())
        return false;
    strip_mode_ = horizontal_tabs();
    app_.cancel_tab_drag();
    drag_tab_ = -1;
    drag_source_ = 0;
    strip_hint_id_ = 0;
    strip_hint_visible_ = 0;
    strip_hint_ready_ = false;
    resize(width_, height_);
    return true;
}
void BrowserUi::draw_tab_strip(SkCanvas &c) {
    const auto theme = interface_theme(app_.profile);
    const auto rows = sidebar_rows();
    const auto tree = app_.tree_rows(false);
    auto selected = std::find(rows.begin(), rows.end(), app_.active);
    if (selected == rows.end()) {
        auto current = app_.current();
        int remaining = int(app_.tabs.size());
        while (current && current->parent && selected == rows.end() && remaining-- > 0) {
            current = app_.tab(current->parent);
            if (current)
                for (auto it = rows.begin(); it != rows.end(); ++it)
                    if (*it >= 0 && app_.tabs[*it]->id == current->id) {
                        selected = it;
                        break;
                    }
        }
        if (selected == rows.end() && app_.current() && app_.current()->group)
            selected = std::find(rows.begin(), rows.end(), -app_.current()->group);
    }
    const int position = selected == rows.end() ? -1 : int(selected - rows.begin());
    const int id = app_.current() ? app_.current()->id : 0;
    auto layout = horizontal_layout();
    if (id != strip_active_id_ || position != strip_active_row_) {
        if (position >= 0 && position < layout.first)
            strip_scroll_ = position;
        else if (position >= layout.first + layout.capacity)
            strip_scroll_ = position - layout.capacity + 1;
        strip_active_id_ = id;
        strip_active_row_ = position;
    }
    layout = horizontal_layout();
    strip_scroll_ = layout.first;
    auto button = [&](Rect r, const char *name, bool enabled) {
        focus_target(r);
        if (enabled && inside(r, hover_x_, hover_y_))
            fill(c, r, 6, SkColorSetA(theme.accent, 25));
        icon(c, name, r.x + (r.width - 16) / 2, r.y + 6, 16, enabled ? theme.text : theme.muted);
    };
    if (layout.overflow) {
        button(layout.previous, "back", layout.first > 0);
        button(layout.next, "forward", layout.first + layout.capacity < int(rows.size()));
    }
    button(layout.add, "plus", true);
    for (const auto &slot : layout.slots) {
        const int index = rows[slot.position];
        const auto r = slot.bounds;
        const bool active = slot.position == position;
        const bool hover = inside(r, hover_x_, hover_y_);
        if (active || hover)
            fill(c, r, 7, SkColorSetA(theme.surface, active ? 245 : 140));
        if (active)
            fill(c, {r.x + 8, r.y + r.height - 2, r.width - 16, 2}, 1, theme.accent);
        if (index < 0) {
            const auto g = app_.profile.group(-index);
            if (!g)
                continue;
            const auto color = tab_group_color(app_.profile, g->color);
            focus_target({r.x, r.y, r.width - 24, r.height});
            focus_target({r.x + r.width - 24, r.y, 24, r.height});
            icon(c, g->collapsed ? "expand" : "collapse", r.x + 6, r.y + 6, 16, color);
            label(c, medium_, g->name, r.x + 28, r.y + 19, 12, color, r.width - 52);
            icon(c, "plus", r.x + r.width - 20, r.y + 7, 14, color);
            continue;
        }
        const auto &tab = *app_.tabs[index];
        const auto node =
            std::find_if(tree.begin(), tree.end(), [&](const auto &r) { return r.index == index; });
        const bool children = app_.profile.tree_tabs && node != tree.end() && node->descendants > 0;
        const int left = r.x + (children ? 28 : 8);
        if (children) {
            focus_target({r.x + 4, 4, 20, 24});
            icon(c, tab.tree_collapsed ? "expand" : "collapse", r.x + 6, r.y + 6, 16, theme.accent);
        }
        focus_target({left, r.y, r.x + r.width - 24 - left, r.height});
        if (!tab.pinned)
            focus_target({r.x + r.width - 24, r.y, 24, r.height});
        if (tab.loading)
            fill(c, {left + 2, r.y + 9, 10, 10}, 5, theme.accent);
        else if (tab.sleeping)
            label(c, medium_, "z", left + 2, r.y + 20, 14, theme.muted);
        else if (tab.media_capture)
            icon(c, tab.media_video ? "camera" : "microphone", left, r.y + 6, 16, theme.accent);
        else if (tab.private_tab)
            icon(c, "private", left, r.y + 6, 16, theme.accent);
        else if (app_.profile.show_favicons && tab.favicon.generation())
            image(c, tab.favicon, {left, r.y + 6, 16, 16});
        else
            icon(c, tab.ntp ? "plus" : "globe", left, r.y + 6, 16, theme.muted);
        label(c, active ? medium_ : regular_,
              tab.page.title.empty() ? url_host(tab.page.url) : tab.page.title, left + 22, r.y + 19,
              12, theme.text, r.x + r.width - 26 - left - 22);
        if (auto g = app_.profile.group(tab.group))
            fill(c, {r.x + 6, r.y + 1, r.width - 12, 2}, 1,
                 tab_group_color(app_.profile, g->color));
        icon(c, tab.pinned ? "pin" : "close", r.x + r.width - 19, r.y + 8, 12, theme.muted);
    }
}
bool BrowserUi::tab_strip_pointer(const PointerInput &input) {
    const int x = input.point.x, y = input.point.y;
    const auto rows = sidebar_rows();
    const auto layout = horizontal_layout();
    if (input.kind == PointerInput::Move && drag_tab_ >= 0 && std::abs(x - drag_x_) > 18) {
        if (y >= 32 || y < 0)
            return false;
        const int dragged = app_.tabs[drag_tab_]->id;
        for (const auto &slot : layout.slots)
            if (inside(slot.bounds, x, y)) {
                const int target = rows[slot.position];
                if (target < 0)
                    app_.set_tab_group(dragged, -target, true);
                else {
                    if (!app_.profile.tree_tabs)
                        app_.set_tab_group(dragged, app_.tabs[target]->group);
                    app_.move_tab(drag_tab_, target);
                    for (int i = 0; i < int(app_.tabs.size()); ++i)
                        if (app_.tabs[i]->id == dragged)
                            drag_tab_ = i;
                }
                drag_x_ = x;
                return true;
            }
        return true;
    }
    if (y < 0 || y >= 32)
        return false;
    if (input.kind == PointerInput::Wheel) {
        if (app_.profile.wheel_switch)
            app_.cycle_tab(input.dy < 0 ? 1 : -1);
        else
            strip_scroll_ = std::clamp(layout.first + ((input.dy < 0 || input.dx > 0) ? 1 : -1), 0,
                                       std::max(0, int(rows.size()) - layout.capacity));
        app_.changed();
        return true;
    }
    if (input.kind != PointerInput::Down)
        return false;
    if (input.button == 1 && inside(layout.add, x, y)) {
        app_.open_new_tab(app_.current() && app_.current()->private_tab);
        return true;
    }
    if (layout.overflow && input.button == 1 &&
        (inside(layout.previous, x, y) || inside(layout.next, x, y))) {
        strip_scroll_ = std::clamp(layout.first + (inside(layout.next, x, y) ? 1 : -1), 0,
                                   std::max(0, int(rows.size()) - layout.capacity));
        app_.changed();
        return true;
    }
    for (const auto &slot : layout.slots)
        if (inside(slot.bounds, x, y)) {
            const int index = rows[slot.position];
            if (index < 0) {
                auto g = app_.profile.group(-index);
                if (!g)
                    return true;
                if (input.button == 3) {
                    open_groups();
                    group_edit_ = g->id;
                    group_input_.set(g->name, true);
                    group_color_ = g->color;
                    group_input_focus_ = true;
                } else if (input.button == 1 && x >= slot.bounds.x + slot.bounds.width - 24)
                    app_.new_tab("aura://newtab", app_.current() && app_.current()->private_tab,
                                 true, false, app_.workspace, g->id);
                else if (input.button == 1) {
                    g->collapsed = !g->collapsed;
                    app_.changed(true);
                }
                return true;
            }
            const auto nodes = app_.tree_rows(false);
            const auto node = std::find_if(nodes.begin(), nodes.end(),
                                           [&](const auto &r) { return r.index == index; });
            if (input.button == 1 && app_.profile.tree_tabs && node != nodes.end() &&
                node->descendants && x < slot.bounds.x + 24) {
                app_.toggle_tree(app_.tabs[index]->id);
                return true;
            }
            if (input.button == 2 && !app_.profile.middle_close)
                return true;
            if (input.button == 2 ||
                (input.button == 1 && x >= slot.bounds.x + slot.bounds.width - 24 &&
                 !app_.tabs[index]->pinned))
                app_.close_tab(index);
            else if (input.button == 1 || input.button == 3) {
                app_.activate(index);
                if (input.button == 3)
                    open_tab_menu(index, x, y + 30);
                else {
                    drag_tab_ = index;
                    drag_source_ = app_.tabs[index]->id;
                    drag_x_ = x;
                    drag_y_ = y;
                }
            }
            return true;
        }
    return false;
}
std::string BrowserUi::tab_strip_report() const {
    if (!horizontal_tabs())
        return "null";
    const auto rows = sidebar_rows();
    const auto layout = horizontal_layout();
    const auto nodes = app_.tree_rows(false);
    auto rect = [](Rect r) {
        return "[" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
               std::to_string(r.width) + "," + std::to_string(r.height) + "]";
    };
    std::string out = "{\"first\":" + std::to_string(layout.first) +
                      ",\"capacity\":" + std::to_string(layout.capacity) +
                      ",\"overflow\":" + (layout.overflow ? "true" : "false") +
                      ",\"add\":" + rect(layout.add) + ",\"previous\":" + rect(layout.previous) +
                      ",\"next\":" + rect(layout.next) + ",\"caption\":" + rect(layout.caption) +
                      ",\"menu\":[12,37,34,36],\"web\":" + rect(web_bounds()) + ",\"items\":[";
    bool first = true;
    for (const auto &slot : layout.slots) {
        if (!first)
            out += ',';
        first = false;
        const int index = rows[slot.position];
        const auto node = std::find_if(nodes.begin(), nodes.end(),
                                       [&](const auto &r) { return r.index == index; });
        const bool children =
            index >= 0 && app_.profile.tree_tabs && node != nodes.end() && node->descendants > 0;
        out +=
            "{\"position\":" + std::to_string(slot.position) +
            ",\"tab_index\":" + std::to_string(index < 0 ? -1 : index) +
            ",\"id\":" + std::to_string(index < 0 ? 0 : app_.tabs[index]->id) +
            ",\"group\":" + std::to_string(index < 0 ? -index : app_.tabs[index]->group) +
            ",\"bounds\":" + rect(slot.bounds) +
            ",\"disclosure\":" + (children ? rect({slot.bounds.x + 4, 4, 20, 24}) : "null") +
            ",\"close\":" +
            rect({slot.bounds.x + slot.bounds.width - 24, slot.bounds.y, 24, slot.bounds.height}) +
            "}";
    }
    return out + "],\"hint\":" + std::to_string(strip_hint_visible_) +
           ",\"focus\":" + std::to_string(keyboard_focus_) + ",\"focus_bounds\":" +
           (keyboard_focus_ >= 0 && keyboard_focus_ < int(focus_targets_.size())
                ? rect(focus_targets_[keyboard_focus_])
                : "null") +
           "}";
}
} // namespace aura

namespace aura {
void BrowserUi::draw_tab_hint(SkCanvas &c) {
    strip_hint_visible_ = 0;
    if (!horizontal_tabs() || panel_ != None || editing || find_open || !app_.dialogs.empty() ||
        app_.menu_action || app_.menu_callback || app_.extension_popup || drag_tab_ >= 0)
        return;
    const auto rows = sidebar_rows();
    const auto layout = horizontal_layout();
    const bool keyboard = keyboard_focus_ >= 0 && keyboard_focus_ < int(focus_targets_.size());
    int x = hover_x_, y = hover_y_;
    if (keyboard) {
        const auto focus = focus_targets_[keyboard_focus_];
        x = focus.x + focus.width / 2;
        y = focus.y + focus.height / 2;
    } else if (!strip_hint_ready_)
        return;
    for (const auto &slot : layout.slots)
        if (inside(slot.bounds, x, y)) {
            const int index = rows[slot.position];
            const int id = index < 0 ? index : app_.tabs[index]->id;
            if (!keyboard && id != strip_hint_id_)
                return;
            std::string title, address, context;
            if (index < 0) {
                const auto group = app_.profile.group(-index);
                if (!group)
                    return;
                title = group->name;
                int count = 0;
                for (const auto &t : app_.tabs)
                    if (t->group == group->id && t->workspace == app_.workspace && !t->page.closed)
                        ++count;
                context = app_.tr("Tabs") + ": " + std::to_string(count);
            } else {
                const auto &tab = *app_.tabs[index];
                title = tab.page.title.empty() ? url_host(tab.page.url) : tab.page.title;
                address = tab.page.url;
                context = tab.private_tab ? app_.tr("Private browsing")
                                          : app_.profile.container_label(tab.container);
                if (auto g = app_.profile.group(tab.group))
                    context += " / " + g->name;
            }
            const auto theme = interface_theme(app_.profile);
            const int width = std::min(420, width_ - 24);
            const auto lines = wrap_text(font(regular_, 12), title, width - 24, 3);
            Rect r{std::clamp(slot.bounds.x, 12, width_ - width - 12), 82, width,
                   30 + int(lines.size()) * 16 + (address.empty() ? 0 : 18)};
            shadow(c, r, 8);
            fill(c, r, 8, theme.surface);
            int baseline = r.y + 21;
            for (const auto &line : lines) {
                label(c, medium_, line, r.x + 12, baseline, 12, theme.text, width - 24);
                baseline += 16;
            }
            if (!address.empty()) {
                label(c, regular_, address, r.x + 12, baseline, 11, theme.muted, width - 24);
                baseline += 18;
            }
            label(c, regular_, context, r.x + 12, baseline, 11, theme.muted, width - 24);
            strip_hint_visible_ = id;
            return;
        }
}
} // namespace aura

namespace aura {
void BrowserUi::open_tab_windows(int id) {
    open_commands();
    move_window_tab_ = id;
}
void BrowserUi::cancel_tab_gesture() {
    drag_cancelled_ = true;
    app_.cancel_tab_drag();
}
bool BrowserUi::tab_drop_at(int x, int y, int &before, Rect &marker) const {
    if (panel_ != None || editing || find_open || app_.menu_action || app_.menu_callback ||
        !app_.dialogs.empty() || app_.extension_popup)
        return false;
    const auto rows = sidebar_rows();
    before = 0;
    auto root_id = [&](int index) {
        auto t = app_.tabs[index].get();
        int remaining = int(app_.tabs.size());
        while (app_.profile.tree_tabs && t->parent && remaining-- > 0) {
            auto parent = app_.tab(t->parent);
            if (!parent)
                break;
            t = parent;
        }
        return t->id;
    };
    if (horizontal_tabs()) {
        const auto layout = horizontal_layout();
        if (x < 12 || x >= width_ - 144 || y < 0 || y >= 32 || inside(layout.add, x, y) ||
            (layout.overflow && (inside(layout.previous, x, y) || inside(layout.next, x, y))))
            return false;
        for (const auto &slot : layout.slots)
            if (x < slot.bounds.x + slot.bounds.width / 2 && rows[slot.position] >= 0) {
                before = root_id(rows[slot.position]);
                int left = layout.slots.front().bounds.x;
                for (const auto &root_slot : layout.slots)
                    if (rows[root_slot.position] >= 0 &&
                        app_.tabs[rows[root_slot.position]]->id == before)
                        left = root_slot.bounds.x;
                marker = {left, 2, 3, 28};
                return true;
            }
        marker = {std::max(12, layout.add.x - 5), 2, 3, 28};
    } else {
        if (x < 12 || x >= 228 || y < 178 || y >= height_ - 220)
            return false;
        int position = tab_scroll_ + (y - 178 + 26) / 52;
        for (int i = position; i < int(rows.size()); ++i)
            if (rows[i] >= 0) {
                before = root_id(rows[i]);
                for (int root = 0; root < int(rows.size()); ++root)
                    if (rows[root] >= 0 && app_.tabs[rows[root]]->id == before)
                        position = root;
                break;
            }
        marker = {
            16,
            std::min(height_ - 223,
                     178 + std::max(0, std::min(position, int(rows.size())) - tab_scroll_) * 52),
            208, 3};
    }
    return true;
}
void BrowserUi::draw_tab_drop(SkCanvas &c) {
    const auto &drop = app_.state()->tab_drop;
    if (drop.target != app_.window_uid)
        return;
    const auto theme = interface_theme(app_.profile);
    fill(c, drop.marker, 1, theme.accent);
    if (horizontal_tabs()) {
        const Rect hint{12, 82, 208, 32};
        fill(c, hint, 6, theme.surface);
        label(c, medium_, app_.tr("Move tab here"), hint.x + 10, hint.y + 21, 12, theme.text,
              hint.width - 20);
    }
}
} // namespace aura

namespace aura {
void BrowserUi::open_spelling_dictionary() {
    if (app_.safe_mode()) {
        toast(app_.tr("This tool is paused in safe mode."));
        return;
    }
    panel(SpellingDictionary);
    spelling_private_ = app_.private_window || (app_.current() && app_.current()->private_tab);
    spelling_input_.set("");
    spelling_error_.clear();
    spelling_scroll_ = 0;
    spelling_focus_ = true;
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
std::vector<std::string> BrowserUi::spelling_results() const {
    std::vector<std::string> result;
    const auto &words =
        spelling_private_ ? app_.state()->private_spelling_words : app_.profile.spelling_words;
    const auto query = fold_settings(spelling_input_.value);
    for (const auto &word : words)
        if (fold_settings(word).find(query) != std::string::npos)
            result.push_back(word);
    return result;
}
void BrowserUi::save_spelling_word() {
    if (app_.change_spelling_word(spelling_input_.value, true, spelling_private_)) {
        spelling_input_.set("");
        spelling_error_.clear();
        spelling_scroll_ = 0;
    } else
        spelling_error_ = app_.tr("Use one new word, up to 99 UTF-8 bytes. Limit: 500 words.");
    spelling_focus_ = true;
    app_.changed();
}
void BrowserUi::draw_spelling(SkCanvas &c, Rect r) {
    const auto t = interface_theme(app_.profile);
    const auto words = spelling_results();
    label(c, regular_,
          app_.tr(spelling_private_
                      ? "Private words disappear when the private session ends."
                      : "Saved words apply to this profile. Checking stays on your device."),
          r.x + 28, r.y + 76, 12, t.muted, r.width - 56);
    if (!app_.state()->spelling_resources_ready)
        label(c, medium_,
              app_.tr("Spelling dictionaries are missing. Reinstall Aura to restore them."),
              r.x + 28, r.y + 98, 11, t.text, r.width - 56);
    label(c, regular_, app_.tr("Find or add a word"), r.x + 28, r.y + 122, 12, t.muted,
          r.width - 56);
    Rect input{r.x + 28, r.y + 134, r.width - 184, 36},
        add{r.x + r.width - 144, r.y + 134, 116, 36};
    focus_target(input);
    focus_target(add);
    fill(c, input, 8, t.background);
    border(c, input, 8, spelling_focus_ ? t.accent : t.muted);
    draw_editor(c, spelling_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                spelling_focus_);
    border(c, add, 8, t.accent);
    centered_label(c, medium_, app_.tr("Add word"), add, 12, t.text, 8, 8);
    if (!spelling_error_.empty())
        label(c, regular_, spelling_error_, r.x + 28, r.y + 193, 11, t.text, r.width - 56);
    else
        label(
            c, regular_,
            app_.tr(spelling_private_
                        ? "Saved profile words are also used; this list contains private additions."
                        : "Enter adds a word. Select Delete to check it again."),
            r.x + 28, r.y + 193, 11, t.muted, r.width - 56);
    const int cap = std::max(1, (r.height - 252) / 40);
    spelling_scroll_ = std::clamp(spelling_scroll_, 0, std::max(0, int(words.size()) - cap));
    if (words.empty())
        label(c, regular_, app_.tr("No matching words. Add a word above."), r.x + 28, r.y + 236, 13,
              t.muted, r.width - 56);
    for (int i = spelling_scroll_; i < int(words.size()) && i < spelling_scroll_ + cap; ++i) {
        const int y = r.y + 212 + (i - spelling_scroll_) * 40;
        label(c, medium_, words[i], r.x + 28, y + 22, 13, t.text, r.width - 160);
        Rect remove{r.x + r.width - 116, y, 88, 32};
        focus_target(remove);
        if (inside(remove, hover_x_, hover_y_))
            fill(c, remove, 6, SkColorSetA(t.accent, 24));
        centered_label(c, regular_, app_.tr("Delete"), remove, 12, t.muted, 8, 8);
    }
    label(c, regular_, std::to_string(words.size()) + " / " + std::to_string(spelling_word_limit),
          r.x + 28, r.y + r.height - 18, 11, t.muted, r.width - 56);
}
void BrowserUi::spelling_pointer(Rect r, int x, int y) {
    spelling_focus_ = inside({r.x + 28, r.y + 134, r.width - 184, 36}, x, y);
    if (spelling_focus_) {
        spelling_input_.select_all();
        return;
    }
    if (inside({r.x + r.width - 144, r.y + 134, 116, 36}, x, y)) {
        save_spelling_word();
        return;
    }
    const auto words = spelling_results();
    const int cap = std::max(1, (r.height - 252) / 40), i = spelling_scroll_ + (y - r.y - 212) / 40;
    if (y >= r.y + 212 && i >= spelling_scroll_ && i < int(words.size()) &&
        i < spelling_scroll_ + cap &&
        inside({r.x + r.width - 116, r.y + 212 + (i - spelling_scroll_) * 40, 88, 32}, x, y))
        app_.change_spelling_word(words[i], false, spelling_private_);
}
std::string BrowserUi::spelling_report() const {
    if (panel_ != SpellingDictionary)
        return "null";
    const auto r = panel_bounds();
    const auto words = spelling_results();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out =
        "{\"private\":" + std::string(spelling_private_ ? "true" : "false") +
        ",\"query\":" + json_quote(spelling_input_.value) +
        ",\"error\":" + json_quote(spelling_error_) + ",\"count\":" + std::to_string(words.size()) +
        ",\"input\":" + rect({r.x + 28, r.y + 134, r.width - 184, 36}) +
        ",\"add\":" + rect({r.x + r.width - 144, r.y + 134, 116, 36}) + ",\"items\":[";
    const int cap = std::max(1, (r.height - 252) / 40);
    for (int i = spelling_scroll_; i < int(words.size()) && i < spelling_scroll_ + cap; ++i) {
        if (i != spelling_scroll_)
            out += ',';
        out += "{\"word\":" + json_quote(words[i]) + ",\"delete\":" +
               rect({r.x + r.width - 116, r.y + 212 + (i - spelling_scroll_) * 40, 88, 32}) + "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::open_extension_access(const std::string &id) {
    if (app_.private_window ||
        (app_.current() && (app_.current()->private_tab || app_.current()->container))) {
        toast(app_.tr("Site access is managed from a regular tab in the default container."));
        return;
    }
    app_.edit_extension_access(id);
    panel(ExtensionAccess);
    extension_access_id_ = id;
    extension_site_scroll_ = 0;
    extension_sites_draft_ = false;
    extension_site_input_.set(app_.current() ? permission_origin(app_.current()->page.url) : "");
    extension_site_focus_ = false;
    extension_site_error_.clear();
    if (auto tab = app_.current(); tab && tab->browser)
        tab->browser->GetHost()->SetFocus(false);
    app_.changed();
}
void BrowserUi::add_extension_site() {
    const auto &s = app_.state()->extension_access.snapshot();
    if (s.busy || !s.available || s.id != extension_access_id_ ||
        (s.mode != "ON_SPECIFIC_SITES" && !extension_sites_draft_))
        return;
    auto pattern = extension_site_pattern(extension_site_input_.value);
    if (pattern.empty())
        extension_site_error_ =
            app_.tr("Enter an http:// or https:// site address, without a path.");
    else if (std::find(s.hosts.begin(), s.hosts.end(), pattern) != s.hosts.end())
        extension_site_error_ = app_.tr("This site is already allowed.");
    else {
        extension_site_error_.clear();
        app_.edit_extension_access(extension_access_id_, "add", pattern);
    }
    app_.changed();
}
void BrowserUi::draw_extension_access(SkCanvas &c, Rect r) {
    const auto t = interface_theme(app_.profile);
    const auto &s = app_.state()->extension_access.snapshot();
    const bool ready = s.id == extension_access_id_ && s.available && !s.busy;
    const auto it = std::find_if(app_.extensions.items.begin(), app_.extensions.items.end(),
                                 [&](const auto &e) { return e.id == extension_access_id_; });
    label(c, medium_, it == app_.extensions.items.end() ? "" : it->name, r.x + 28, r.y + 79, 13,
          t.text, r.width - 56);
    auto button = [&](Rect b, const std::string &name, bool selected, bool enabled,
                      bool outlined = true) {
        if (enabled)
            focus_target(b);
        if (selected)
            fill(c, b, 6, SkColorSetA(t.accent, 28));
        if (outlined)
            border(c, b, 6, selected ? t.accent : SkColorSetA(t.muted, 110));
        if (selected)
            icon(c, "check", b.x + 10, b.y + 7, 18, t.accent);
        centered_label(c, medium_, app_.tr(name), b, 12, enabled ? t.text : t.muted, 8, 8);
    };
    const char *modes[] = {"ON_CLICK", "ON_SPECIFIC_SITES", "ON_ALL_SITES"};
    const char *names[] = {"Block automatic access", "On selected sites", "On all requested sites"};
    for (int i = 0; i < 3; ++i)
        button({r.x + 28, r.y + 94 + i * 38, r.width - 56, 32}, names[i],
               s.id == extension_access_id_ &&
                   (extension_sites_draft_ ? i == 1 : s.mode == modes[i]),
               ready);
    label(c, regular_, app_.tr("Reload open pages to finish applying the restriction."), r.x + 28,
          r.y + 221, 11, t.muted, r.width - 56);
    label(c, regular_, app_.tr("Site address (scheme and host; all ports)"), r.x + 28, r.y + 253,
          12, t.muted, r.width - 56);
    Rect input{r.x + 28, r.y + 265, r.width - 180, 36},
        add{r.x + r.width - 140, r.y + 265, 112, 36};
    const bool editing_allowed = ready && (s.mode == "ON_SPECIFIC_SITES" || extension_sites_draft_);
    if (editing_allowed)
        focus_target(input);
    fill(c, input, 7, t.background);
    border(c, input, 7, extension_site_focus_ ? t.accent : t.muted);
    draw_editor(c, extension_site_input_, {input.x + 10, input.y + 2, input.width - 20, 32}, 13,
                extension_site_focus_);
    button(add, "Add site", false, editing_allowed);
    std::string message =
        !extension_site_error_.empty() ? extension_site_error_
        : s.busy                       ? app_.tr("Reading site access...")
        : s.id != extension_access_id_ ? app_.tr("Refresh to read this extension's access.")
        : !s.error.empty()             ? app_.tr(s.error)
        : extension_sites_draft_ && s.hosts.empty()
            ? app_.tr("Add a site to allow the extension there.")
        : s.mode == "ON_CLICK" ? app_.tr("Website access is withheld. Temporary activeTab actions "
                                         "are unavailable in Aura.")
        : s.mode == "ON_ALL_SITES"
            ? app_.tr("Access stays within the extension's requested websites.")
            : app_.tr("Only allowed sites are listed below.");
    label(c, regular_, message, r.x + 28, r.y + 324, 11, t.muted, r.width - 56);
    const int cap = std::max(1, (r.height - 420) / 36);
    extension_site_scroll_ =
        std::clamp(extension_site_scroll_, 0, std::max(0, int(s.hosts.size()) - cap));
    if (s.id == extension_access_id_ && s.hosts.empty() && !s.busy)
        label(c, regular_, app_.tr("No allowed sites."), r.x + 28, r.y + 362, 13, t.muted,
              r.width - 56);
    if (s.id == extension_access_id_)
        for (int i = extension_site_scroll_;
             i < int(s.hosts.size()) && i < extension_site_scroll_ + cap; ++i) {
            const int y = r.y + 342 + (i - extension_site_scroll_) * 36;
            label(c, regular_, s.hosts[i], r.x + 28, y + 23, 12, t.text, r.width - 176);
            button({r.x + r.width - 124, y, 96, 30}, "Remove", false, editing_allowed, false);
        }
    button({r.x + 28, r.y + r.height - 58, 100, 32}, "Back", false, true);
    button({r.x + 140, r.y + r.height - 58, 116, 32}, "Refresh", false, !s.busy);
    button({r.x + r.width - 248, r.y + r.height - 58, 220, 32}, "Reload current page", false,
           app_.current() && app_.current()->browser);
}
void BrowserUi::extension_access_pointer(Rect r, int x, int y) {
    const auto &s = app_.state()->extension_access.snapshot();
    extension_site_focus_ = false;
    if (inside({r.x + 28, r.y + r.height - 58, 100, 32}, x, y)) {
        panel(Extensions);
        return;
    }
    if (inside({r.x + 140, r.y + r.height - 58, 116, 32}, x, y)) {
        extension_sites_draft_ = false;
        app_.edit_extension_access(extension_access_id_);
        return;
    }
    if (inside({r.x + r.width - 248, r.y + r.height - 58, 220, 32}, x, y)) {
        panel(None);
        if (auto t = app_.current(); t && t->browser)
            t->browser->Reload();
        return;
    }
    if (s.busy || !s.available || s.id != extension_access_id_)
        return;
    const char *modes[] = {"ON_CLICK", "ON_SPECIFIC_SITES", "ON_ALL_SITES"};
    for (int i = 0; i < 3; ++i)
        if (inside({r.x + 28, r.y + 94 + i * 38, r.width - 56, 32}, x, y)) {
            extension_sites_draft_ = i == 1;
            app_.edit_extension_access(extension_access_id_, "mode", modes[i]);
            extension_site_error_.clear();
            return;
        }
    if (s.mode != "ON_SPECIFIC_SITES" && !extension_sites_draft_)
        return;
    if (inside({r.x + 28, r.y + 265, r.width - 180, 36}, x, y)) {
        extension_site_focus_ = true;
        extension_site_input_.select_all();
        return;
    }
    if (inside({r.x + r.width - 140, r.y + 265, 112, 36}, x, y)) {
        add_extension_site();
        return;
    }
    const int cap = std::max(1, (r.height - 420) / 36);
    for (int i = extension_site_scroll_;
         i < int(s.hosts.size()) && i < extension_site_scroll_ + cap; ++i)
        if (inside({r.x + r.width - 124, r.y + 342 + (i - extension_site_scroll_) * 36, 96, 30}, x,
                   y)) {
            app_.edit_extension_access(extension_access_id_, "remove", s.hosts[i]);
            return;
        }
}
std::string BrowserUi::extension_access_report() const {
    if (panel_ != ExtensionAccess)
        return "null";
    const auto r = panel_bounds();
    const auto &s = app_.state()->extension_access.snapshot();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    std::string out = "{\"id\":" + json_quote(extension_access_id_) +
                      ",\"query\":" + json_quote(extension_site_input_.value) +
                      ",\"error\":" + json_quote(extension_site_error_) +
                      ",\"input\":" + rect({r.x + 28, r.y + 265, r.width - 180, 36}) +
                      ",\"add\":" + rect({r.x + r.width - 140, r.y + 265, 112, 36}) +
                      ",\"back\":" + rect({r.x + 28, r.y + r.height - 58, 100, 32}) +
                      ",\"refresh\":" + rect({r.x + 140, r.y + r.height - 58, 116, 32}) +
                      ",\"modes\":[";
    for (int i = 0; i < 3; ++i) {
        if (i)
            out += ',';
        out += rect({r.x + 28, r.y + 94 + i * 38, r.width - 56, 32});
    }
    out += "],\"items\":[";
    const int cap = std::max(1, (r.height - 420) / 36);
    for (int i = extension_site_scroll_;
         i < int(s.hosts.size()) && i < extension_site_scroll_ + cap; ++i) {
        if (i != extension_site_scroll_)
            out += ',';
        out += "{\"host\":" + json_quote(s.hosts[i]) + ",\"remove\":" +
               rect({r.x + r.width - 124, r.y + 342 + (i - extension_site_scroll_) * 36, 96, 30}) +
               "}";
    }
    return out + "]}";
}
} // namespace aura

namespace aura {
void BrowserUi::open_diagnostics() {
    app_.close_extension_popup();
    panel(Diagnostics);
    app_.close_menu();
    if (auto t = app_.current(); t && t->browser)
        t->browser->GetHost()->SetFocus(false);
    app_.changed();
}
void BrowserUi::draw_diagnostics(SkCanvas &c, Rect r) {
    const auto t = interface_theme(app_.profile);
    const bool safe = app_.safe_mode();
    label(c, medium_, app_.tr(safe ? "Safe mode is active" : "Normal mode is active"), r.x + 28,
          r.y + 96, 18, t.text, r.width - 56);
    label(c, regular_, app_.tr("Temporary mode. Your saved preferences stay unchanged."), r.x + 28,
          r.y + 130, 12, t.muted, r.width - 56);
    const char *labels[] = {"Extensions and page injections", "Network ad filtering",
                            "Hardware acceleration"};
    for (int i = 0; i < 3; ++i) {
        label(c, regular_, app_.tr(labels[i]), r.x + 28, r.y + 180 + i * 32, 13, t.text,
              r.width - 220);
        label(c, medium_, app_.tr(safe ? "Paused" : "Normal settings"), r.x + r.width - 184,
              r.y + 180 + i * 32, 12, t.muted, 156);
    }
    label(c, regular_, app_.tr("Report: technical counts and event codes."), r.x + 28, r.y + 282,
          12, t.muted, r.width - 56);
    label(c, regular_, app_.tr("No addresses, page text, logins or old logs."), r.x + 28, r.y + 309,
          12, t.muted, r.width - 56);
    auto button = [&](Rect b, const std::string &text, bool primary) {
        focus_target(b);
        if (primary)
            fill(c, b, 8, t.accent);
        else
            border(c, b, 8, t.muted);
        centered_label(c, medium_, app_.tr(text), b, 12, primary ? t.background : t.text, 12, 12);
    };
    button({r.x + 28, r.y + r.height - 130, r.width - 56, 36},
           safe ? "Restart in normal mode" : "Restart in safe mode", true);
    button({r.x + 28, r.y + r.height - 84, r.width - 56, 36}, "Save diagnostics report", false);
    label(c, regular_, app_.tr("Nothing is uploaded automatically."), r.x + 28, r.y + r.height - 20,
          11, t.muted, r.width - 56);
}
void BrowserUi::diagnostics_pointer(Rect r, int x, int y) {
    if (inside({r.x + 28, r.y + r.height - 130, r.width - 56, 36}, x, y))
        app_.restart_in_mode(!app_.safe_mode());
    else if (inside({r.x + 28, r.y + r.height - 84, r.width - 56, 36}, x, y))
        save_export(app_.diagnostics_report(), "aura-diagnostics.json", "*.json",
                    "Save diagnostics report");
}
std::string BrowserUi::diagnostics_report() const {
    if (panel_ != Diagnostics)
        return "null";
    const auto r = panel_bounds();
    auto rect = [](Rect b) {
        return "[" + std::to_string(b.x) + "," + std::to_string(b.y) + "," +
               std::to_string(b.width) + "," + std::to_string(b.height) + "]";
    };
    return "{\"picker_pid\":" + std::to_string(picker_pid_) +
           ",\"last_export\":" + json_quote(last_export_) +
           ",\"error\":" + json_quote(save_error_) +
           ",\"restart\":" + rect({r.x + 28, r.y + r.height - 130, r.width - 56, 36}) +
           ",\"save\":" + rect({r.x + 28, r.y + r.height - 84, r.width - 56, 36}) + "}";
}
} // namespace aura

namespace aura {
namespace {
CefRefPtr<CefBrowser> composition_target(BrowserController &app) {
    if (auto p = app.extension_popup; p && p->browser)
        return p->browser;
    if (auto t = app.current(); t && t->browser && !t->ntp)
        return t->browser;
    return nullptr;
}
} // namespace
void BrowserUi::composition(const std::string &value, int start, int length) {
    if (value.size() > 65536)
        return;
    auto target = composition_target(app_);
    if (preedit_browser_ && (!target || target->GetIdentifier() != preedit_browser_ || editing ||
                             find_open || panel_ != None || !app_.dialogs.empty())) {
        for (const auto &t : app_.tabs)
            if (t->browser && t->browser->GetIdentifier() == preedit_browser_)
                t->browser->GetHost()->ImeCancelComposition();
        preedit_browser_ = 0;
    }
    if (target && !editing && !find_open && panel_ == None && app_.dialogs.empty()) {
        if (value.empty()) {
            if (preedit_browser_ == target->GetIdentifier())
                target->GetHost()->ImeCancelComposition();
            preedit_browser_ = 0;
        } else {
            auto utf16 = CefString(value).ToString16();
            auto offset = [&](int count) {
                size_t byte = 0;
                for (int n = 0; byte < value.size() && n < count; ++n) {
                    ++byte;
                    while (byte < value.size() &&
                           (static_cast<unsigned char>(value[byte]) & 0xc0) == 0x80)
                        ++byte;
                }
                return int(CefString(value.substr(0, byte)).ToString16().size());
            };
            const int a = offset(std::max(0, start)),
                      b = offset(std::max(0, start) + std::max(0, length));
            CefCompositionUnderline u;
            u.range = CefRange(0, int(utf16.size()));
            u.color = SK_ColorBLACK;
            u.background_color = SK_ColorTRANSPARENT;
            u.thick = false;
            target->GetHost()->ImeSetComposition(value, {u}, CefRange(-1, -1), CefRange(a, b));
            preedit_browser_ = target->GetIdentifier();
        }
        preedit_.clear();
    } else
        preedit_ = value;
    app_.changed();
}
void BrowserUi::commit_composition(const std::string &value) {
    preedit_.clear();
    auto target = composition_target(app_);
    if (preedit_browser_ && target && target->GetIdentifier() == preedit_browser_)
        target->GetHost()->ImeCommitText(value, CefRange(-1, -1), 0);
    else
        text(value);
    preedit_browser_ = 0;
    app_.changed();
}
} // namespace aura
