#pragma once
#include "common/geometry.hpp"
#include "include/cef_browser.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
class SkCanvas;
namespace aura {
struct PointerInput {
    enum Kind { Move, Leave, Down, Up, Wheel } kind;
    CefMouseEvent point;
    int button = 1, dx = 0, dy = 0, clicks = 1;
};
struct WindowCallbacks {
    std::function<void(SkCanvas &)> draw;
    std::function<bool(SkCanvas &)> draw_content;
    std::function<void(int, int, double, double)> resize;
    std::function<void(const PointerInput &)> pointer;
    std::function<bool(const CefKeyEvent &)> key;
    std::function<void(const std::string &)> text;
    std::function<void(bool)> focus;
    std::function<void()> close;
};
class BrowserWindow {
  public:
    BrowserWindow(int width, int height, double scale, WindowCallbacks callbacks,
                  bool accelerated = false, double interface_zoom = 1);
    ~BrowserWindow();
    CefWindowHandle handle() const;
    std::uintptr_t native_id() const;
    void show();
    void hide();
    void dispatch();
    void paint();
    void wait(int milliseconds);
    void invalidate();
    void invalidate(Rect damage);
    void wake();
    int refresh_rate();
    std::string presentation_stats() const;
    void interface_zoom(double factor);
    void title(const std::string &text);
    void caret(int x, int y);
    void cursor(int kind);
    void copy(const std::string &text);
    void paste(std::function<void(std::string)> callback);
    std::string compositor() const;
    std::string renderer() const;
    std::uint64_t frames() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace aura
