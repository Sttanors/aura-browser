#include "platform/linux/x11/browser_window.hpp"
#include "common/branding_data.hpp"
#include "platform/linux/x11/software_presenter.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace aura {
namespace {
int cef_modifiers(unsigned mask) {
    int flags = 0;
    if (mask & ShiftMask)
        flags |= EVENTFLAG_SHIFT_DOWN;
    if (mask & ControlMask)
        flags |= EVENTFLAG_CONTROL_DOWN;
    if (mask & Mod1Mask)
        flags |= EVENTFLAG_ALT_DOWN;
    if (mask & Mod4Mask)
        flags |= EVENTFLAG_COMMAND_DOWN;
    if (mask & Mod5Mask)
        flags |= EVENTFLAG_ALTGR_DOWN;
    if (mask & LockMask)
        flags |= EVENTFLAG_CAPS_LOCK_ON;
    if (mask & Button1Mask)
        flags |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (mask & Button2Mask)
        flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (mask & Button3Mask)
        flags |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return flags;
}
int windows_key(KeySym key) {
    if (key >= XK_a && key <= XK_z)
        return static_cast<int>(key - XK_a + 'A');
    if (key >= XK_A && key <= XK_Z)
        return static_cast<int>(key);
    if (key >= XK_0 && key <= XK_9)
        return static_cast<int>(key);
    if (key >= XK_F1 && key <= XK_F24)
        return static_cast<int>(key - XK_F1 + 0x70);
    switch (key) {
    case XK_BackSpace:
        return 0x08;
    case XK_Tab:
    case XK_ISO_Left_Tab:
        return 0x09;
    case XK_Return:
    case XK_KP_Enter:
        return 0x0D;
    case XK_Escape:
        return 0x1B;
    case XK_space:
        return 0x20;
    case XK_Page_Up:
        return 0x21;
    case XK_Page_Down:
        return 0x22;
    case XK_End:
        return 0x23;
    case XK_Home:
        return 0x24;
    case XK_Left:
        return 0x25;
    case XK_Up:
        return 0x26;
    case XK_Right:
        return 0x27;
    case XK_Down:
        return 0x28;
    case XK_Insert:
        return 0x2D;
    case XK_Delete:
        return 0x2E;
    case XK_Shift_L:
    case XK_Shift_R:
        return 0x10;
    case XK_Control_L:
    case XK_Control_R:
        return 0x11;
    case XK_Alt_L:
    case XK_Alt_R:
        return 0x12;
    case XK_semicolon:
        return 0xBA;
    case XK_equal:
        return 0xBB;
    case XK_comma:
        return 0xBC;
    case XK_minus:
        return 0xBD;
    case XK_period:
        return 0xBE;
    case XK_slash:
        return 0xBF;
    case XK_grave:
        return 0xC0;
    case XK_bracketleft:
        return 0xDB;
    case XK_backslash:
        return 0xDC;
    case XK_bracketright:
        return 0xDD;
    case XK_apostrophe:
        return 0xDE;
    default:
        return 0;
    }
}
} // namespace

struct BrowserWindow::Impl {
    WindowCallbacks callbacks;
    double scale, base_scale, requested_zoom = 1;
    std::string composition, graphics_renderer, clipboard;
    std::function<void(std::string)> paste_callback;
    std::string paste_bytes;
    bool paste_incremental = false;
    std::chrono::steady_clock::time_point paste_deadline;
    static constexpr size_t paste_limit = 16 * 1024 * 1024;
    std::uint64_t frame_count = 0;
    Cursor cursors[3]{};
    int current_cursor = -1;
    Display *display{};
    ::Window window{};
    GC gc{};
    Atom delete_window{}, wm_protocols{};
    XIM input_method{};
    XIC input_context{};

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLContext egl_context = EGL_NO_CONTEXT;
    sk_sp<GrDirectContext> gpu;
    sk_sp<SkSurface> surface;
    int surface_width{}, surface_height{}, stencil_bits{};
    int pixel_width{}, pixel_height{}, width{}, height{};

    bool dirty = true, mapped = false, full_damage = true;
    Rect damage{};
    int wake_fd = -1;
    sk_sp<SkSurface> raster;
    std::unique_ptr<SoftwarePresenter> software;
    std::array<double, 240> paint_costs{}, draw_costs{}, upload_costs{};
    std::size_t paint_cost_count = 0, paint_cost_index = 0;
    std::uint64_t raster_allocations = 0, presented_pixels = 0, content_frames = 0;
    void presented(std::chrono::steady_clock::time_point start, std::uint64_t pixels) {
        paint_costs[paint_cost_index] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        paint_cost_index = (paint_cost_index + 1) % paint_costs.size();
        paint_cost_count = std::min(paint_cost_count + 1, paint_costs.size());
        presented_pixels += pixels;
        ++frame_count;
        dirty = false;
        full_damage = false;
    }
    void invalidate(Rect r) {
        if (full_damage) {
            dirty = true;
            return;
        }
        if (dirty) {
            const auto right = std::max(damage.x + damage.width, r.x + r.width);
            const auto bottom = std::max(damage.y + damage.height, r.y + r.height);
            damage.x = std::min(damage.x, r.x);
            damage.y = std::min(damage.y, r.y);
            damage.width = right - damage.x;
            damage.height = bottom - damage.y;
        } else
            damage = r;
        dirty = true;
    }
    Time last_click{};
    unsigned last_button{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    Impl(double value, WindowCallbacks hooks)
        : callbacks(std::move(hooks)), scale(value), base_scale(value) {
        wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd < 0)
            throw std::runtime_error("Could not create the browser wake event");
    }
    ~Impl() {
        if (wake_fd >= 0)
            close(wake_fd);
        reset_graphics();
        raster.reset();
        software.reset();
        if (input_context)
            XDestroyIC(input_context);
        if (input_method)
            XCloseIM(input_method);
        for (auto c : cursors)
            if (c)
                XFreeCursor(display, c);
        if (gc)
            XFreeGC(display, gc);
        if (window)
            XDestroyWindow(display, window);
        if (display)
            XCloseDisplay(display);
    }
    void reset_graphics() {
        if (egl_context != EGL_NO_CONTEXT)
            eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
        surface.reset();
        if (gpu)
            gpu->releaseResourcesAndAbandonContext();
        gpu.reset();
        if (egl_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_surface != EGL_NO_SURFACE)
                eglDestroySurface(egl_display, egl_surface);
            if (egl_context != EGL_NO_CONTEXT)
                eglDestroyContext(egl_display, egl_context);
            eglTerminate(egl_display);
        }
        egl_display = EGL_NO_DISPLAY;
        egl_surface = EGL_NO_SURFACE;
        egl_context = EGL_NO_CONTEXT;
    }
    Atom atom(const char *name) {
        return XInternAtom(display, name, False);
    }
    void initialize_graphics() {
        egl_display = eglGetDisplay(display);
        if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, nullptr, nullptr))
            throw std::runtime_error("EGL display initialization failed");
        if (!eglBindAPI(EGL_OPENGL_ES_API))
            throw std::runtime_error("EGL GLES API unavailable");
        const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                     EGL_WINDOW_BIT,
                                     EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_ES3_BIT,
                                     EGL_RED_SIZE,
                                     8,
                                     EGL_GREEN_SIZE,
                                     8,
                                     EGL_BLUE_SIZE,
                                     8,
                                     EGL_STENCIL_SIZE,
                                     8,
                                     EGL_NATIVE_VISUAL_ID,
                                     static_cast<EGLint>(XVisualIDFromVisual(
                                         DefaultVisual(display, DefaultScreen(display)))),
                                     EGL_NONE};
        EGLConfig config{};
        EGLint count{};
        if (!eglChooseConfig(egl_display, attributes, &config, 1, &count) || count != 1)
            throw std::runtime_error("No compatible X11 GLES3 EGL config");
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
        egl_surface = eglCreateWindowSurface(egl_display, config, window, nullptr);
        if (egl_context == EGL_NO_CONTEXT || egl_surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
            throw std::runtime_error("EGL window surface creation failed");
        auto interface =
            GrGLMakeAssembledGLESInterface(nullptr, [](void *, const char *name) -> GrGLFuncPtr {
                return reinterpret_cast<GrGLFuncPtr>(eglGetProcAddress(name));
            });
        gpu = GrDirectContexts::MakeGL(std::move(interface));
        if (!gpu)
            throw std::runtime_error("Skia Ganesh GLES initialization failed");
        eglGetConfigAttrib(egl_display, config, EGL_STENCIL_SIZE, &stencil_bits);
        // The CEF UI pump must stay responsive while XWayland occludes the window.
        // UI animation is paced by the host loop; never wait for VSync on this thread.
        eglSwapInterval(egl_display, 0);
        composition = "Skia / EGL / GLES3";
        const auto *renderer = glGetString(GL_RENDERER);
        graphics_renderer =
            renderer ? reinterpret_cast<const char *>(renderer) : "unknown GLES renderer";
        spdlog::info("Linux graphics: {} / {}", composition, graphics_renderer);
    }
    void resize(int w, int h) {
        if (w <= 0 || h <= 0)
            return;
        // Keep every native control reachable even when a saved zoom exceeds the window.
        scale = std::min({base_scale * requested_zoom, w / 900.0, h / 620.0});
        pixel_width = w;
        pixel_height = h;
        width = std::max(1, static_cast<int>(std::lround(w / scale)));
        height = std::max(1, static_cast<int>(std::lround(h / scale)));
        if (callbacks.resize)
            callbacks.resize(width, height, scale / base_scale, base_scale);
        dirty = full_damage = true;
    }
    void draw(SkCanvas &canvas, bool content_only = false) {
        canvas.save();
        canvas.scale(static_cast<float>(scale), static_cast<float>(scale));
        if (content_only && callbacks.draw_content && callbacks.draw_content(canvas))
            ++content_frames;
        else if (callbacks.draw)
            callbacks.draw(canvas);
        canvas.restore();
    }
    void paint() {
        if (!dirty || !mapped || pixel_width <= 0 || pixel_height <= 0)
            return;
        const auto paint_start = std::chrono::steady_clock::now();
        if (gpu) {
            try {
                if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
                    throw std::runtime_error("EGL context activation failed");
                if (!surface || surface_width != pixel_width || surface_height != pixel_height) {
                    surface.reset();
                    const GrGLFramebufferInfo info{0, GL_RGBA8};
                    const auto target = GrBackendRenderTargets::MakeGL(pixel_width, pixel_height, 0,
                                                                       stencil_bits, info);
                    surface = SkSurfaces::WrapBackendRenderTarget(
                        gpu.get(), target, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
                        nullptr, nullptr);
                    if (!surface)
                        throw std::runtime_error("Skia EGL render target wrapping failed");
                    surface_width = pixel_width;
                    surface_height = pixel_height;
                }
                draw(*surface->getCanvas());
                gpu->flushAndSubmit();
                if (!eglSwapBuffers(egl_display, egl_surface))
                    throw std::runtime_error("eglSwapBuffers failed");
                presented(paint_start, std::uint64_t(pixel_width) * pixel_height);
                return;
            } catch (const std::exception &error) {
                spdlog::warn("Linux GPU presentation failed; falling back to raster: {}",
                             error.what());
                reset_graphics();
                composition = "Skia raster / X11";
                graphics_renderer = "raster fallback";
            }
        }
        if (!raster || raster->width() != pixel_width || raster->height() != pixel_height) {
            raster.reset();
            software->resize(pixel_width, pixel_height);
            raster = software->surface();
            ++raster_allocations;
            full_damage = true;
        }
        if (!raster)
            throw std::runtime_error("Linux raster surface allocation failed");
        if (!software->ready())
            return;
        // Keep the backing store and redraw only the page for OSR frames. Native
        // controls and shadows outside this clip stay in the retained surface.
        const int left = int(std::floor(damage.x * scale));
        const int top = int(std::floor(damage.y * scale));
        Rect area =
            full_damage
                ? Rect{0, 0, pixel_width, pixel_height}
                : intersect({left, top, int(std::ceil((damage.x + damage.width) * scale)) - left,
                             int(std::ceil((damage.y + damage.height) * scale)) - top},
                            {0, 0, pixel_width, pixel_height});
        auto &canvas = *raster->getCanvas();
        canvas.save();
        canvas.clipRect(SkRect::MakeXYWH(area.x, area.y, area.width, area.height));
        draw(canvas, !full_damage);
        canvas.restore();
        const auto drawn = std::chrono::steady_clock::now();
        draw_costs[paint_cost_index] =
            std::chrono::duration<double, std::milli>(drawn - paint_start).count();
        software->submit(area);
        upload_costs[paint_cost_index] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawn).count();
        presented(paint_start, std::uint64_t(area.width) * area.height);
    }
    CefMouseEvent mouse(int x, int y, unsigned mask) const {
        CefMouseEvent point;
        point.x = static_cast<int>(std::lround(x / scale));
        point.y = static_cast<int>(std::lround(y / scale));
        point.modifiers = cef_modifiers(mask);
        return point;
    }
    void wm_message(Atom type, long first, long second, long third, long fourth = 0,
                    long fifth = 0) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = type;
        event.xclient.format = 32;
        event.xclient.data.l[0] = first;
        event.xclient.data.l[1] = second;
        event.xclient.data.l[2] = third;
        event.xclient.data.l[3] = fourth;
        event.xclient.data.l[4] = fifth;
        XSendEvent(display, DefaultRootWindow(display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XFlush(display);
    }
    bool chrome_click(const XButtonEvent &button) {
        if (button.button != Button1)
            return false;
        const auto x = button.x / scale, y = button.y / scale;
        constexpr int border = 6;
        const bool left = x < border, right = x >= width - border;
        const bool top = y < border, bottom = y >= height - border;
        int direction = -1;
        if (top && left)
            direction = 0;
        else if (top && right)
            direction = 2;
        else if (bottom && right)
            direction = 4;
        else if (bottom && left)
            direction = 6;
        else if (top)
            direction = 1;
        else if (right)
            direction = 3;
        else if (bottom)
            direction = 5;
        else if (left)
            direction = 7;
        if (direction >= 0) {
            XUngrabPointer(display, CurrentTime);
            wm_message(atom("_NET_WM_MOVERESIZE"), button.x_root, button.y_root, direction, Button1,
                       1);
            return true;
        }
        if (y >= 30 ||
            (x < width - 144 && !(callbacks.caption ? callbacks.caption(x, y) : x >= 240)))
            return false;
        if (x > width - 48) {
            if (callbacks.close)
                callbacks.close();
        } else if (x > width - 96)
            wm_message(atom("_NET_WM_STATE"), 2,
                       static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_VERT")),
                       static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_HORZ")), 1);
        else if (x > width - 144)
            XIconifyWindow(display, window, DefaultScreen(display));
        else {
            XUngrabPointer(display, CurrentTime);
            wm_message(atom("_NET_WM_MOVERESIZE"), button.x_root, button.y_root, 8, Button1, 1);
        }
        return true;
    }
    void key(XKeyEvent &event, bool pressed) {
        const auto symbol = XLookupKeysym(&event, 0);
        CefKeyEvent key_event;
        key_event.type = pressed ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
        key_event.windows_key_code = windows_key(symbol);
        key_event.native_key_code = static_cast<int>(event.keycode);
        key_event.modifiers = cef_modifiers(event.state);
        const bool consumed = callbacks.key && callbacks.key(key_event);
        if (!pressed || consumed || (event.state & (ControlMask | Mod1Mask | Mod4Mask)))
            return;
        std::string text;
        KeySym translated{};
        char buffer[128];
        if (input_context) {
            Status status{};
            const int count = Xutf8LookupString(input_context, &event, buffer, sizeof(buffer),
                                                &translated, &status);
            if (status == XBufferOverflow && count > 0 && count < 65536) {
                text.resize(static_cast<std::size_t>(count));
                const int actual = Xutf8LookupString(input_context, &event, text.data(), count,
                                                     &translated, &status);
                text.resize(static_cast<std::size_t>(std::max(0, actual)));
            } else if (count > 0 && (status == XLookupChars || status == XLookupBoth))
                text.assign(buffer, static_cast<std::size_t>(count));
        } else {
            const int count = XLookupString(&event, buffer, sizeof(buffer), &translated, nullptr);
            if (count > 0)
                text.assign(buffer, static_cast<std::size_t>(count));
        }
        if (!text.empty() && callbacks.text)
            callbacks.text(text);
    }
    void finish_paste(bool success) {
        spdlog::debug("Clipboard transfer complete: success={}, bytes={}", success, paste_bytes.size());
        paste_incremental = false;
        auto callback = std::move(paste_callback);
        auto text = success ? std::move(paste_bytes) : std::string{};
        paste_bytes.clear();
        if (callback)
            callback(std::move(text));
    }
    void read_paste_property(bool initial) {
        const auto property = atom("AURA_PASTE");
        unsigned long offset = 0;
        bool received = false;
        for (;;) {
            Atom type{};
            int format{};
            unsigned long count{}, after{};
            unsigned char *bytes{};
            const int result = XGetWindowProperty(display, window, property, offset, 16384,
                                                   False, AnyPropertyType, &type, &format,
                                                   &count, &after, &bytes);
            if (result != Success) {
                if (bytes) XFree(bytes);
                finish_paste(false);
                return;
            }
            if (initial && offset == 0 && type == atom("INCR")) {
                // xclip uses an empty INCR header rather than a size hint.
                // Enforce the hard limit on actual chunks in either case.
                const bool valid = format == 32 && (count == 0 ||
                    (count == 1 && bytes && *reinterpret_cast<unsigned long *>(bytes) <= paste_limit));
                if (bytes) XFree(bytes);
                if (!valid) {
                    XDeleteProperty(display, window, property);
                    finish_paste(false);
                    return;
                }
                paste_incremental = true;
                spdlog::debug("Clipboard INCR started");
                // Deleting acknowledges INCR and asks the owner for the first chunk.
                XDeleteProperty(display, window, property);
                XFlush(display);
                return;
            }
            // An old PropertyNotify may precede the first actual INCR chunk.
            if (type == None && paste_incremental) {
                if (bytes) XFree(bytes);
                return;
            }
            const bool valid = format == 8 &&
                (type == atom("UTF8_STRING") || type == XA_STRING || type == atom("TEXT")) &&
                count <= paste_limit - paste_bytes.size();
            if (!valid) {
                if (bytes) XFree(bytes);
                XDeleteProperty(display, window, property);
                finish_paste(false);
                return;
            }
            if (count && bytes) {
                paste_bytes.append(reinterpret_cast<char *>(bytes), count);
                received = true;
            }
            if (bytes) XFree(bytes);
            if (!after)
                break;
            if (!count) {
                finish_paste(false);
                return;
            }
            offset += (count + 3) / 4;
        }
        XDeleteProperty(display, window, property);
        XFlush(display);
        paste_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        if (!paste_incremental || !received)
            finish_paste(true);
    }
    void selection_request(const XSelectionRequestEvent &request) {
        XEvent response{};
        response.xselection.type = SelectionNotify;
        response.xselection.display = display;
        response.xselection.requestor = request.requestor;
        response.xselection.selection = request.selection;
        response.xselection.target = request.target;
        response.xselection.time = request.time;
        response.xselection.property = None;
        const auto property = request.property == None ? request.target : request.property;
        if (request.target == atom("TARGETS")) {
            Atom targets[] = {atom("UTF8_STRING"), XA_STRING, atom("TEXT"), atom("TARGETS")};
            XChangeProperty(display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<unsigned char *>(targets), 4);
            response.xselection.property = property;
        } else if (request.target == atom("UTF8_STRING") || request.target == XA_STRING ||
                   request.target == atom("TEXT")) {
            XChangeProperty(display, request.requestor, property, request.target, 8,
                            PropModeReplace,
                            reinterpret_cast<const unsigned char *>(clipboard.data()),
                            static_cast<int>(clipboard.size()));
            response.xselection.property = property;
        }
        XSendEvent(display, request.requestor, False, 0, &response);
        XFlush(display);
    }
    void event(XEvent &event) {
        switch (event.type) {
        case MappingNotify:
            XRefreshKeyboardMapping(&event.xmapping);
            break;
        case Expose:
            dirty = full_damage = true;
            break;
        case ConfigureNotify:
            resize(event.xconfigure.width, event.xconfigure.height);
            break;
        case MapNotify:
            mapped = true;
            dirty = full_damage = true;
            break;
        case UnmapNotify:
            mapped = false;
            break;
        case ClientMessage:
            if (event.xclient.message_type == wm_protocols &&
                static_cast<Atom>(event.xclient.data.l[0]) == delete_window && callbacks.close)
                callbacks.close();
            break;
        case FocusIn:
            if (input_context)
                XSetICFocus(input_context);
            if (callbacks.focus)
                callbacks.focus(true);
            break;
        case FocusOut:
            if (input_context)
                XUnsetICFocus(input_context);
            if (callbacks.focus)
                callbacks.focus(false);
            break;
        case KeyPress:
            key(event.xkey, true);
            break;
        case KeyRelease:
            key(event.xkey, false);
            break;
        case SelectionRequest:
            selection_request(event.xselectionrequest);
            break;
        case SelectionNotify: {
            if (!paste_callback)
                break;
            if (event.xselection.property == atom("AURA_PASTE"))
                read_paste_property(true);
            else if (event.xselection.property == None)
                finish_paste(false);
            break;
        }
        case PropertyNotify:
            if (paste_callback && paste_incremental && event.xproperty.atom == atom("AURA_PASTE") &&
                event.xproperty.state == PropertyNewValue)
                read_paste_property(false);
            break;
        case MotionNotify:
            if (callbacks.pointer)
                callbacks.pointer({PointerInput::Move,
                                   mouse(event.xmotion.x, event.xmotion.y, event.xmotion.state)});
            break;
        case LeaveNotify:
            if (callbacks.pointer)
                callbacks.pointer({PointerInput::Leave, mouse(event.xcrossing.x, event.xcrossing.y,
                                                              event.xcrossing.state)});
            break;
        case ButtonPress:
        case ButtonRelease: {
            const bool down = event.type == ButtonPress;
            if (down && chrome_click(event.xbutton))
                break;
            if (!callbacks.pointer)
                break;
            const auto button = event.xbutton.button;
            PointerInput p{down ? PointerInput::Down : PointerInput::Up,
                           mouse(event.xbutton.x, event.xbutton.y, event.xbutton.state)};
            p.button = button;
            if (button >= 4 && button <= 7) {
                if (!down)
                    break;
                p.kind = PointerInput::Wheel;
                p.dx = button == 6 ? 120 : button == 7 ? -120 : 0;
                p.dy = button == 4 ? 120 : button == 5 ? -120 : 0;
            } else if (button >= 1 && button <= 3) {
                p.clicks = down && button == last_button && event.xbutton.time &&
                                   event.xbutton.time - last_click < 400
                               ? 2
                               : 1;
                if (down) {
                    last_click = event.xbutton.time;
                    last_button = button;
                }
            } else if (button == 8 || button == 9) {
                if (down) {
                    CefKeyEvent key;
                    key.type = KEYEVENT_RAWKEYDOWN;
                    key.windows_key_code = button == 8 ? 0x25 : 0x27;
                    key.modifiers = EVENTFLAG_ALT_DOWN;
                    if (callbacks.key)
                        callbacks.key(key);
                }
                break;
            } else
                break;
            callbacks.pointer(p);
            break;
        }
        }
    }
};

BrowserWindow::BrowserWindow(int width, int height, double scale, WindowCallbacks callbacks,
                             bool accelerated, double interface_zoom)
    : impl_(std::make_unique<Impl>(scale, std::move(callbacks))) {
    auto &p = *impl_;
    p.requested_zoom = interface_zoom;
    XInitThreads();
    std::setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    p.display = XOpenDisplay(nullptr);
    if (!p.display)
        throw std::runtime_error(
            "Cannot open X11 display. In a Wayland session enable XWayland (DISPLAY).");
    const auto screen = DefaultScreen(p.display);
    const int pixel_width = static_cast<int>(std::lround(width * scale)),
              pixel_height = static_cast<int>(std::lround(height * scale));
    p.window = XCreateSimpleWindow(p.display, RootWindow(p.display, screen), 60, 60,
                                   static_cast<unsigned>(pixel_width),
                                   static_cast<unsigned>(pixel_height), 0, 0, 0);
    if (!p.window)
        throw std::runtime_error("X11 window creation failed");
    p.gc = XCreateGC(p.display, p.window, 0, nullptr);
    p.software = std::make_unique<SoftwarePresenter>(p.display, p.window, p.gc);
    const long event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask | KeyPressMask |
                            KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | LeaveWindowMask | PropertyChangeMask;
    XSelectInput(p.display, p.window, event_mask);
    XStoreName(p.display, p.window, "Aura");
    XClassHint hint{const_cast<char *>("aura2"), const_cast<char *>("Aura2")};
    XSetClassHint(p.display, p.window, &hint);
    // EWMH icon pixels are CARDINALs, stored as native unsigned longs by Xlib.
    std::vector<unsigned long> brand_icon{64, 64};
    for (const auto pixel : brand::argb)
        brand_icon.push_back(pixel);
    XChangeProperty(p.display, p.window, p.atom("_NET_WM_ICON"), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(brand_icon.data()),
                    static_cast<int>(brand_icon.size()));
    const unsigned long pid = static_cast<unsigned long>(getpid());
    XChangeProperty(p.display, p.window, p.atom("_NET_WM_PID"), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&pid), 1);
    const Atom window_type = p.atom("_NET_WM_WINDOW_TYPE_NORMAL");
    XChangeProperty(p.display, p.window, p.atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(&window_type), 1);
    struct MotifHints {
        unsigned long flags, functions, decorations;
        long input_mode;
        unsigned long status;
    } hints{2, 0, 0, 0, 0};
    XChangeProperty(p.display, p.window, p.atom("_MOTIF_WM_HINTS"), p.atom("_MOTIF_WM_HINTS"), 32,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(&hints), 5);
    XSizeHints sizes{};
    sizes.flags = PMinSize;
    sizes.min_width = static_cast<int>(900 * scale * .75);
    sizes.min_height = static_cast<int>(620 * scale * .75);
    XSetWMNormalHints(p.display, p.window, &sizes);
    p.wm_protocols = p.atom("WM_PROTOCOLS");
    p.delete_window = p.atom("WM_DELETE_WINDOW");
    XSetWMProtocols(p.display, p.window, &p.delete_window, 1);
    p.input_method = XOpenIM(p.display, nullptr, nullptr, nullptr);
    if (p.input_method) {
        XPoint spot{0, 0};
        auto attributes = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
        p.input_context = XCreateIC(
            p.input_method, XNInputStyle, XIMPreeditPosition | XIMStatusNothing, XNClientWindow,
            p.window, XNFocusWindow, p.window, XNPreeditAttributes, attributes, nullptr);
        if (attributes)
            XFree(attributes);
        if (!p.input_context)
            p.input_context =
                XCreateIC(p.input_method, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, p.window, XNFocusWindow, p.window, nullptr);
    }
    p.composition = "Skia raster / X11";
    p.graphics_renderer = "CPU / software rendering";
    if (accelerated) {
        try {
            p.initialize_graphics();
        } catch (const std::exception &error) {
            spdlog::warn("EGL unavailable; using Skia raster presentation: {}", error.what());
            p.reset_graphics();
            p.composition = "Skia raster / X11";
            p.graphics_renderer = "raster fallback";
        }
    }
    p.resize(pixel_width, pixel_height);
    if (std::getenv("AURA_BACKGROUND_TEST")) {
        const unsigned long timestamp = 0;
        XChangeProperty(p.display, p.window, p.atom("_NET_WM_USER_TIME"), XA_CARDINAL, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(&timestamp), 1);
    }
    XFlush(p.display);
}
void BrowserWindow::interface_zoom(double factor) {
    auto &p = *impl_;
    if (p.requested_zoom == factor)
        return;
    p.requested_zoom = factor;
    p.resize(p.pixel_width, p.pixel_height);
}
BrowserWindow::~BrowserWindow() = default;
void BrowserWindow::hide() {
    XUnmapWindow(impl_->display, impl_->window);
    XFlush(impl_->display);
}
void BrowserWindow::show() {
    XMapWindow(impl_->display, impl_->window);
    XFlush(impl_->display);
    // Present the initial buffer even when the Wayland compositor defers MapNotify.
    impl_->mapped = true;
    impl_->dirty = impl_->full_damage = true;
}
void BrowserWindow::activate() {
    XMapRaised(impl_->display, impl_->window);
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = impl_->window;
    event.xclient.message_type = XInternAtom(impl_->display, "_NET_ACTIVE_WINDOW", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(impl_->display, DefaultRootWindow(impl_->display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(impl_->display);
}
CefWindowHandle BrowserWindow::handle() const {
    return impl_->window;
}
std::uintptr_t BrowserWindow::native_id() const {
    return handle();
}
void BrowserWindow::invalidate() {
    impl_->dirty = impl_->full_damage = true;
}
void BrowserWindow::dispatch() {
    while (XPending(impl_->display)) {
        XEvent e{};
        XNextEvent(impl_->display, &e);
        if (impl_->software->event(e))
            continue;
        if (!XFilterEvent(&e, impl_->window))
            impl_->event(e);
    }
    if (impl_->paste_callback && std::chrono::steady_clock::now() > impl_->paste_deadline)
        impl_->finish_paste(false);
}
void BrowserWindow::paint() {
    impl_->paint();
}
void BrowserWindow::wait(int milliseconds) {
    if (XPending(impl_->display))
        return;
    pollfd fds[] = {{ConnectionNumber(impl_->display), POLLIN, 0}, {impl_->wake_fd, POLLIN, 0}};
    poll(fds, 2, milliseconds);
    if (fds[1].revents & POLLIN) {
        std::uint64_t value;
        while (read(impl_->wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
    }
}
void BrowserWindow::invalidate(Rect damage) {
    impl_->invalidate(damage);
}
int BrowserWindow::refresh_rate() {
    return impl_->software->refresh_rate();
}
void BrowserWindow::wake() {
    const std::uint64_t value = 1;
    while (write(impl_->wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
}
std::string BrowserWindow::presentation_stats() const {
    const auto &p = *impl_;
    const auto percentile = [&](std::array<double, 240> costs) {
        std::sort(costs.begin(), costs.begin() + p.paint_cost_count);
        return p.paint_cost_count ? costs[(p.paint_cost_count - 1) * 95 / 100] : 0;
    };
    const auto p95 = percentile(p.paint_costs);
    std::ostringstream out;
    out << "{\"paint_ms_p95\":" << p95
        << ",\"draw_ms_p95\":" << percentile(p.draw_costs)
        << ",\"upload_ms_p95\":" << percentile(p.upload_costs)
        << ",\"raster_allocations\":" << p.raster_allocations
        << ",\"content_frames\":" << p.content_frames
        << ",\"presented_pixels\":" << p.presented_pixels << ",\"display\":" << p.software->report()
        << "}";
    return out.str();
}
void BrowserWindow::title(const std::string &text) {
    auto &p = *impl_;
    XStoreName(p.display, p.window, text.c_str());
    XChangeProperty(p.display, p.window, p.atom("_NET_WM_NAME"), p.atom("UTF8_STRING"), 8,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(text.data()),
                    static_cast<int>(text.size()));
}
void BrowserWindow::caret(int x, int y) {
    auto &p = *impl_;
    if (!p.input_context)
        return;
    XPoint spot{static_cast<short>(x * p.scale), static_cast<short>(y * p.scale)};
    auto list = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
    if (list) {
        XSetICValues(p.input_context, XNPreeditAttributes, list, nullptr);
        XFree(list);
    }
}
void BrowserWindow::cursor(int kind) {
    auto &p = *impl_;
    kind = std::clamp(kind, 0, 2);
    if (kind == p.current_cursor)
        return;
    if (!p.cursors[kind])
        p.cursors[kind] = XCreateFontCursor(p.display, kind == 1   ? XC_xterm
                                                       : kind == 2 ? XC_hand2
                                                                   : XC_left_ptr);
    XDefineCursor(p.display, p.window, p.cursors[kind]);
    p.current_cursor = kind;
}
void BrowserWindow::copy(const std::string &text) {
    auto &p = *impl_;
    p.clipboard = text;
    XSetSelectionOwner(p.display, p.atom("CLIPBOARD"), p.window, CurrentTime);
    XFlush(p.display);
}
void BrowserWindow::paste(std::function<void(std::string)> callback) {
    auto &p = *impl_;
    if (XGetSelectionOwner(p.display, p.atom("CLIPBOARD")) == p.window) {
        callback(p.clipboard);
        return;
    }
    p.paste_callback = std::move(callback);
    p.paste_bytes.clear();
    p.paste_incremental = false;
    p.paste_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    XDeleteProperty(p.display, p.window, p.atom("AURA_PASTE"));
    XConvertSelection(p.display, p.atom("CLIPBOARD"), p.atom("UTF8_STRING"), p.atom("AURA_PASTE"),
                      p.window, CurrentTime);
    XFlush(p.display);
}
std::string BrowserWindow::compositor() const {
    return impl_->composition;
}
std::string BrowserWindow::renderer() const {
    return impl_->graphics_renderer;
}
std::uint64_t BrowserWindow::frames() const {
    return impl_->frame_count;
}
} // namespace aura
