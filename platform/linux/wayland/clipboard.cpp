#include "platform/linux/wayland/clipboard.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <wayland-client.h>
namespace aura {
namespace {
constexpr size_t limit = 16 * 1024 * 1024;
auto now() { return std::chrono::steady_clock::now(); }
ssize_t pipe_write(int fd, const void *data, size_t size) {
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    sigpending(&pending);
    const bool old = sigismember(&pending, SIGPIPE);
    const auto n = write(fd, data, size);
    const int error = errno;
    if (n < 0 && error == EPIPE && !old) {
        timespec zero{};
        sigtimedwait(&blocked, nullptr, &zero);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = error;
    return n;
}
} // namespace
struct WaylandClipboard::Impl {
    wl_display *display;
    wl_registry *registry = nullptr;
    wl_data_device_manager *manager = nullptr;
    wl_data_device *device = nullptr;
    wl_seat *seat = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_pointer *pointer = nullptr;
    uint32_t serial = 0;
    bool focused = false;
    struct Offer {
        wl_data_offer *proxy;
        std::string mime;
    };
    std::unordered_map<wl_data_offer *, std::unique_ptr<Offer>> offers;
    Offer *selected = nullptr;
    int selection_events = 0, cancellations = 0, reads = 0, failures = 0;
    size_t last_read_bytes = 0;
    struct Source {
        Impl *owner;
        wl_data_source *proxy;
        std::shared_ptr<const std::string> data;
    };
    std::unique_ptr<Source> source;
    struct Send {
        int fd;
        size_t offset = 0;
        std::shared_ptr<const std::string> data;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<Send> sends;
    int read_fd = -1;
    bool overflowing = false;
    size_t read_total = 0;
    std::string reading;
    std::function<void(std::string)> receive;
    std::chrono::steady_clock::time_point read_deadline;
    explicit Impl(wl_display *d) : display(d) {}
    ~Impl() {
        if (read_fd >= 0)
            close(read_fd);
        for (auto &s : sends)
            close(s.fd);
        if (source)
            wl_data_source_destroy(source->proxy);
        for (auto &[p, o] : offers)
            wl_data_offer_destroy(p);
        if (pointer) {
            if (wl_pointer_get_version(pointer) >= 3)
                wl_pointer_release(pointer);
            else
                wl_pointer_destroy(pointer);
        }
        if (keyboard) {
            if (wl_keyboard_get_version(keyboard) >= 3)
                wl_keyboard_release(keyboard);
            else
                wl_keyboard_destroy(keyboard);
        }
        if (device) {
            if (wl_data_device_get_version(device) >= 2)
                wl_data_device_release(device);
            else
                wl_data_device_destroy(device);
        }
        if (seat) {
            if (wl_seat_get_version(seat) >= 5)
                wl_seat_release(seat);
            else
                wl_seat_destroy(seat);
        }
        if (manager)
            wl_data_device_manager_destroy(manager);
        if (registry)
            wl_registry_destroy(registry);
    }
    static void global(void *d, wl_registry *r, uint32_t name, const char *type, uint32_t version) {
        auto &p = *static_cast<Impl *>(d);
        if (!strcmp(type, "wl_data_device_manager"))
            p.manager = static_cast<wl_data_device_manager *>(wl_registry_bind(
                r, name, &wl_data_device_manager_interface, std::min(version, 3u)));
        if (!p.seat && !strcmp(type, "wl_seat")) {
            p.seat = static_cast<wl_seat *>(
                wl_registry_bind(r, name, &wl_seat_interface, std::min(version, 7u)));
            static const wl_seat_listener l{capabilities, seat_name};
            wl_seat_add_listener(p.seat, &l, &p);
        }
    }
    static void removed(void *, wl_registry *, uint32_t) {}
    static void seat_name(void *, wl_seat *, const char *) {}
    static void keymap(void *, wl_keyboard *, uint32_t, int32_t fd, uint32_t) { close(fd); }
    static void keyboard_enter(void *d, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {
        static_cast<Impl *>(d)->focused = true;
    }
    static void keyboard_leave(void *d, wl_keyboard *, uint32_t, wl_surface *) {
        auto &p = *static_cast<Impl *>(d);
        p.focused = false;
        if (p.receive)
            p.finish(false);
    }
    static void key(void *d, wl_keyboard *, uint32_t serial, uint32_t, uint32_t, uint32_t) {
        static_cast<Impl *>(d)->serial = serial;
    }
    static void mods(void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
    static void repeat(void *, wl_keyboard *, int32_t, int32_t) {}
    static void enter(void *, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t) {}
    static void leave(void *, wl_pointer *, uint32_t, wl_surface *) {}
    static void motion(void *, wl_pointer *, uint32_t, wl_fixed_t, wl_fixed_t) {}
    static void button(void *d, wl_pointer *, uint32_t serial, uint32_t, uint32_t, uint32_t) {
        static_cast<Impl *>(d)->serial = serial;
    }
    static void axis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
    static void frame(void *, wl_pointer *) {}
    static void axis_source(void *, wl_pointer *, uint32_t) {}
    static void axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
    static void axis_discrete(void *, wl_pointer *, uint32_t, int32_t) {}
    static void capabilities(void *d, wl_seat *s, uint32_t caps) {
        auto &p = *static_cast<Impl *>(d);
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !p.keyboard) {
            p.keyboard = wl_seat_get_keyboard(s);
            static const wl_keyboard_listener l{keymap, keyboard_enter, keyboard_leave,
                                                key,    mods,           repeat};
            wl_keyboard_add_listener(p.keyboard, &l, &p);
        }
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !p.pointer) {
            p.pointer = wl_seat_get_pointer(s);
            static const wl_pointer_listener l{enter, leave,       motion,    button,       axis,
                                               frame, axis_source, axis_stop, axis_discrete};
            wl_pointer_add_listener(p.pointer, &l, &p);
        }
    }
    static void mime(void *d, wl_data_offer *, const char *m) {
        auto &o = *static_cast<Offer *>(d);
        if (!strcmp(m, "text/plain;charset=utf-8") ||
            (!o.mime.size() && (!strcmp(m, "text/plain") || !strcmp(m, "UTF8_STRING"))))
            o.mime = m;
    }
    static void offer_actions(void *, wl_data_offer *, uint32_t) {}
    static void offer(void *d, wl_data_device *, wl_data_offer *o) {
        auto &p = *static_cast<Impl *>(d);
        if (p.offers.size() >= 64) {
            wl_data_offer_destroy(o);
            return;
        }
        auto value = std::make_unique<Offer>();
        value->proxy = o;
        static const wl_data_offer_listener l{mime, offer_actions, offer_actions};
        wl_data_offer_add_listener(o, &l, value.get());
        p.offers[o] = std::move(value);
    }
    static void drag_enter(void *, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t,
                           wl_data_offer *) {}
    static void drag_leave(void *, wl_data_device *) {}
    static void drag_motion(void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {}
    static void drag_drop(void *, wl_data_device *) {}
    static void selection(void *d, wl_data_device *, wl_data_offer *o) {
        auto &p = *static_cast<Impl *>(d);
        ++p.selection_events;
        auto it = p.offers.find(o);
        p.selected = it == p.offers.end() ? nullptr : it->second.get();
        for (auto i = p.offers.begin(); i != p.offers.end();)
            if (i->first != o) {
                wl_data_offer_destroy(i->first);
                i = p.offers.erase(i);
            } else
                ++i;
    }
    static void target(void *, wl_data_source *, const char *) {}
    static void send(void *d, wl_data_source *, const char *m, int32_t fd) {
        auto &s = *static_cast<Source *>(d);
        if (strcmp(m, "text/plain;charset=utf-8") && strcmp(m, "text/plain")) {
            close(fd);
            return;
        }
        if (s.owner->sends.size() >= 8) {
            close(fd);
            return;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        s.owner->sends.push_back({fd, 0, s.data, now() + std::chrono::seconds(5)});
    }
    static void cancelled(void *d, wl_data_source *proxy) {
        auto &s = *static_cast<Source *>(d);
        auto *p = s.owner;
        ++p->cancellations;
        wl_data_source_destroy(proxy);
        if (p->source.get() == &s)
            p->source.reset();
    }
    static void source_event(void *, wl_data_source *) {}
    static void source_action(void *, wl_data_source *, uint32_t) {}
    void finish(bool success) {
        ++reads;
        if (!success)
            ++failures;
        last_read_bytes = read_total;
        if (read_fd >= 0)
            close(read_fd);
        read_fd = -1;
        auto callback = std::move(receive);
        auto result = success ? std::move(reading) : std::string{};
        reading.clear();
        if (callback)
            callback(std::move(result));
    }
};
WaylandClipboard::WaylandClipboard(wl_display *d) : impl_(std::make_unique<Impl>(d)) {
    auto &p = *impl_;
    p.registry = wl_display_get_registry(d);
    static const wl_registry_listener l{Impl::global, Impl::removed};
    wl_registry_add_listener(p.registry, &l, &p);
    wl_display_roundtrip(d);
    if (p.manager && p.seat) {
        p.device = wl_data_device_manager_get_data_device(p.manager, p.seat);
        static const wl_data_device_listener listener{Impl::offer,      Impl::drag_enter,
                                                      Impl::drag_leave, Impl::drag_motion,
                                                      Impl::drag_drop,  Impl::selection};
        wl_data_device_add_listener(p.device, &listener, &p);
        wl_display_roundtrip(d);
    }
}
WaylandClipboard::~WaylandClipboard() = default;
void WaylandClipboard::copy(const std::string &text) {
    auto &p = *impl_;
    if (!p.device || !p.serial || !p.focused || text.size() > limit)
        return;
    if (p.source)
        wl_data_source_destroy(p.source->proxy);
    p.source = std::make_unique<Impl::Source>();
    p.source->owner = &p;
    p.source->data = std::make_shared<const std::string>(text);
    p.source->proxy = wl_data_device_manager_create_data_source(p.manager);
    static const wl_data_source_listener l{Impl::target,       Impl::send,
                                           Impl::cancelled,    Impl::source_event,
                                           Impl::source_event, Impl::source_action};
    wl_data_source_add_listener(p.source->proxy, &l, p.source.get());
    wl_data_source_offer(p.source->proxy, "text/plain;charset=utf-8");
    wl_data_source_offer(p.source->proxy, "text/plain");
    wl_data_device_set_selection(p.device, p.source->proxy, p.serial);
    wl_display_flush(p.display);
}
void WaylandClipboard::paste(std::function<void(std::string)> callback) {
    auto &p = *impl_;
    if (p.receive)
        p.finish(false);
    p.receive = std::move(callback);
    p.overflowing = false;
    p.read_total = 0;
    if (p.source) {
        p.reading = *p.source->data;
        p.read_total = p.reading.size();
        p.finish(true);
        return;
    }
    if (!p.selected || p.selected->mime.empty()) {
        p.finish(false);
        return;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK)) {
        p.finish(false);
        return;
    }
    p.read_fd = fds[0];
    p.read_deadline = now() + std::chrono::seconds(5);
    wl_data_offer_receive(p.selected->proxy, p.selected->mime.c_str(), fds[1]);
    close(fds[1]);
    wl_display_flush(p.display);
}
void WaylandClipboard::poll() {
    auto &p = *impl_;
    for (auto i = p.sends.begin(); i != p.sends.end();) {
        bool done = now() > i->deadline;
        while (!done && i->offset < i->data->size()) {
            auto n = pipe_write(i->fd, i->data->data() + i->offset,
                                std::min(size_t(65536), i->data->size() - i->offset));
            if (n > 0)
                i->offset += n;
            else if (n < 0 && errno == EINTR)
                continue;
            else {
                done = !(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
                break;
            }
        }
        if (done || i->offset == i->data->size()) {
            close(i->fd);
            i = p.sends.erase(i);
        } else
            ++i;
    }
    if (p.read_fd >= 0) {
        char bytes[65536];
        for (int chunks = 0; chunks < 32; ++chunks) {
            auto n = read(p.read_fd, bytes, sizeof(bytes));
            if (n > 0) {
                p.read_total = std::min(limit + 1, p.read_total + size_t(n));
                if (p.overflowing)
                    continue;
                if (size_t(n) > limit - p.reading.size()) {
                    // Drain the rest without retaining it. A normal producer can finish
                    // its pipe cleanly instead of receiving SIGPIPE on a size refusal.
                    p.overflowing = true;
                    p.reading.clear();
                    continue;
                }
                p.reading.append(bytes, n);
            } else if (n == 0) {
                p.finish(!p.overflowing);
                break;
            } else if (errno == EINTR)
                continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else {
                p.finish(false);
                break;
            }
        }
        if (p.read_fd >= 0 && now() > p.read_deadline)
            p.finish(false);
    }
}
} // namespace aura

namespace aura {
std::string WaylandClipboard::report() const {
    const auto &p = *impl_;
    return "{\"focused\":" + std::string(p.focused ? "true" : "false") +
           ",\"selection_events\":" + std::to_string(p.selection_events) +
           ",\"cancellations\":" + std::to_string(p.cancellations) +
           ",\"has_source\":" + (p.source ? "true" : "false") +
           ",\"has_selection\":" + (p.selected ? "true" : "false") +
           ",\"has_mime\":" + (p.selected && !p.selected->mime.empty() ? "true" : "false") +
           ",\"reads\":" + std::to_string(p.reads) + ",\"failures\":" + std::to_string(p.failures) +
           ",\"last_read_bytes\":" + std::to_string(p.last_read_bytes) + "}";
}
} // namespace aura

namespace aura {
bool WaylandClipboard::pending() const { return impl_->read_fd >= 0 || !impl_->sends.empty(); }
} // namespace aura
