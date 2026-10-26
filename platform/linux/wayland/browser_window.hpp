#pragma once
#include "platform/linux/window_backend.hpp"
namespace aura {
class WaylandWindow final : public LinuxWindow {
  public:
    WaylandWindow(int width, int height, double scale, WindowCallbacks callbacks,
                  bool accelerated = false, double interface_zoom = 1,
                  bool native_clipboard = true);
    ~WaylandWindow();
    CefWindowHandle handle() const;
    std::uintptr_t native_id() const;
    void show();
    void clear_callbacks();
    std::optional<std::pair<int, int>> point_from(const LinuxWindow &source, int x, int y) const;
    std::pair<int, int> screen_point(int x, int y) const;
    void activate();
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
