#include "platform/browser_window.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dwmapi.h>
#include <exception>
#include <imm.h>
#include <stdexcept>
#include <utility>
#include <windows.h>
#include <windowsx.h>

namespace aura {
namespace {
uint32_t modifiers() {
    uint32_t result = 0;
    for (auto [key, flag] : {std::pair{VK_SHIFT, EVENTFLAG_SHIFT_DOWN},
                             {VK_CONTROL, EVENTFLAG_CONTROL_DOWN},
                             {VK_MENU, EVENTFLAG_ALT_DOWN},
                             {VK_LBUTTON, EVENTFLAG_LEFT_MOUSE_BUTTON},
                             {VK_MBUTTON, EVENTFLAG_MIDDLE_MOUSE_BUTTON},
                             {VK_RBUTTON, EVENTFLAG_RIGHT_MOUSE_BUTTON}})
        if (GetKeyState(key) & 0x8000)
            result |= flag;
    if ((GetKeyState(VK_RMENU) & 0x8000) && (GetKeyState(VK_CONTROL) & 0x8000))
        result |= EVENTFLAG_ALTGR_DOWN;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
        result |= EVENTFLAG_COMMAND_DOWN;
    if (GetKeyState(VK_CAPITAL) & 1)
        result |= EVENTFLAG_CAPS_LOCK_ON;
    if (GetKeyState(VK_NUMLOCK) & 1)
        result |= EVENTFLAG_NUM_LOCK_ON;
    return result;
}
} // namespace
struct BrowserWindow::Impl {
    HWND window{};
    HANDLE event{};
    HDC memory{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    sk_sp<SkSurface> surface;
    WindowCallbacks callbacks;
    std::exception_ptr failure;
    int pixel_width = 0, pixel_height = 0, width = 0, height = 0, hz = 60, cursor_kind = 0;
    double cli_scale = 1, base_scale = 1, scale = 1, zoom = 1;
    bool dirty = true, full = true, tracking = false, suppress_character = false;
    wchar_t surrogate = 0;
    uint64_t frame_count = 0, allocations = 0;
    Impl(int w, int h, double factor, WindowCallbacks hooks, double interface_zoom)
        : callbacks(std::move(hooks)), cli_scale(factor), zoom(interface_zoom) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("Cannot create window wake event");
        WNDCLASSEXW type{sizeof(type)};
        type.style = CS_DBLCLKS;
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.hIcon = static_cast<HICON>(LoadImageW(type.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXICON),
                                                   GetSystemMetrics(SM_CYICON), LR_SHARED));
        type.hIconSm = static_cast<HICON>(LoadImageW(type.hInstance, MAKEINTRESOURCEW(1),
                                                     IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                     GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.lpszClassName = L"AuraBrowserWindow";
        RegisterClassExW(&type);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const double dpi = GetDpiForSystem() / 96.0 * factor;
        window = CreateWindowExW(0, type.lpszClassName, L"Aura", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, std::min(int(w * dpi), int(work.right - work.left)),
                                 std::min(int(h * dpi), int(work.bottom - work.top)), nullptr,
                                 nullptr, type.hInstance, this);
        if (!window) {
            CloseHandle(event);
            throw std::runtime_error("Cannot create browser window");
        }
        memory = CreateCompatibleDC(nullptr);
        if (!memory)
            throw std::runtime_error("Cannot create raster context");
        resize();
    }
    ~Impl() {
        callbacks = {}; // The controller UI has already been destroyed.
        release_surface();
        if (memory)
            DeleteDC(memory);
        if (window)
            DestroyWindow(window);
        if (event)
            CloseHandle(event);
    }
    void release_surface() {
        surface.reset();
        if (bitmap) {
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
    }
    void refresh() {
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency >= 30 && mode.dmDisplayFrequency <= 360)
            hz = int(mode.dmDisplayFrequency);
    }
    void resize() {
        if (!window || IsIconic(window))
            return;
        RECT client{};
        GetClientRect(window, &client);
        const int w = std::max(1, int(client.right)), h = std::max(1, int(client.bottom));
        base_scale = GetDpiForWindow(window) / 96.0 * cli_scale;
        scale = std::max(0.1, std::min({base_scale * zoom, w / 900.0, h / 620.0}));
        width = std::max(1, int(std::lround(w / scale)));
        height = std::max(1, int(std::lround(h / scale)));
        if (w != pixel_width || h != pixel_height) {
            release_surface();
            pixel_width = w;
            pixel_height = h;
        }
        if (callbacks.resize)
            callbacks.resize(width, height, scale / base_scale, base_scale);
        dirty = full = true;
        refresh();
    }
    void paint() {
        if (!dirty || !IsWindowVisible(window) || IsIconic(window))
            return;
        if (!surface) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = pixel_width;
            info.bmiHeader.biHeight = -pixel_height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void *pixels = nullptr;
            bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap || !pixels)
                throw std::runtime_error("Cannot allocate raster frame");
            previous = SelectObject(memory, bitmap);
            surface = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(pixel_width, pixel_height),
                                             pixels, size_t(pixel_width) * 4);
            if (!surface)
                throw std::runtime_error("Cannot wrap raster frame");
            ++allocations;
            full = true;
        }
        auto &canvas = *surface->getCanvas();
        canvas.save();
        canvas.scale(float(scale), float(scale));
        if (full || !callbacks.draw_content || !callbacks.draw_content(canvas))
            if (callbacks.draw)
                callbacks.draw(canvas);
        canvas.restore();
        HDC destination = GetDC(window);
        if (destination) {
            BitBlt(destination, 0, 0, pixel_width, pixel_height, memory, 0, 0, SRCCOPY);
            GdiFlush();
            ReleaseDC(window, destination);
        }
        ValidateRect(window, nullptr);
        dirty = full = false;
        ++frame_count;
    }
    CefMouseEvent point(int x, int y) {
        CefMouseEvent p;
        p.x = int(std::lround(x / scale));
        p.y = int(std::lround(y / scale));
        p.modifiers = modifiers();
        return p;
    }
    void text(const std::wstring &value) {
        if (callbacks.text && !value.empty())
            callbacks.text(CefString(value).ToString());
    }
    static LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wp, LPARAM lp) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            self->window = handle;
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            try {
                return self->message(message, wp, lp);
            } catch (...) {
                self->failure = std::current_exception();
                SetEvent(self->event);
                return 0;
            }
        }
        return DefWindowProcW(handle, message, wp, lp);
    }
    LRESULT message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_NCCALCSIZE:
            if (wp)
                return 0;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT state{};
            BeginPaint(window, &state);
            EndPaint(window, &state);
            dirty = full = true;
            return 0;
        }
        case WM_CLOSE:
            if (callbacks.close)
                callbacks.close();
            return 0;
        case WM_SIZE:
            resize();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_MOVE:
            refresh();
            break;
        case WM_DPICHANGED: {
            auto r = reinterpret_cast<RECT *>(lp);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            resize();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO *>(lp);
            auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(monitor, &mi);
            info->ptMaxPosition = {mi.rcWork.left - mi.rcMonitor.left,
                                   mi.rcWork.top - mi.rcMonitor.top};
            info->ptMaxSize = {mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
            double dpi = GetDpiForWindow(window) / 96.0;
            info->ptMinTrackSize = {LONG(675 * dpi), LONG(465 * dpi)};
            return 0;
        }
        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(window, &p);
            int border = std::max(4, int(6 * base_scale));
            if (!IsZoomed(window)) {
                bool l = p.x < border, r = p.x >= pixel_width - border, t = p.y < border,
                     b = p.y >= pixel_height - border;
                if (t && l)
                    return HTTOPLEFT;
                if (t && r)
                    return HTTOPRIGHT;
                if (b && l)
                    return HTBOTTOMLEFT;
                if (b && r)
                    return HTBOTTOMRIGHT;
                if (l)
                    return HTLEFT;
                if (r)
                    return HTRIGHT;
                if (t)
                    return HTTOP;
                if (b)
                    return HTBOTTOM;
            }
            if (p.y < 30 * scale && p.x >= 240 * scale && p.x < (width - 144) * scale)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            if (callbacks.focus)
                callbacks.focus(message == WM_SETFOCUS);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, cursor_kind == 1   ? IDC_IBEAM
                                               : cursor_kind == 2 ? IDC_HAND
                                                                  : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            tracking = false;
            if (callbacks.pointer)
                callbacks.pointer({PointerInput::Leave, point(-1, -1)});
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: {
            auto p = point(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            p.modifiers &= ~(EVENTFLAG_LEFT_MOUSE_BUTTON | EVENTFLAG_MIDDLE_MOUSE_BUTTON |
                             EVENTFLAG_RIGHT_MOUSE_BUTTON);
            if (wp & MK_LBUTTON)
                p.modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
            if (wp & MK_MBUTTON)
                p.modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
            if (wp & MK_RBUTTON)
                p.modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
            bool move = message == WM_MOUSEMOVE;
            bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;
            if (!move && !up) {
                SetFocus(window);
                SetCapture(window);
            }
            if (up && !(wp & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)))
                ReleaseCapture();
            if (!tracking) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
                tracking = true;
            }
            if (message == WM_LBUTTONDOWN && p.y < 30 && p.x >= width - 144) {
                if (p.x >= width - 48) {
                    if (callbacks.close)
                        callbacks.close();
                } else
                    ShowWindow(window, p.x >= width - 96
                                           ? (IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE)
                                           : SW_MINIMIZE);
                return 0;
            }
            int button = (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
                          message == WM_RBUTTONDBLCLK)
                             ? 3
                         : (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
                            message == WM_MBUTTONDBLCLK)
                             ? 2
                             : 1;
            bool twice = message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
                         message == WM_MBUTTONDBLCLK;
            if (callbacks.pointer)
                callbacks.pointer({move ? PointerInput::Move
                                   : up ? PointerInput::Up
                                        : PointerInput::Down,
                                   p, button, 0, 0, twice ? 2 : 1});
            return 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(window, &p);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (callbacks.pointer)
                callbacks.pointer({PointerInput::Wheel, point(p.x, p.y), 1,
                                   message == WM_MOUSEHWHEEL ? delta : 0,
                                   message == WM_MOUSEWHEEL ? delta : 0});
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            CefKeyEvent key;
            key.type = (message == WM_KEYUP || message == WM_SYSKEYUP) ? KEYEVENT_KEYUP
                                                                       : KEYEVENT_RAWKEYDOWN;
            key.windows_key_code = int(wp);
            key.native_key_code = int(lp);
            key.modifiers = modifiers();
            key.is_system_key = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
            const bool handled = callbacks.key && callbacks.key(key);
            if (key.type == KEYEVENT_RAWKEYDOWN)
                suppress_character = handled;
            if (handled)
                return 0;
            if (!key.is_system_key)
                return 0;
            break;
        }
        case WM_CHAR: {
            wchar_t c = wchar_t(wp);
            if (c >= 0xD800 && c <= 0xDBFF) {
                surrogate = c;
                return 0;
            }
            if (!suppress_character && (c >= 32 || c == 8 || c == 9 || c == 13)) {
                std::wstring value;
                if (c >= 0xDC00 && c <= 0xDFFF && surrogate)
                    value += surrogate;
                value += c;
                text(value);
            }
            surrogate = 0;
            return 0;
        }
        case WM_IME_COMPOSITION:
            if (lp & GCS_RESULTSTR) {
                auto context = ImmGetContext(window);
                if (context) {
                    auto size = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                    if (size > 0) {
                        std::wstring value(size / sizeof(wchar_t), L'\0');
                        ImmGetCompositionStringW(context, GCS_RESULTSTR, value.data(), size);
                        text(value);
                    }
                    ImmReleaseContext(window, context);
                }
                return 0;
            }
            break;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};
BrowserWindow::BrowserWindow(int w, int h, double scale, WindowCallbacks hooks, bool, double zoom)
    : impl_(std::make_unique<Impl>(w, h, scale, std::move(hooks), zoom)) {}
BrowserWindow::~BrowserWindow() = default;
CefWindowHandle BrowserWindow::handle() const {
    return impl_->window;
}
std::uintptr_t BrowserWindow::native_id() const {
    return reinterpret_cast<std::uintptr_t>(handle());
}
void BrowserWindow::hide() {
    ShowWindow(handle(), SW_HIDE);
}
void BrowserWindow::show() {
    ShowWindow(handle(), SW_SHOW);
    UpdateWindow(handle());
}
void BrowserWindow::dispatch() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (impl_->failure)
            std::rethrow_exception(std::exchange(impl_->failure, {}));
    }
    // Another window's dispatch() can drain this thread's messages first.
    if (impl_->failure)
        std::rethrow_exception(std::exchange(impl_->failure, {}));
}
void BrowserWindow::paint() {
    impl_->paint();
}
void BrowserWindow::wait(int ms) {
    MsgWaitForMultipleObjectsEx(1, &impl_->event, DWORD(std::max(0, ms)), QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
}
void BrowserWindow::invalidate() {
    impl_->dirty = impl_->full = true;
    wake();
}
void BrowserWindow::invalidate(Rect) {
    impl_->dirty = true;
    wake();
}
void BrowserWindow::wake() {
    SetEvent(impl_->event);
}
int BrowserWindow::refresh_rate() {
    impl_->refresh();
    return impl_->hz;
}
std::string BrowserWindow::presentation_stats() const {
    BOOL composed = FALSE;
    DwmIsCompositionEnabled(&composed);
    return "{\"refresh_hz\":" + std::to_string(impl_->hz) +
           ",\"synchronized\":" + (composed ? "true" : "false") +
           ",\"raster_allocations\":" + std::to_string(impl_->allocations) + "}";
}
void BrowserWindow::interface_zoom(double factor) {
    impl_->zoom = factor;
    impl_->resize();
    wake();
}
void BrowserWindow::title(const std::string &value) {
    SetWindowTextW(handle(), CefString(value).ToWString().c_str());
}
void BrowserWindow::caret(int x, int y) {
    auto context = ImmGetContext(handle());
    if (context) {
        COMPOSITIONFORM position{CFS_POINT, {LONG(x * impl_->scale), LONG(y * impl_->scale)}, {}};
        ImmSetCompositionWindow(context, &position);
        ImmReleaseContext(handle(), context);
    }
}
void BrowserWindow::cursor(int kind) {
    impl_->cursor_kind = kind;
    SetCursor(LoadCursorW(nullptr, kind == 1 ? IDC_IBEAM : kind == 2 ? IDC_HAND : IDC_ARROW));
}
void BrowserWindow::copy(const std::string &value) {
    const auto text = CefString(value).ToWString();
    if (!OpenClipboard(handle()))
        return;
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (data) {
        auto destination = GlobalLock(data);
        if (destination) {
            memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(data);
            if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, data))
                GlobalFree(data);
        } else
            GlobalFree(data);
    }
    CloseClipboard();
}
void BrowserWindow::paste(std::function<void(std::string)> callback) {
    std::string value;
    if (OpenClipboard(handle())) {
        if (auto data = GetClipboardData(CF_UNICODETEXT)) {
            if (auto text = static_cast<const wchar_t *>(GlobalLock(data))) {
                value = CefString(text).ToString();
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }
    callback(value);
}
std::string BrowserWindow::compositor() const {
    return "Win32 / DWM";
}
std::string BrowserWindow::renderer() const {
    return "Skia raster / persistent DIB";
}
std::uint64_t BrowserWindow::frames() const {
    return impl_->frame_count;
}
} // namespace aura
