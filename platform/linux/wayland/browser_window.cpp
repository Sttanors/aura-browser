#include "platform/linux/wayland/browser_window.hpp"
#include "common/branding_data.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "platform/linux/wayland/clipboard.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <wayland-client.h>
namespace aura {
namespace {
void check(bool ok) {
    if (!ok)
        throw std::runtime_error(std::string("Wayland: ") + SDL_GetError());
}
int modifiers(SDL_Keymod m, SDL_MouseButtonFlags buttons = 0) {
    int v = 0;
    if (m & SDL_KMOD_SHIFT)
        v |= EVENTFLAG_SHIFT_DOWN;
    if (m & SDL_KMOD_CTRL)
        v |= EVENTFLAG_CONTROL_DOWN;
    if (m & SDL_KMOD_ALT)
        v |= EVENTFLAG_ALT_DOWN;
    if (m & SDL_KMOD_GUI)
        v |= EVENTFLAG_COMMAND_DOWN;
    if (m & SDL_KMOD_MODE)
        v |= EVENTFLAG_ALTGR_DOWN;
    if (m & SDL_KMOD_CAPS)
        v |= EVENTFLAG_CAPS_LOCK_ON;
    if (buttons & SDL_BUTTON_LMASK)
        v |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (buttons & SDL_BUTTON_MMASK)
        v |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (buttons & SDL_BUTTON_RMASK)
        v |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return v;
}
int virtual_key(SDL_Scancode k) {
    if (k >= SDL_SCANCODE_A && k <= SDL_SCANCODE_Z)
        return 'A' + k - SDL_SCANCODE_A;
    if (k >= SDL_SCANCODE_1 && k <= SDL_SCANCODE_9)
        return '1' + k - SDL_SCANCODE_1;
    if (k >= SDL_SCANCODE_F1 && k <= SDL_SCANCODE_F12)
        return 0x70 + k - SDL_SCANCODE_F1;
    switch (k) {
    case SDL_SCANCODE_0:
        return '0';
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        return 13;
    case SDL_SCANCODE_BACKSPACE:
        return 8;
    case SDL_SCANCODE_TAB:
        return 9;
    case SDL_SCANCODE_ESCAPE:
        return 27;
    case SDL_SCANCODE_SPACE:
        return 32;
    case SDL_SCANCODE_PAGEUP:
        return 0x21;
    case SDL_SCANCODE_PAGEDOWN:
        return 0x22;
    case SDL_SCANCODE_END:
        return 0x23;
    case SDL_SCANCODE_HOME:
        return 0x24;
    case SDL_SCANCODE_LEFT:
        return 0x25;
    case SDL_SCANCODE_UP:
        return 0x26;
    case SDL_SCANCODE_RIGHT:
        return 0x27;
    case SDL_SCANCODE_DOWN:
        return 0x28;
    case SDL_SCANCODE_INSERT:
        return 0x2d;
    case SDL_SCANCODE_DELETE:
        return 0x2e;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        return 0x10;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        return 0x11;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:
        return 0x12;
    case SDL_SCANCODE_SEMICOLON:
        return 0xba;
    case SDL_SCANCODE_EQUALS:
        return 0xbb;
    case SDL_SCANCODE_COMMA:
        return 0xbc;
    case SDL_SCANCODE_MINUS:
        return 0xbd;
    case SDL_SCANCODE_PERIOD:
        return 0xbe;
    case SDL_SCANCODE_SLASH:
        return 0xbf;
    case SDL_SCANCODE_GRAVE:
        return 0xc0;
    case SDL_SCANCODE_LEFTBRACKET:
        return 0xdb;
    case SDL_SCANCODE_BACKSLASH:
        return 0xdc;
    case SDL_SCANCODE_RIGHTBRACKET:
        return 0xdd;
    case SDL_SCANCODE_APOSTROPHE:
        return 0xde;
    default:
        return 0;
    }
}
} // namespace
struct WaylandWindow::Impl {
    WindowCallbacks callbacks;
    std::shared_ptr<void> alive = std::make_shared<int>(0);
    SDL_Window *window = nullptr;
    SDL_Cursor *cursors[3]{};
    wl_display *display = nullptr;
    wl_surface *surface = nullptr;
    wl_shm *shm = nullptr;
    wl_registry *registry = nullptr;
    wl_callback *frame = nullptr;
    struct Buffer {
        wl_buffer *proxy = nullptr;
        void *data = MAP_FAILED;
        size_t size = 0;
        int w = 0, h = 0;
        bool busy = false;
    };
    std::array<Buffer, 3> buffers;
    static void released(void *data, wl_buffer *) { static_cast<Buffer *>(data)->busy = false; }
    static void frame_done(void *data, wl_callback *cb, uint32_t) {
        auto &p = *static_cast<Impl *>(data);
        wl_callback_destroy(cb);
        p.frame = nullptr;
    }
    static void global(void *data, wl_registry *r, uint32_t name, const char *interface, uint32_t) {
        auto &p = *static_cast<Impl *>(data);
        if (!strcmp(interface, "wl_shm"))
            p.shm = static_cast<wl_shm *>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    }
    static void removed(void *, wl_registry *, uint32_t) {}
    static void destroy(Buffer &b) {
        if (b.proxy)
            wl_buffer_destroy(b.proxy);
        if (b.data != MAP_FAILED)
            munmap(b.data, b.size);
        b = {};
    }
    Buffer *available_buffer() {
        for (auto &b : buffers)
            if (!b.busy) {
                if (b.w != pixel_w || b.h != pixel_h) {
                    destroy(b);
                    const size_t size = size_t(pixel_w) * pixel_h * 4;
                    if (!size || size > 512 * 1024 * 1024)
                        throw std::runtime_error("Wayland frame exceeds memory limit");
                    int fd = memfd_create("aura-wayland-frame", MFD_CLOEXEC);
                    if (fd < 0)
                        throw std::runtime_error("Cannot allocate Wayland frame");
                    if (ftruncate(fd, size)) {
                        close(fd);
                        throw std::runtime_error("Cannot size Wayland frame");
                    }
                    b.data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (b.data == MAP_FAILED) {
                        close(fd);
                        throw std::runtime_error("Cannot map Wayland frame");
                    }
                    b.size = size;
                    b.w = pixel_w;
                    b.h = pixel_h;
                    auto *pool = wl_shm_create_pool(shm, fd, int(size));
                    b.proxy = wl_shm_pool_create_buffer(pool, 0, pixel_w, pixel_h, pixel_w * 4,
                                                        WL_SHM_FORMAT_ARGB8888);
                    wl_shm_pool_destroy(pool);
                    close(fd);
                    static const wl_buffer_listener listener{released};
                    wl_buffer_add_listener(b.proxy, &listener, &b);
                    ++allocations;
                }
                return &b;
            }
        return nullptr;
    }
    double requested_scale = 1, base_scale = 1, zoom = 1, scale = 1;
    int width = 1360, height = 900, pixel_w = 0, pixel_h = 0, rate = 60;
    bool dirty = true, visible = false, suppress_text = false;
    uint64_t frame_count = 0, allocations = 0;
    double paint_p95 = 0;
    std::array<double, 240> costs{};
    size_t cost_count = 0, cost_index = 0;
    sk_sp<SkSurface> raster;
    SDL_MouseButtonFlags buttons = 0;
    static std::unordered_map<SDL_WindowID, Impl *> &windows() {
        static std::unordered_map<SDL_WindowID, Impl *> v;
        return v;
    }
    static std::unique_ptr<WaylandClipboard> &clipboard() {
        static std::unique_ptr<WaylandClipboard> c;
        return c;
    }
    static Uint32 wake_type() {
        static Uint32 type = SDL_RegisterEvents(1);
        return type;
    }
    Impl(WindowCallbacks cb) : callbacks(std::move(cb)) {}
    ~Impl() {
        if (frame)
            wl_callback_destroy(frame);
        for (auto &b : buffers)
            destroy(b);
        if (shm)
            wl_shm_destroy(shm);
        if (registry)
            wl_registry_destroy(registry);
        if (window) {
            windows().erase(SDL_GetWindowID(window));
            SDL_DestroyWindow(window);
        }
        for (auto c : cursors)
            if (c)
                SDL_DestroyCursor(c);
        if (windows().empty()) {
            clipboard().reset();
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }
    void resize() {
        int w = 0, h = 0;
        check(SDL_GetWindowSizeInPixels(window, &w, &h));
        if (w <= 0 || h <= 0)
            return;
        const auto dpi = SDL_GetWindowDisplayScale(window);
        base_scale = std::max(.5, double(dpi)) * requested_scale;
        scale = std::min({base_scale * zoom, w / 900.0, h / 620.0});
        pixel_w = w;
        pixel_h = h;
        width = std::max(1, int(std::lround(w / scale)));
        height = std::max(1, int(std::lround(h / scale)));
        const auto *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        rate = mode && mode->refresh_rate > 0
                   ? std::clamp(int(std::lround(mode->refresh_rate)), 30, 240)
                   : 60;
        dirty = true;
        if (callbacks.resize)
            callbacks.resize(width, height, scale / base_scale, base_scale);
    }
    CefMouseEvent point(float x, float y) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        CefMouseEvent p;
        p.x = int(std::lround(x * pixel_w / std::max(1, w) / scale));
        p.y = int(std::lround(y * pixel_h / std::max(1, h) / scale));
        p.modifiers = modifiers(SDL_GetModState(), buttons);
        return p;
    }
    static SDL_HitTestResult hit(SDL_Window *, const SDL_Point *point, void *data) {
        auto &p = *static_cast<Impl *>(data);
        auto v = p.point(point->x, point->y);
        constexpr int b = 6;
        bool l = v.x < b, r = v.x >= p.width - b, t = v.y < b, bt = v.y >= p.height - b;
        if (t && l)
            return SDL_HITTEST_RESIZE_TOPLEFT;
        if (t && r)
            return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bt && l)
            return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bt && r)
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (t)
            return SDL_HITTEST_RESIZE_TOP;
        if (bt)
            return SDL_HITTEST_RESIZE_BOTTOM;
        if (l)
            return SDL_HITTEST_RESIZE_LEFT;
        if (r)
            return SDL_HITTEST_RESIZE_RIGHT;
        return p.callbacks.caption && p.callbacks.caption(v.x, v.y) && v.x < p.width - 144
                   ? SDL_HITTEST_DRAGGABLE
                   : SDL_HITTEST_NORMAL;
    }
    void event(const SDL_Event &e) {
        switch (e.type) {
        case SDL_EVENT_WINDOW_EXPOSED:
            dirty = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            resize();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (callbacks.close)
                callbacks.close();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            SDL_StartTextInput(window);
            if (callbacks.focus)
                callbacks.focus(true);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            buttons = 0;
            if (callbacks.focus)
                callbacks.focus(false);
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (callbacks.pointer)
                callbacks.pointer({PointerInput::Leave, point(-1, -1)});
            break;
        case SDL_EVENT_MOUSE_MOTION:
            buttons = e.motion.state;
            if (callbacks.pointer)
                callbacks.pointer({PointerInput::Move, point(e.motion.x, e.motion.y)});
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const auto mask = SDL_BUTTON_MASK(e.button.button);
            if (down)
                buttons |= mask;
            else
                buttons &= ~mask;
            auto p = point(e.button.x, e.button.y);
            if (down && e.button.button == SDL_BUTTON_LEFT && p.y < 30 && p.x >= width - 144) {
                if (p.x >= width - 48) {
                    if (callbacks.close)
                        callbacks.close();
                } else if (p.x >= width - 96) {
                    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)
                        SDL_RestoreWindow(window);
                    else
                        SDL_MaximizeWindow(window);
                } else
                    SDL_MinimizeWindow(window);
                break;
            }
            if (e.button.button == SDL_BUTTON_X1 || e.button.button == SDL_BUTTON_X2) {
                if (down && callbacks.key) {
                    CefKeyEvent k;
                    k.type = KEYEVENT_RAWKEYDOWN;
                    k.windows_key_code = e.button.button == SDL_BUTTON_X1 ? 0x25 : 0x27;
                    k.modifiers = EVENTFLAG_ALT_DOWN;
                    callbacks.key(k);
                }
                break;
            }
            if (callbacks.pointer)
                callbacks.pointer({down ? PointerInput::Down : PointerInput::Up, p,
                                   e.button.button == SDL_BUTTON_LEFT     ? 1
                                   : e.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                          : 3,
                                   0, 0, std::min(2, int(e.button.clicks))});
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            const int sign = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
            if (callbacks.pointer)
                callbacks.pointer({PointerInput::Wheel, point(e.wheel.mouse_x, e.wheel.mouse_y), 1,
                                   int(std::lround(e.wheel.x * 120 * sign)),
                                   int(std::lround(e.wheel.y * 120 * sign))});
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            CefKeyEvent k;
            k.type = e.key.down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
            k.windows_key_code = virtual_key(e.key.scancode);
            k.native_key_code = e.key.raw + 8;
            k.modifiers = modifiers(e.key.mod);
            const bool consumed = callbacks.key && callbacks.key(k);
            if (e.key.down)
                suppress_text = consumed;
            break;
        }
        case SDL_EVENT_TEXT_EDITING:
            if (callbacks.preedit)
                callbacks.preedit(e.edit.text, e.edit.start, e.edit.length);
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (!suppress_text && callbacks.text)
                callbacks.text(e.text.text);
            suppress_text = false;
            break;
        }
    }
    static void deliver(const SDL_Event &e) {
        auto *window = SDL_GetWindowFromEvent(&e);
        if (!window)
            return;
        const auto id = SDL_GetWindowID(window);
        auto it = windows().find(id);
        if (it != windows().end())
            it->second->event(e);
    }
};
WaylandWindow::WaylandWindow(int w, int h, double requested, WindowCallbacks hooks, bool,
                             double zoom, bool native_clipboard)
    : impl_(std::make_unique<Impl>(std::move(hooks))) {
    auto &p = *impl_;
    p.requested_scale = requested;
    p.zoom = zoom;
    if (Impl::windows().empty()) {
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "wayland", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0", SDL_HINT_OVERRIDE);
        SDL_SetHint(SDL_HINT_APP_ID, "aura-browser");
        SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");
        SDL_SetHintWithPriority("AURA_WAYLAND_EXTERNAL_CLIPBOARD", native_clipboard ? "1" : "0",
                                SDL_HINT_OVERRIDE);
        check(SDL_InitSubSystem(SDL_INIT_VIDEO));
        (void)Impl::wake_type();
        check(std::string(SDL_GetCurrentVideoDriver()) == "wayland");
    }
    SDL_Rect available{};
    int initial_w = int(std::lround(w * requested)), initial_h = int(std::lround(h * requested));
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &available)) {
        initial_w = std::min(initial_w, std::max(675, available.w));
        initial_h = std::min(initial_h, std::max(465, available.h));
    }
    p.window = SDL_CreateWindow("Aura", initial_w, initial_h,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS |
                                    SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    check(p.window != nullptr);
    Impl::windows()[SDL_GetWindowID(p.window)] = &p;
    SDL_SetWindowMinimumSize(p.window, 675, 465);
    check(SDL_SetWindowHitTest(p.window, Impl::hit, &p));
    auto *icon = SDL_CreateSurfaceFrom(64, 64, SDL_PIXELFORMAT_ARGB8888,
                                       const_cast<uint32_t *>(brand::argb), 64 * 4);
    if (icon) {
        SDL_SetWindowIcon(p.window, icon);
        SDL_DestroySurface(icon);
    }
    auto props = SDL_GetWindowProperties(p.window);
    p.display = static_cast<wl_display *>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    p.surface = static_cast<wl_surface *>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    check(p.display && p.surface);
    p.registry = wl_display_get_registry(p.display);
    static const wl_registry_listener listener{Impl::global, Impl::removed};
    wl_registry_add_listener(p.registry, &listener, &p);
    check(wl_display_roundtrip(p.display) >= 0);
    check(p.shm != nullptr);
    if (native_clipboard && !Impl::clipboard())
        Impl::clipboard() = std::make_unique<WaylandClipboard>(p.display);
    p.resize();
}
WaylandWindow::~WaylandWindow() = default;
CefWindowHandle WaylandWindow::handle() const { return 0; }
std::uintptr_t WaylandWindow::native_id() const { return SDL_GetWindowID(impl_->window); }
void WaylandWindow::show() {
    check(SDL_ShowWindow(impl_->window));
    impl_->visible = true;
    impl_->dirty = true;
    SDL_StartTextInput(impl_->window);
}
void WaylandWindow::hide() {
    if (impl_->frame) {
        wl_callback_destroy(impl_->frame);
        impl_->frame = nullptr;
    }
    SDL_HideWindow(impl_->window);
    impl_->visible = false;
}
void WaylandWindow::activate() {
    SDL_ShowWindow(impl_->window);
    SDL_RaiseWindow(impl_->window);
    impl_->visible = true;
    impl_->dirty = true;
}
void WaylandWindow::clear_callbacks() {
    impl_->alive.reset();
    impl_->callbacks = {};
}
void WaylandWindow::dispatch() {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        Impl::deliver(e);
    if (Impl::clipboard())
        Impl::clipboard()->poll();
}
void WaylandWindow::wait(int ms) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(
            &e, std::clamp(ms, 0, Impl::clipboard() && Impl::clipboard()->pending() ? 1 : 16)))
        Impl::deliver(e);
}
void WaylandWindow::wake() {
    SDL_Event e{};
    e.type = Impl::wake_type();
    SDL_PushEvent(&e);
}
void WaylandWindow::invalidate() { impl_->dirty = true; }
void WaylandWindow::invalidate(Rect) { invalidate(); }
void WaylandWindow::paint() {
    auto &p = *impl_;
    if (!p.visible || !p.dirty)
        return;
    const auto start = SDL_GetTicksNS();
    if (p.frame)
        return;
    auto *b = p.available_buffer();
    if (!b)
        return;
    const auto info = SkImageInfo::MakeN32Premul(p.pixel_w, p.pixel_h);
    auto target = SkSurfaces::WrapPixels(info, b->data, p.pixel_w * 4);
    if (!target)
        throw std::runtime_error("Cannot wrap Wayland pixels");
    auto &c = *target->getCanvas();
    c.save();
    c.scale(p.scale, p.scale);
    if (p.callbacks.draw)
        p.callbacks.draw(c);
    c.restore();
    p.frame = wl_surface_frame(p.surface);
    static const wl_callback_listener listener{Impl::frame_done};
    wl_callback_add_listener(p.frame, &listener, &p);
    b->busy = true;
    wl_surface_attach(p.surface, b->proxy, 0, 0);
    wl_surface_damage_buffer(p.surface, 0, 0, p.pixel_w, p.pixel_h);
    wl_surface_commit(p.surface);
    wl_display_flush(p.display);
    p.dirty = false;
    ++p.frame_count;
    p.costs[p.cost_index++ % p.costs.size()] = (SDL_GetTicksNS() - start) / 1e6;
    p.cost_count = std::min(p.cost_count + 1, p.costs.size());
}
int WaylandWindow::refresh_rate() { return impl_->rate; }
std::string WaylandWindow::presentation_stats() const {
    auto costs = impl_->costs;
    std::sort(costs.begin(), costs.begin() + impl_->cost_count);
    std::ostringstream o;
    o << "{\"paint_ms_p95\":" << (impl_->cost_count ? costs[(impl_->cost_count - 1) * 95 / 100] : 0)
      << ",\"input_focus\":"
      << ((SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_INPUT_FOCUS) ? "true" : "false")
      << ",\"raster_allocations\":" << impl_->allocations
      << ",\"display\":{\"native_wayland\":true,\"refresh_hz\":" << impl_->rate
      << "},\"clipboard\":" << Impl::clipboard()->report() << "}";
    return o.str();
}
void WaylandWindow::interface_zoom(double zoom) {
    if (impl_->zoom != zoom) {
        impl_->zoom = zoom;
        impl_->resize();
    }
}
void WaylandWindow::title(const std::string &t) { SDL_SetWindowTitle(impl_->window, t.c_str()); }
void WaylandWindow::caret(int x, int y) {
    auto &p = *impl_;
    int w = 0, h = 0;
    SDL_GetWindowSize(p.window, &w, &h);
    SDL_Rect r{int(x * p.scale * w / std::max(1, p.pixel_w)),
               int(y * p.scale * h / std::max(1, p.pixel_h)), 1, 20};
    SDL_SetTextInputArea(p.window, &r, 0);
}
void WaylandWindow::cursor(int k) {
    k = std::clamp(k, 0, 2);
    auto &p = *impl_;
    if (!p.cursors[k])
        p.cursors[k] = SDL_CreateSystemCursor(k == 1   ? SDL_SYSTEM_CURSOR_TEXT
                                              : k == 2 ? SDL_SYSTEM_CURSOR_POINTER
                                                       : SDL_SYSTEM_CURSOR_DEFAULT);
    if (p.cursors[k])
        SDL_SetCursor(p.cursors[k]);
}
void WaylandWindow::copy(const std::string &t) { Impl::clipboard()->copy(t); }
void WaylandWindow::paste(std::function<void(std::string)> cb) {
    std::weak_ptr<void> weak = impl_->alive;
    Impl::clipboard()->paste([weak, cb = std::move(cb)](std::string text) {
        if (!weak.expired() && !text.empty() && cb)
            cb(std::move(text));
    });
}
std::string WaylandWindow::compositor() const { return "Skia raster / native Wayland wl_shm"; }
std::string WaylandWindow::renderer() const { return "CPU / Wayland shared memory"; }
std::uint64_t WaylandWindow::frames() const { return impl_->frame_count; }
std::optional<std::pair<int, int>> WaylandWindow::point_from(const LinuxWindow &, int, int) const {
    return {};
}
std::pair<int, int> WaylandWindow::screen_point(int x, int y) const { return {x, y}; }
} // namespace aura
