#pragma once
#include <functional>
#include <memory>
#include <string>
struct wl_display;
struct wl_surface;
namespace aura {
// Standard wl_data_device on the focused SDL client's connection. Bounded and nonblocking.
class WaylandClipboard {
  public:
    explicit WaylandClipboard(wl_display *);
    ~WaylandClipboard();
    void register_surface(wl_surface *, std::function<bool(int, double, double)>);
    void unregister_surface(wl_surface *);
    bool begin_drag(wl_surface *);
    void copy(const std::string &);
    void paste(std::function<void(std::string)>);
    void poll();
    bool pending() const;
    std::string report() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace aura
