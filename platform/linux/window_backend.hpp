#pragma once
#include "platform/browser_window.hpp"
namespace aura {
void select_linux_window_system(const std::string &value);
std::string linux_window_system();
class LinuxWindow {
  public:
    virtual ~LinuxWindow() = default;
    virtual CefWindowHandle handle() const = 0;
    virtual std::uintptr_t native_id() const = 0;
    virtual void cancel_composition() {}
    virtual bool begin_tab_drag() { return false; }
    virtual void show() = 0;
    virtual void clear_callbacks() = 0;
    virtual std::optional<std::pair<int, int>> point_from(const LinuxWindow &source, int x,
                                                          int y) const = 0;
    virtual std::pair<int, int> screen_point(int x, int y) const = 0;
    virtual void activate() = 0;
    virtual void hide() = 0;
    virtual void dispatch() = 0;
    virtual void paint() = 0;
    virtual void wait(int milliseconds) = 0;
    virtual void invalidate() = 0;
    virtual void invalidate(Rect damage) = 0;
    virtual void wake() = 0;
    virtual int refresh_rate() = 0;
    virtual std::string presentation_stats() const = 0;
    virtual void interface_zoom(double factor) = 0;
    virtual void title(const std::string &text) = 0;
    virtual void caret(int x, int y) = 0;
    virtual void cursor(int kind) = 0;
    virtual void copy(const std::string &text) = 0;
    virtual void paste(std::function<void(std::string)> callback) = 0;
    virtual std::string compositor() const = 0;
    virtual std::string renderer() const = 0;
    virtual std::uint64_t frames() const = 0;
};
} // namespace aura
