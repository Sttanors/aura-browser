#include "platform/linux/x11/browser_window.hpp"
#include "platform/linux/wayland/browser_window.hpp"
#include "platform/linux/window_backend.hpp"
#include <cstdlib>
#include <stdexcept>
namespace aura {
namespace {
std::string selected = "x11";
}
void select_linux_window_system(const std::string &value) {
    if (value != "auto" && value != "x11" && value != "wayland")
        throw std::runtime_error("Window system must be auto, x11 or wayland");
    selected = value;
    if (selected == "auto")
        selected =
            (std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY")) ? "wayland" : "x11";
}
std::string linux_window_system() { return selected; }
struct BrowserWindow::Impl {
    std::unique_ptr<LinuxWindow> window;
};
BrowserWindow::BrowserWindow(int w, int h, double scale, WindowCallbacks hooks, bool gpu,
                             double zoom)
    : impl_(std::make_unique<Impl>()) {
    if (selected == "wayland")
        impl_->window = std::make_unique<WaylandWindow>(w, h, scale, std::move(hooks), gpu, zoom);
    else
        impl_->window = std::make_unique<X11Window>(w, h, scale, std::move(hooks), gpu, zoom);
}
BrowserWindow::~BrowserWindow() = default;
void BrowserWindow::cancel_composition() { impl_->window->cancel_composition(); }
bool BrowserWindow::begin_tab_drag() { return impl_->window->begin_tab_drag(); }
CefWindowHandle BrowserWindow::handle() const { return impl_->window->handle(); }
std::uintptr_t BrowserWindow::native_id() const { return impl_->window->native_id(); }
std::optional<std::pair<int, int>> BrowserWindow::point_from(const BrowserWindow &s, int x,
                                                             int y) const {
    return impl_->window->point_from(*s.impl_->window, x, y);
}
std::pair<int, int> BrowserWindow::screen_point(int x, int y) const {
    return impl_->window->screen_point(x, y);
}
std::string BrowserWindow::backend() const {
    return dynamic_cast<WaylandWindow *>(impl_->window.get()) ? "wayland" : "x11";
}
#define AURA_FORWARD_VOID(name)                                                                    \
    void BrowserWindow::name() { impl_->window->name(); }
AURA_FORWARD_VOID(show)
AURA_FORWARD_VOID(clear_callbacks)
AURA_FORWARD_VOID(activate)
AURA_FORWARD_VOID(hide)
AURA_FORWARD_VOID(dispatch)
AURA_FORWARD_VOID(paint)
AURA_FORWARD_VOID(invalidate)
AURA_FORWARD_VOID(wake)
#undef AURA_FORWARD_VOID
void BrowserWindow::wait(int ms) { impl_->window->wait(ms); }
void BrowserWindow::invalidate(Rect r) { impl_->window->invalidate(r); }
int BrowserWindow::refresh_rate() { return impl_->window->refresh_rate(); }
std::string BrowserWindow::presentation_stats() const {
    return impl_->window->presentation_stats();
}
void BrowserWindow::interface_zoom(double v) { impl_->window->interface_zoom(v); }
void BrowserWindow::title(const std::string &v) { impl_->window->title(v); }
void BrowserWindow::caret(int x, int y) { impl_->window->caret(x, y); }
void BrowserWindow::cursor(int k) { impl_->window->cursor(k); }
void BrowserWindow::copy(const std::string &v) { impl_->window->copy(v); }
void BrowserWindow::paste(std::function<void(std::string)> cb) {
    impl_->window->paste(std::move(cb));
}
std::string BrowserWindow::compositor() const { return impl_->window->compositor(); }
std::string BrowserWindow::renderer() const { return impl_->window->renderer(); }
std::uint64_t BrowserWindow::frames() const { return impl_->window->frames(); }
} // namespace aura
