#include "platform/linux/x11/diagnostic_window.hpp"
#include "aura/design_tokens.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <spdlog/spdlog.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <clocale>
#include <cstring>
#include <stdexcept>

namespace aura {
namespace {
int cef_modifiers(unsigned mask) {
    int flags = 0;
    if (mask & ShiftMask) flags |= EVENTFLAG_SHIFT_DOWN;
    if (mask & ControlMask) flags |= EVENTFLAG_CONTROL_DOWN;
    if (mask & Mod1Mask) flags |= EVENTFLAG_ALT_DOWN;
    if (mask & Mod4Mask) flags |= EVENTFLAG_COMMAND_DOWN;
    if (mask & LockMask) flags |= EVENTFLAG_CAPS_LOCK_ON;
    if (mask & Button1Mask) flags |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (mask & Button2Mask) flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (mask & Button3Mask) flags |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return flags;
}
int windows_key(KeySym key) {
    if (key >= XK_a && key <= XK_z) return static_cast<int>(key - XK_a + 'A');
    if (key >= XK_A && key <= XK_Z) return static_cast<int>(key);
    if (key >= XK_0 && key <= XK_9) return static_cast<int>(key);
    if (key >= XK_F1 && key <= XK_F24) return static_cast<int>(key - XK_F1 + 0x70);
    switch (key) {
    case XK_BackSpace: return 0x08; case XK_Tab: case XK_ISO_Left_Tab: return 0x09;
    case XK_Return: case XK_KP_Enter: return 0x0D; case XK_Escape: return 0x1B;
    case XK_space: return 0x20; case XK_Page_Up: return 0x21; case XK_Page_Down: return 0x22;
    case XK_End: return 0x23; case XK_Home: return 0x24; case XK_Left: return 0x25;
    case XK_Up: return 0x26; case XK_Right: return 0x27; case XK_Down: return 0x28;
    case XK_Insert: return 0x2D; case XK_Delete: return 0x2E;
    case XK_Shift_L: case XK_Shift_R: return 0x10;
    case XK_Control_L: case XK_Control_R: return 0x11;
    case XK_Alt_L: case XK_Alt_R: return 0x12;
    case XK_semicolon: return 0xBA; case XK_equal: return 0xBB; case XK_comma: return 0xBC;
    case XK_minus: return 0xBD; case XK_period: return 0xBE; case XK_slash: return 0xBF;
    case XK_grave: return 0xC0; case XK_bracketleft: return 0xDB;
    case XK_backslash: return 0xDC; case XK_bracketright: return 0xDD; case XK_apostrophe: return 0xDE;
    default: return 0;
    }
}
}

struct LinuxDiagnosticWindow::Impl {
    DiagnosticScene& scene;
    DiagnosticState& state;
    Display* display{};
    ::Window window{};
    GC gc{};
    Atom delete_window{}, wm_protocols{};
    XIM input_method{};
    XIC input_context{};
    CefRefPtr<DiagnosticClient> client;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLContext egl_context = EGL_NO_CONTEXT;
    sk_sp<GrDirectContext> gpu;
    sk_sp<SkSurface> surface;
    int surface_width{}, surface_height{}, stencil_bits{};
    int pixel_width{}, pixel_height{}, width{}, height{};
    unsigned selected_theme{};
    bool dirty = true, close_requested = false, mapped = false, web_focus = false;
    Time last_click{};
    unsigned last_button{};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    Impl(DiagnosticScene& scene_value, DiagnosticState& state_value)
        : scene(scene_value), state(state_value) {}
    ~Impl() {
        state.invalidate = {};
        state.ime_caret = {};
        client = nullptr;
        reset_graphics();
        if (input_context) XDestroyIC(input_context);
        if (input_method) XCloseIM(input_method);
        if (gc) XFreeGC(display, gc);
        if (window) XDestroyWindow(display, window);
        if (display) XCloseDisplay(display);
    }
    void reset_graphics() {
        surface.reset();
        if (gpu) gpu->releaseResourcesAndAbandonContext();
        gpu.reset();
        if (egl_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
            if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
            eglTerminate(egl_display);
        }
        egl_display = EGL_NO_DISPLAY;
        egl_surface = EGL_NO_SURFACE;
        egl_context = EGL_NO_CONTEXT;
    }
    Atom atom(const char* name) { return XInternAtom(display, name, False); }
    void initialize_graphics() {
        egl_display = eglGetDisplay(display);
        if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, nullptr, nullptr))
            throw std::runtime_error("EGL display initialization failed");
        if (!eglBindAPI(EGL_OPENGL_ES_API)) throw std::runtime_error("EGL GLES API unavailable");
        const EGLint attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_STENCIL_SIZE, 8,
            EGL_NATIVE_VISUAL_ID, static_cast<EGLint>(XVisualIDFromVisual(DefaultVisual(display, DefaultScreen(display)))),
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
        auto interface = GrGLMakeAssembledGLESInterface(nullptr,
            [](void*, const char* name) -> GrGLFuncPtr {
                return reinterpret_cast<GrGLFuncPtr>(eglGetProcAddress(name));
            });
        gpu = GrDirectContexts::MakeGL(std::move(interface));
        if (!gpu) throw std::runtime_error("Skia Ganesh GLES initialization failed");
        eglGetConfigAttrib(egl_display, config, EGL_STENCIL_SIZE, &stencil_bits);
        eglSwapInterval(egl_display, 1);
        state.compositor = "Skia / EGL / GLES3";
        const auto* renderer = glGetString(GL_RENDERER);
        state.graphics_renderer = renderer ? reinterpret_cast<const char*>(renderer) : "unknown GLES renderer";
        spdlog::info("Linux graphics: {} / {}", state.compositor, state.graphics_renderer);
    }
    void resize(int w, int h) {
        if (w <= 0 || h <= 0) return;
        pixel_width = w; pixel_height = h;
        width = std::max(1, static_cast<int>(std::lround(w / state.scale)));
        height = std::max(1, static_cast<int>(std::lround(h / state.scale)));
        const auto bounds = DiagnosticScene::web_bounds(width, height);
        state.view_width = bounds.width;
        state.view_height = bounds.height;
        if (client && client->browser()) {
            client->browser()->GetHost()->NotifyScreenInfoChanged();
            client->browser()->GetHost()->WasResized();
        }
        dirty = true;
    }
    void update_ime(const CefRect& caret) {
        if (!input_context) return;
        const auto bounds = DiagnosticScene::web_bounds(width, height);
        XPoint spot{static_cast<short>((bounds.x + caret.x) * state.scale),
                    static_cast<short>((bounds.y + caret.y + caret.height) * state.scale)};
        auto nested = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
        if (nested) { XSetICValues(input_context, XNPreeditAttributes, nested, nullptr); XFree(nested); }
    }
    void draw(SkCanvas& canvas) {
        canvas.save();
        canvas.scale(static_cast<float>(state.scale), static_cast<float>(state.scale));
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        scene.draw(canvas, width, height, state, selected_theme, elapsed);
        SkPaint ink;
        ink.setAntiAlias(true);
        ink.setColor(design::themes[selected_theme].text);
        ink.setStrokeWidth(1.3f);
        const auto right = static_cast<float>(width);
        canvas.drawLine(right-122, 33, right-110, 33, ink);
        ink.setStyle(SkPaint::kStroke_Style);
        canvas.drawRect(SkRect::MakeXYWH(right-78, 27, 10, 10), ink);
        canvas.drawLine(right-34, 27, right-24, 37, ink);
        canvas.drawLine(right-24, 27, right-34, 37, ink);
        canvas.restore();
    }
    void paint() {
        if (!dirty || !mapped || pixel_width <= 0 || pixel_height <= 0) return;
        if (gpu) {
            try {
                if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
                    throw std::runtime_error("EGL context activation failed");
                if (!surface || surface_width != pixel_width || surface_height != pixel_height) {
                    surface.reset();
                    const GrGLFramebufferInfo info{0, GL_RGBA8};
                    const auto target = GrBackendRenderTargets::MakeGL(pixel_width, pixel_height, 0, stencil_bits, info);
                    surface = SkSurfaces::WrapBackendRenderTarget(gpu.get(), target,
                        kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
                    if (!surface) throw std::runtime_error("Skia EGL render target wrapping failed");
                    surface_width = pixel_width; surface_height = pixel_height;
                }
                draw(*surface->getCanvas());
                gpu->flushAndSubmit();
                if (!eglSwapBuffers(egl_display, egl_surface)) throw std::runtime_error("eglSwapBuffers failed");
                ++state.presentation_frames;
                dirty = false;
                return;
            } catch (const std::exception& error) {
                spdlog::warn("Linux GPU presentation failed; falling back to raster: {}", error.what());
                reset_graphics();
                state.compositor = "Skia raster / X11";
                state.graphics_renderer = "raster fallback";
            }
        }
        auto raster = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(pixel_width, pixel_height));
        if (!raster) throw std::runtime_error("Linux raster surface allocation failed");
        draw(*raster->getCanvas());
        SkPixmap pixels;
        raster->peekPixels(&pixels);
        auto* image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
            static_cast<unsigned>(DefaultDepth(display, DefaultScreen(display))), ZPixmap, 0,
            static_cast<char*>(pixels.writable_addr()), static_cast<unsigned>(pixel_width),
            static_cast<unsigned>(pixel_height), 32, static_cast<int>(pixels.rowBytes()));
        if (!image) throw std::runtime_error("XImage allocation failed");
        XPutImage(display, window, gc, image, 0, 0, 0, 0,
                  static_cast<unsigned>(pixel_width), static_cast<unsigned>(pixel_height));
        image->data = nullptr;
        XDestroyImage(image);
        XFlush(display);
        ++state.presentation_frames;
        dirty = false;
    }
    CefMouseEvent mouse(int x, int y, unsigned mask) const {
        const auto bounds = DiagnosticScene::web_bounds(width, height);
        CefMouseEvent event;
        event.x = static_cast<int>(std::lround(x / state.scale)) - bounds.x;
        event.y = static_cast<int>(std::lround(y / state.scale)) - bounds.y;
        event.modifiers = cef_modifiers(mask);
        return event;
    }
    bool in_web(int x, int y) const {
        const auto event = mouse(x, y, 0);
        return event.x >= 0 && event.y >= 0 && event.x < state.view_width && event.y < state.view_height;
    }
    void wm_message(Atom type, long first, long second, long third, long fourth = 0, long fifth = 0) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = type;
        event.xclient.format = 32;
        event.xclient.data.l[0] = first; event.xclient.data.l[1] = second;
        event.xclient.data.l[2] = third; event.xclient.data.l[3] = fourth; event.xclient.data.l[4] = fifth;
        XSendEvent(display, DefaultRootWindow(display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XFlush(display);
    }
    bool chrome_click(const XButtonEvent& button) {
        if (button.button != Button1) return false;
        const auto x = button.x / state.scale, y = button.y / state.scale;
        constexpr int border = 6;
        const bool left = x < border, right = x >= width-border;
        const bool top = y < border, bottom = y >= height-border;
        int direction = -1;
        if (top && left) direction = 0; else if (top && right) direction = 2;
        else if (bottom && right) direction = 4; else if (bottom && left) direction = 6;
        else if (top) direction = 1; else if (right) direction = 3;
        else if (bottom) direction = 5; else if (left) direction = 7;
        if (direction >= 0) {
            XUngrabPointer(display, CurrentTime);
            wm_message(atom("_NET_WM_MOVERESIZE"), button.x_root, button.y_root, direction, Button1, 1);
            return true;
        }
        if (y >= 64) return false;
        if (x > width-48) close_requested = true;
        else if (x > width-96) wm_message(atom("_NET_WM_STATE"), 2,
            static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_VERT")), static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_HORZ")), 1);
        else if (x > width-144) XIconifyWindow(display, window, DefaultScreen(display));
        else {
            XUngrabPointer(display, CurrentTime);
            wm_message(atom("_NET_WM_MOVERESIZE"), button.x_root, button.y_root, 8, Button1, 1);
        }
        return true;
    }
    void key(XKeyEvent& event, bool pressed) {
        const auto symbol = XLookupKeysym(&event, 0);
        if (pressed && symbol == XK_F6) { selected_theme = (selected_theme + 2) % 6; dirty = true; return; }
        if (pressed && symbol == XK_F7) { selected_theme ^= 1; dirty = true; return; }
        if (!client || !client->browser()) return;
        const auto host = client->browser()->GetHost();
        CefKeyEvent key_event;
        key_event.type = pressed ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
        key_event.windows_key_code = windows_key(symbol);
        key_event.native_key_code = static_cast<int>(event.keycode);
        key_event.modifiers = cef_modifiers(event.state);
        host->SendKeyEvent(key_event);
        if (!pressed || !web_focus || (event.state & (ControlMask | Mod1Mask | Mod4Mask))) return;
        std::string text;
        KeySym translated{};
        char buffer[128];
        if (input_context) {
            Status status{};
            const int count = Xutf8LookupString(input_context, &event, buffer, sizeof(buffer), &translated, &status);
            if (status == XBufferOverflow && count > 0 && count < 65536) {
                text.resize(static_cast<std::size_t>(count));
                const int actual = Xutf8LookupString(input_context, &event, text.data(), count, &translated, &status);
                text.resize(static_cast<std::size_t>(std::max(0, actual)));
            } else if (count > 0 && (status == XLookupChars || status == XLookupBoth))
                text.assign(buffer, static_cast<std::size_t>(count));
        } else {
            const int count = XLookupString(&event, buffer, sizeof(buffer), &translated, nullptr);
            if (count > 0) text.assign(buffer, static_cast<std::size_t>(count));
        }
        const auto characters = CefString(text).ToString16();
        for (const auto character : characters) {
            key_event.type = KEYEVENT_CHAR;
            key_event.character = character;
            key_event.unmodified_character = character;
            host->SendKeyEvent(key_event);
        }
    }
    void event(XEvent& event) {
        const auto browser = client ? client->browser() : nullptr;
        switch (event.type) {
        case MappingNotify: XRefreshKeyboardMapping(&event.xmapping); break;
        case Expose: dirty = true; break;
        case ConfigureNotify: resize(event.xconfigure.width, event.xconfigure.height); break;
        case MapNotify: mapped = true; dirty = true; if (browser) browser->GetHost()->WasHidden(false); break;
        case UnmapNotify: mapped = false; if (browser) browser->GetHost()->WasHidden(true); break;
        case ClientMessage:
            if (event.xclient.message_type == wm_protocols &&
                static_cast<Atom>(event.xclient.data.l[0]) == delete_window) close_requested = true;
            break;
        case FocusIn:
            web_focus = true;
            if (input_context) XSetICFocus(input_context);
            if (browser) browser->GetHost()->SetFocus(true);
            break;
        case FocusOut:
            web_focus = false;
            if (input_context) XUnsetICFocus(input_context);
            if (browser) browser->GetHost()->SetFocus(false);
            break;
        case KeyPress: key(event.xkey, true); break;
        case KeyRelease: key(event.xkey, false); break;
        case MotionNotify:
            if (browser) browser->GetHost()->SendMouseMoveEvent(mouse(event.xmotion.x, event.xmotion.y, event.xmotion.state),
                !in_web(event.xmotion.x, event.xmotion.y));
            break;
        case LeaveNotify:
            if (browser) browser->GetHost()->SendMouseMoveEvent(mouse(event.xcrossing.x, event.xcrossing.y, event.xcrossing.state), true);
            break;
        case ButtonPress: case ButtonRelease: {
            const bool pressed = event.type == ButtonPress;
            if (pressed && chrome_click(event.xbutton)) break;
            if (!browser || !in_web(event.xbutton.x, event.xbutton.y)) break;
            const auto button = event.xbutton.button;
            auto mouse_event = mouse(event.xbutton.x, event.xbutton.y, event.xbutton.state);
            if (button >= 4 && button <= 7) {
                if (pressed) browser->GetHost()->SendMouseWheelEvent(mouse_event,
                    button == 6 ? 120 : button == 7 ? -120 : 0,
                    button == 4 ? 120 : button == 5 ? -120 : 0);
                break;
            }
            if (button < 1 || button > 3) break;
            if (pressed) { web_focus = true; browser->GetHost()->SetFocus(true); }
            const bool twice = pressed && button == last_button && event.xbutton.time - last_click < 400;
            const auto type = button == Button1 ? MBT_LEFT : button == Button2 ? MBT_MIDDLE : MBT_RIGHT;
            browser->GetHost()->SendMouseClickEvent(mouse_event, type, !pressed, twice ? 2 : 1);
            if (pressed) { last_click = event.xbutton.time; last_button = button; }
            break;
        }
        }
    }
};

LinuxDiagnosticWindow::LinuxDiagnosticWindow(DiagnosticScene& scene, DiagnosticState& state,
    int width, int height, unsigned theme, bool accelerated) : impl_(std::make_unique<Impl>(scene, state)) {
    auto& p = *impl_;
    XInitThreads();
    std::setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    p.display = XOpenDisplay(nullptr);
    if (!p.display) throw std::runtime_error("Cannot open X11 display. In a Wayland session enable XWayland (DISPLAY).");
    p.selected_theme = theme;
    const auto screen = DefaultScreen(p.display);
    const int pixel_width = pixels_from_dip(width, state.scale), pixel_height = pixels_from_dip(height, state.scale);
    p.window = XCreateSimpleWindow(p.display, RootWindow(p.display, screen), 60, 60,
        static_cast<unsigned>(pixel_width), static_cast<unsigned>(pixel_height), 0, 0, 0);
    if (!p.window) throw std::runtime_error("X11 window creation failed");
    p.gc = XCreateGC(p.display, p.window, 0, nullptr);
    const long event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask | KeyPressMask |
        KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask;
    XSelectInput(p.display, p.window, event_mask);
    XStoreName(p.display, p.window, "Aura 2 / Milestone 0");
    XClassHint hint{const_cast<char*>("aura2"), const_cast<char*>("Aura2")};
    XSetClassHint(p.display, p.window, &hint);
    const unsigned long pid = static_cast<unsigned long>(getpid());
    XChangeProperty(p.display, p.window, p.atom("_NET_WM_PID"), XA_CARDINAL, 32,
        PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);
    const Atom window_type = p.atom("_NET_WM_WINDOW_TYPE_NORMAL");
    XChangeProperty(p.display, p.window, p.atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32,
        PropModeReplace, reinterpret_cast<const unsigned char*>(&window_type), 1);
    struct MotifHints { unsigned long flags, functions, decorations; long input_mode; unsigned long status; } hints{2,0,0,0,0};
    XChangeProperty(p.display, p.window, p.atom("_MOTIF_WM_HINTS"), p.atom("_MOTIF_WM_HINTS"), 32,
        PropModeReplace, reinterpret_cast<const unsigned char*>(&hints), 5);
    XSizeHints sizes{};
    sizes.flags = PMinSize;
    sizes.min_width = pixels_from_dip(900, state.scale);
    sizes.min_height = pixels_from_dip(740, state.scale);
    XSetWMNormalHints(p.display, p.window, &sizes);
    p.wm_protocols = p.atom("WM_PROTOCOLS");
    p.delete_window = p.atom("WM_DELETE_WINDOW");
    XSetWMProtocols(p.display, p.window, &p.delete_window, 1);
    p.input_method = XOpenIM(p.display, nullptr, nullptr, nullptr);
    if (p.input_method) {
        XPoint spot{0,0};
        auto attributes = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
        p.input_context = XCreateIC(p.input_method, XNInputStyle, XIMPreeditPosition | XIMStatusNothing,
            XNClientWindow, p.window, XNFocusWindow, p.window, XNPreeditAttributes, attributes, nullptr);
        if (attributes) XFree(attributes);
        if (!p.input_context) p.input_context = XCreateIC(p.input_method,
            XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, p.window, XNFocusWindow, p.window, nullptr);
    }
    state.compositor = "Skia raster / X11";
    state.graphics_renderer = "CPU / software rendering";
    if(accelerated) {
        try { p.initialize_graphics(); }
        catch (const std::exception& error) {
            spdlog::warn("EGL unavailable; using Skia raster presentation: {}",error.what());
            p.reset_graphics();
            state.compositor="Skia raster / X11";
            state.graphics_renderer="raster fallback";
        }
    }
    state.invalidate = [this] { impl_->dirty = true; };
    state.ime_caret = [this](const CefRect& caret) { impl_->update_ime(caret); };
    p.resize(pixel_width, pixel_height);
    XMapWindow(p.display, p.window);
    XFlush(p.display);
}
LinuxDiagnosticWindow::~LinuxDiagnosticWindow() = default;
unsigned long LinuxDiagnosticWindow::handle() const { return impl_->window; }
void LinuxDiagnosticWindow::attach(CefRefPtr<DiagnosticClient> client) { impl_->client = client; }
bool LinuxDiagnosticWindow::wants_close() const { return impl_->close_requested; }
unsigned LinuxDiagnosticWindow::theme() const { return impl_->selected_theme; }
void LinuxDiagnosticWindow::dispatch() {
    while (XPending(impl_->display)) {
        XEvent event{};
        XNextEvent(impl_->display, &event);
        if (!XFilterEvent(&event, impl_->window)) impl_->event(event);
    }
}
void LinuxDiagnosticWindow::render_if_needed() { impl_->paint(); }
void LinuxDiagnosticWindow::wait(int milliseconds) {
    if (XPending(impl_->display)) return;
    pollfd descriptor{ConnectionNumber(impl_->display), POLLIN, 0};
    poll(&descriptor, 1, milliseconds);
}
}
