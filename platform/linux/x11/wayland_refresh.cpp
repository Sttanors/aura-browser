#include "platform/linux/x11/wayland_refresh.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <poll.h>
#include <string_view>
#include <vector>
#include <wayland-client.h>
namespace aura {
struct WaylandRefresh::Impl {
    struct Output {
        wl_output *proxy = nullptr;
        std::uint32_t id = 0;
        int x = 0, y = 0, width = 0, height = 0, scale = 1, transform = 0, refresh = 0;
        ~Output() {
            if (proxy)
                wl_output_destroy(proxy);
        }
    };
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    std::vector<std::unique_ptr<Output>> outputs;
    static void geometry(void *data, wl_output *, int x, int y, int, int, int, const char *,
                         const char *, int transform) {
        auto &o = *static_cast<Output *>(data);
        o.x = x;
        o.y = y;
        o.transform = transform;
    }
    static void mode(void *data, wl_output *, std::uint32_t flags, int width, int height,
                     int refresh) {
        if (!(flags & WL_OUTPUT_MODE_CURRENT))
            return;
        auto &o = *static_cast<Output *>(data);
        o.width = width;
        o.height = height;
        o.refresh = refresh;
    }
    static void done(void *, wl_output *) {}
    static void scale(void *data, wl_output *, int scale) {
        static_cast<Output *>(data)->scale = std::max(1, scale);
    }
    static void name(void *, wl_output *, const char *) {}
    static void description(void *, wl_output *, const char *) {}
    static void global(void *data, wl_registry *registry, std::uint32_t id, const char *interface,
                       std::uint32_t version) {
        if (std::string_view(interface) != "wl_output")
            return;
        auto &self = *static_cast<Impl *>(data);
        auto output = std::make_unique<Output>();
        output->id = id;
        output->proxy = static_cast<wl_output *>(
            wl_registry_bind(registry, id, &wl_output_interface, std::min(version, 4u)));
        static const wl_output_listener listener = {geometry, mode, done, scale, name, description};
        wl_output_add_listener(output->proxy, &listener, output.get());
        self.outputs.push_back(std::move(output));
    }
    static void remove(void *data, wl_registry *, std::uint32_t id) {
        std::erase_if(static_cast<Impl *>(data)->outputs,
                      [id](const auto &o) { return o->id == id; });
    }
    Impl() {
        if (!std::getenv("WAYLAND_DISPLAY"))
            return;
        display = wl_display_connect(nullptr);
        if (!display)
            return;
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener = {global, remove};
        wl_registry_add_listener(registry, &listener, this);
        // Initial discovery only. During rendering, polling below never waits.
        if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0)
            outputs.clear();
    }
    ~Impl() {
        outputs.clear();
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
    }
    void update() {
        if (!display || wl_display_get_error(display))
            return;
        while (wl_display_prepare_read(display) != 0)
            if (wl_display_dispatch_pending(display) < 0)
                return;
        wl_display_flush(display);
        pollfd fd{wl_display_get_fd(display), POLLIN, 0};
        if (poll(&fd, 1, 0) > 0 && (fd.revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
    }
};
WaylandRefresh::WaylandRefresh() : impl_(std::make_unique<Impl>()) {}
WaylandRefresh::~WaylandRefresh() = default;
int WaylandRefresh::rate(Rect window) {
    auto &p = *impl_;
    p.update();
    std::int64_t best = 0;
    int rate = 0;
    for (const auto &o : p.outputs) {
        int width = o->width / o->scale, height = o->height / o->scale;
        if (o->transform == WL_OUTPUT_TRANSFORM_90 || o->transform == WL_OUTPUT_TRANSFORM_270 ||
            o->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
            o->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270)
            std::swap(width, height);
        const auto overlap = intersect(window, {o->x, o->y, width, height});
        const auto area = std::int64_t(overlap.width) * overlap.height;
        if (area > best && o->refresh >= 30000 && o->refresh <= 360000) {
            best = area;
            rate = (o->refresh + 500) / 1000;
        }
    }
    return rate;
}
} // namespace aura
