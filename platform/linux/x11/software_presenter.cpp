#include "platform/linux/x11/software_presenter.hpp"
#include "include/core/SkPixmap.h"
#include "platform/linux/x11/wayland_refresh.hpp"
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xpresent.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/shm.h>
#include <vector>

namespace aura {
namespace {
// XShmAttach may fail asynchronously on remote or restricted X servers. Trap
// only errors for our display; preserve the handler for CEF's other displays.
std::mutex attach_mutex;
std::atomic<Display *> attach_display{};
std::atomic_bool attach_failed{};
std::atomic<XErrorHandler> prior_handler{};
int attach_error(Display *display, XErrorEvent *event) {
    if (display == attach_display.load()) {
        attach_failed = true;
        return 0;
    }
    if (const auto previous = prior_handler.load())
        return previous(display, event);
    return 0;
}
bool attach(Display *display, XShmSegmentInfo &segment) {
    std::lock_guard lock(attach_mutex);
    XSync(display, False);
    attach_display = display;
    attach_failed = false;
    prior_handler = XSetErrorHandler(attach_error);
    const bool requested = XShmAttach(display, &segment);
    XSync(display, False);
    XSetErrorHandler(prior_handler.load());
    attach_display = nullptr;
    return requested && !attach_failed;
}
bool allowed(const char *name) {
    const auto value = std::getenv(name);
    return !value || std::string(value) != "0";
}
} // namespace
struct SoftwarePresenter::Impl {
    Display *display;
    Window window;
    GC gc;
    int width = 0, height = 0, opcode = 0;
    sk_sp<SkSurface> raster;
    XImage *image = nullptr;
    XShmSegmentInfo segment{};
    bool attached = false, marked_for_removal = false;
    void *present_library = nullptr, *randr_library = nullptr;
    decltype(&XRRGetScreenResourcesCurrent) screen_resources = nullptr;
    decltype(&XRRGetCrtcInfo) crtc_info = nullptr;
    decltype(&XRRFreeScreenResources) free_resources = nullptr;
    decltype(&XRRFreeCrtcInfo) free_crtc = nullptr;
    int refresh = 60;
    WaylandRefresh wayland;
    std::chrono::steady_clock::time_point refresh_checked{};
    decltype(&XPresentQueryExtension) query = nullptr;
    decltype(&XPresentSelectInput) select = nullptr;
    decltype(&XPresentFreeInput) free_input = nullptr;
    decltype(&XPresentPixmap) present = nullptr;
    XID subscription = 0;
    std::vector<std::uint32_t> pending;
    std::uint64_t last_target_msc = 0, superseded = 0;
    std::size_t maximum_pending = 0;
    std::chrono::steady_clock::time_point last_complete{};
    struct Slot {
        Pixmap pixmap = 0;
        bool busy = false;
        std::uint32_t serial = 0;
    };
    std::array<Slot, 2> slots{};
    std::uint32_t serial = 0;
    std::uint64_t completed = 0, skipped = 0, last_ust = 0, last_msc = 0;
    std::array<double, 240> intervals{};
    std::size_t interval_count = 0, interval_index = 0;
    std::uint64_t shm_frames = 0;
    std::array<std::array<std::uint64_t, 3>, 240> frames{};
    std::size_t frame_index = 0, frame_count = 0;
    Impl(Display *d, Window w, GC g) : display(d), window(w), gc(g) {
        segment.shmid = -1;
        randr_library = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (randr_library) {
            screen_resources = reinterpret_cast<decltype(screen_resources)>(
                dlsym(randr_library, "XRRGetScreenResourcesCurrent"));
            crtc_info =
                reinterpret_cast<decltype(crtc_info)>(dlsym(randr_library, "XRRGetCrtcInfo"));
            free_resources = reinterpret_cast<decltype(free_resources)>(
                dlsym(randr_library, "XRRFreeScreenResources"));
            free_crtc =
                reinterpret_cast<decltype(free_crtc)>(dlsym(randr_library, "XRRFreeCrtcInfo"));
        }
        if (!allowed("AURA_X11_PRESENT"))
            return;
        present_library = dlopen("libXpresent.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (!present_library)
            return;
        query = reinterpret_cast<decltype(query)>(dlsym(present_library, "XPresentQueryExtension"));
        select = reinterpret_cast<decltype(select)>(dlsym(present_library, "XPresentSelectInput"));
        free_input =
            reinterpret_cast<decltype(free_input)>(dlsym(present_library, "XPresentFreeInput"));
        present = reinterpret_cast<decltype(present)>(dlsym(present_library, "XPresentPixmap"));
        int event_base, error_base;
        if (query && select && free_input && present &&
            query(display, &opcode, &event_base, &error_base))
            subscription =
                select(display, window, PresentCompleteNotifyMask | PresentIdleNotifyMask);
    }
    void release() {
        // Ensure all uploads have consumed shared pixels before unmapping them.
        if (display)
            XSync(display, False);
        raster.reset();
        for (auto &slot : slots) {
            if (slot.pixmap)
                XFreePixmap(display, slot.pixmap);
            slot = {};
        }
        if (attached) {
            XShmDetach(display, &segment);
            XSync(display, False);
            attached = false;
        }
        if (image) {
            image->data = nullptr;
            XDestroyImage(image);
            image = nullptr;
        }
        if (segment.shmaddr)
            shmdt(segment.shmaddr);
        if (segment.shmid >= 0 && !marked_for_removal)
            shmctl(segment.shmid, IPC_RMID, nullptr);
        marked_for_removal = false;
        segment = {};
        segment.shmid = -1;
    }
    ~Impl() {
        release();
        if (subscription)
            free_input(display, window, subscription);
        if (present_library)
            dlclose(present_library);
        if (randr_library)
            dlclose(randr_library);
    }
    bool shared_surface() {
        if (!allowed("AURA_X11_SHM") || !XShmQueryExtension(display))
            return false;
        auto *visual = DefaultVisual(display, DefaultScreen(display));
        image = XShmCreateImage(display, visual, DefaultDepth(display, DefaultScreen(display)),
                                ZPixmap, nullptr, &segment, width, height);
        if (!image || image->bits_per_pixel != 32 || image->byte_order != LSBFirst ||
            visual->red_mask != 0xFF0000 || visual->green_mask != 0xFF00 ||
            visual->blue_mask != 0xFF)
            return false;
        const auto bytes = std::size_t(image->bytes_per_line) * image->height;
        segment.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (segment.shmid < 0)
            return false;
        const auto address = shmat(segment.shmid, nullptr, 0);
        if (address == reinterpret_cast<void *>(-1))
            return false;
        segment.shmaddr = image->data = static_cast<char *>(address);
        segment.readOnly = True;
        if (!attach(display, segment))
            return false;
        attached = true;
        // Both processes hold the segment; mark it for removal even after a crash.
        marked_for_removal = shmctl(segment.shmid, IPC_RMID, nullptr) == 0;
        raster = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(width, height), image->data,
                                        image->bytes_per_line);
        return bool(raster);
    }
    void submit(Rect damage) {
        auto slot = std::find_if(slots.begin(), slots.end(), [](const auto &s) { return !s.busy; });
        if (slot == slots.end())
            return;
        const auto area = subscription ? Rect{0, 0, width, height} : damage;
        if (attached) {
            XShmPutImage(display, slot->pixmap, gc, image, area.x, area.y, area.x, area.y,
                         area.width, area.height, False);
            // This is an upload barrier, not a presentation wait. Drawing cannot
            // modify the shared pixels until the server has copied them.
            XSync(display, False);
            ++shm_frames;
        } else {
            SkPixmap pixels;
            raster->peekPixels(&pixels);
            auto *upload = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                                        DefaultDepth(display, DefaultScreen(display)), ZPixmap, 0,
                                        static_cast<char *>(pixels.writable_addr()), width, height,
                                        32, pixels.rowBytes());
            if (!upload)
                throw std::runtime_error("XImage allocation failed");
            XPutImage(display, slot->pixmap, gc, upload, area.x, area.y, area.x, area.y, area.width,
                      area.height);
            upload->data = nullptr;
            XDestroyImage(upload);
        }
        if (subscription) {
            slot->serial = ++serial;
            slot->busy = true;
            pending.push_back(slot->serial);
            maximum_pending = std::max(maximum_pending, pending.size());
            last_target_msc = std::max(last_target_msc + 1, last_msc + 1);
            // No Async/MayTear flags. At most two frames can be in flight; when
            // both buffers are busy the caller keeps only the latest CEF frame.
            present(display, window, slot->pixmap, slot->serial, 0, 0, 0, 0, 0, 0, 0,
                    PresentOptionNone, last_target_msc, 0, 0, nullptr, 0);
        } else {
            // The upload finishes in an offscreen pixmap before one window copy,
            // even if MIT-SHM or Present is unavailable.
            XCopyArea(display, slot->pixmap, window, gc, area.x, area.y, area.width, area.height,
                      area.x, area.y);
        }
        XFlush(display);
    }
};
SoftwarePresenter::SoftwarePresenter(Display *d, Window w, GC gc)
    : impl_(std::make_unique<Impl>(d, w, gc)) {}
SoftwarePresenter::~SoftwarePresenter() = default;
bool SoftwarePresenter::resize(int width, int height) {
    auto &p = *impl_;
    if (width == p.width && height == p.height)
        return false;
    p.release();
    p.width = width;
    p.height = height;
    if (!p.shared_surface()) {
        p.release();
        p.raster = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    }
    if (!p.raster)
        throw std::runtime_error("Linux raster surface allocation failed");
    for (auto &slot : p.slots)
        slot.pixmap = XCreatePixmap(p.display, p.window, width, height,
                                    DefaultDepth(p.display, DefaultScreen(p.display)));
    return true;
}
sk_sp<SkSurface> SoftwarePresenter::surface() const {
    return impl_->raster;
}
int SoftwarePresenter::refresh_rate() {
    auto &p = *impl_;
    const auto now = std::chrono::steady_clock::now();
    if (now - p.refresh_checked < std::chrono::seconds(1))
        return p.refresh;
    p.refresh_checked = now;
    const auto root = DefaultRootWindow(p.display);
    XWindowAttributes attributes{};
    int x = 0, y = 0;
    Window child;
    XGetWindowAttributes(p.display, p.window, &attributes);
    XTranslateCoordinates(p.display, p.window, root, 0, 0, &x, &y, &child);
    // Xwayland can expose a synthetic 60 Hz RandR mode on faster Wayland outputs.
    if (const auto rate = p.wayland.rate({x, y, attributes.width, attributes.height})) {
        p.refresh = rate;
        return p.refresh;
    }
    if (!p.screen_resources || !p.crtc_info || !p.free_resources || !p.free_crtc)
        return p.refresh;
    auto *resources = p.screen_resources(p.display, root);
    if (!resources)
        return p.refresh;
    std::int64_t best_area = 0;
    for (int i = 0; i < resources->ncrtc; ++i) {
        auto *crtc = p.crtc_info(p.display, resources, resources->crtcs[i]);
        if (!crtc)
            continue;
        const auto overlap = intersect({x, y, attributes.width, attributes.height},
                                       {crtc->x, crtc->y, int(crtc->width), int(crtc->height)});
        const auto area = std::int64_t(overlap.width) * overlap.height;
        if (crtc->mode && area > best_area) {
            for (int j = 0; j < resources->nmode; ++j) {
                const auto &mode = resources->modes[j];
                if (mode.id != crtc->mode || !mode.hTotal || !mode.vTotal || !mode.dotClock)
                    continue;
                double rate = double(mode.dotClock) / mode.hTotal / mode.vTotal;
                if (mode.modeFlags & RR_Interlace)
                    rate *= 2;
                if (mode.modeFlags & RR_DoubleScan)
                    rate /= 2;
                if (rate >= 30 && rate <= 360) {
                    p.refresh = int(std::lround(rate));
                    best_area = area;
                }
            }
        }
        p.free_crtc(crtc);
    }
    p.free_resources(resources);
    return p.refresh;
}
bool SoftwarePresenter::ready() const {
    const auto &p = *impl_;
    const auto limit =
        std::chrono::steady_clock::now() - p.last_complete < std::chrono::milliseconds(100) ? 2u
                                                                                            : 1u;
    return p.pending.size() < limit &&
           std::any_of(p.slots.begin(), p.slots.end(), [](const auto &s) { return !s.busy; });
}
void SoftwarePresenter::submit(Rect damage) {
    impl_->submit(damage);
}
bool SoftwarePresenter::event(XEvent &event) {
    auto &p = *impl_;
    if (!p.subscription || event.type != GenericEvent || event.xcookie.extension != p.opcode)
        return false;
    if (!XGetEventData(p.display, &event.xcookie))
        return true;
    if (event.xcookie.evtype == PresentIdleNotify) {
        const auto &idle = *static_cast<XPresentIdleNotifyEvent *>(event.xcookie.data);
        for (auto &slot : p.slots)
            if (slot.pixmap == idle.pixmap && slot.serial == idle.serial_number)
                slot.busy = false;
    } else if (event.xcookie.evtype == PresentCompleteNotify) {
        const auto &complete = *static_cast<XPresentCompleteNotifyEvent *>(event.xcookie.data);
        if (complete.kind == PresentCompleteKindPixmap) {
            std::erase(p.pending, complete.serial_number);
            p.last_complete = std::chrono::steady_clock::now();
            p.last_target_msc = std::max(p.last_target_msc, complete.msc);
            if (complete.mode == PresentCompleteModeSkip)
                ++p.skipped;
            else {
                if (p.last_ust && complete.ust > p.last_ust && complete.ust - p.last_ust < 100000) {
                    p.intervals[p.interval_index] = (complete.ust - p.last_ust) / 1000.0;
                    p.interval_index = (p.interval_index + 1) % p.intervals.size();
                    p.interval_count = std::min(p.interval_count + 1, p.intervals.size());
                }
                const auto previous = (p.frame_index + p.frames.size() - 1) % p.frames.size();
                if (p.frame_count && p.frames[previous][2] == complete.msc) {
                    // Only the last copy at one refresh can be visible. Keep
                    // that sample and report superseded requests explicitly.
                    p.frames[previous] = {complete.serial_number, complete.ust, complete.msc};
                    ++p.superseded;
                } else {
                    p.frames[p.frame_index] = {complete.serial_number, complete.ust, complete.msc};
                    p.frame_index = (p.frame_index + 1) % p.frames.size();
                    p.frame_count = std::min(p.frame_count + 1, p.frames.size());
                }
                p.last_ust = complete.ust;
                p.last_msc = complete.msc;
                ++p.completed;
            }
        }
    }
    XFreeEventData(p.display, &event.xcookie);
    return true;
}
std::string SoftwarePresenter::report() const {
    const auto &p = *impl_;
    auto intervals = p.intervals;
    std::sort(intervals.begin(), intervals.begin() + p.interval_count);
    std::ostringstream out;
    out << "{\"refresh_hz\":" << p.refresh
        << ",\"synchronized\":" << (p.subscription ? "true" : "false")
        << ",\"shared_memory\":" << (p.attached ? "true" : "false")
        << ",\"completed\":" << p.completed << ",\"skipped\":" << p.skipped
        << ",\"maximum_pending\":" << p.maximum_pending << ",\"superseded\":" << p.superseded
        << ",\"shm_frames\":" << p.shm_frames << ",\"last_ust\":" << p.last_ust
        << ",\"last_msc\":" << p.last_msc
        << ",\"interval_ms_p50\":" << (p.interval_count ? intervals[(p.interval_count - 1) / 2] : 0)
        << ",\"interval_ms_p95\":"
        << (p.interval_count ? intervals[(p.interval_count - 1) * 95 / 100] : 0) << ",\"frames\":[";
    for (std::size_t i = 0; i < p.frame_count; ++i) {
        const auto &frame =
            p.frames[(p.frame_index + p.frames.size() - p.frame_count + i) % p.frames.size()];
        if (i)
            out << ',';
        out << '[' << frame[0] << ',' << frame[1] << ',' << frame[2] << ']';
    }
    out << "]}";
    return out.str();
}
} // namespace aura
