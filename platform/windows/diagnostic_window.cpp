#include "platform/windows/diagnostic_window.hpp"
#include "aura/design_tokens.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace aura {
namespace {
int modifiers() {
    int flags = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) flags |= EVENTFLAG_SHIFT_DOWN;
    if (GetKeyState(VK_CONTROL) & 0x8000) flags |= EVENTFLAG_CONTROL_DOWN;
    if (GetKeyState(VK_MENU) & 0x8000) flags |= EVENTFLAG_ALT_DOWN;
    if (GetKeyState(VK_LBUTTON) & 0x8000) flags |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (GetKeyState(VK_RBUTTON) & 0x8000) flags |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    if (GetKeyState(VK_MBUTTON) & 0x8000) flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (GetKeyState(VK_CAPITAL) & 1) flags |= EVENTFLAG_CAPS_LOCK_ON;
    if (GetKeyState(VK_NUMLOCK) & 1) flags |= EVENTFLAG_NUM_LOCK_ON;
    return flags;
}
}
DiagnosticWindow::DiagnosticWindow(HINSTANCE instance, DiagnosticScene& scene, DiagnosticState& state,
    int width, int height, unsigned theme) : scene_(scene), state_(state), width_(width), height_(height), theme_(theme) {
    WNDCLASSEXW descriptor{sizeof(WNDCLASSEXW)};
    descriptor.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    descriptor.lpfnWndProc = procedure;
    descriptor.hInstance = instance;
    descriptor.hCursor = LoadCursor(nullptr, IDC_ARROW);
    descriptor.lpszClassName = L"Aura2DiagnosticWindow";
    if (!RegisterClassExW(&descriptor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("RegisterClassEx failed");
    state_.scale = GetDpiForSystem() / 96.0;
    window_ = CreateWindowExW(0, descriptor.lpszClassName, L"Aura 2 / Milestone 0",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        pixels_from_dip(width, state_.scale), pixels_from_dip(height, state_.scale),
        nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("CreateWindowEx failed");
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    const int corner = 2; // DWMWCP_ROUND; unsupported versions safely reject this attribute.
    DwmSetWindowAttribute(window_, 33, &corner, sizeof(corner));
    const int backdrop = 2; // DWMSBT_MAINWINDOW.
    spdlog::info("DWM backdrop HRESULT={}", DwmSetWindowAttribute(window_, 38, &backdrop, sizeof(backdrop)));
    const MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(window_, &margins);
    try {
        gpu_ = std::make_unique<AngleSurface>(window_);
        state_.compositor = "Skia / ANGLE / D3D11";
    } catch (const std::exception& error) {
        spdlog::warn("GPU surface unavailable; raster fallback: {}", error.what());
        state_.compositor = "Skia raster / fallback";
    }
    state_.invalidate = [this] { InvalidateRect(window_, nullptr, FALSE); };
    state_.ime_caret = [this](const CefRect& caret) {
        const auto bounds = DiagnosticScene::web_bounds(width_, height_);
        const POINT point{pixels_from_dip(std::max(0, bounds.x + caret.x), state_.scale),
                          pixels_from_dip(std::max(0, bounds.y + caret.y + caret.height), state_.scale)};
        if (auto context = ImmGetContext(window_)) {
            COMPOSITIONFORM form{CFS_POINT, point, {}};
            ImmSetCompositionWindow(context, &form);
            CANDIDATEFORM candidate{0, CFS_CANDIDATEPOS, point, {}};
            ImmSetCandidateWindow(context, &candidate);
            ImmReleaseContext(window_, context);
        }
    };
    resize();
    ShowWindow(window_, SW_SHOW);
}
DiagnosticWindow::~DiagnosticWindow() {
    state_.invalidate = {};
    state_.ime_caret = {};
    client_ = nullptr;
    gpu_.reset();
    if (window_) DestroyWindow(window_);
}
LRESULT CALLBACK DiagnosticWindow::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<DiagnosticWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<DiagnosticWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    try { return self->message(message, wparam, lparam); }
    catch (const std::exception& error) {
        spdlog::error("Window event failed: {}", error.what());
        self->state_.failed = true;
        self->state_.error = error.what();
        self->close_requested_ = true;
        return 0;
    }
}
void DiagnosticWindow::dispatch() {
    MSG event{};
    while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
        if (event.message == WM_QUIT) close_requested_ = true;
        TranslateMessage(&event);
        DispatchMessageW(&event);
    }
}
void DiagnosticWindow::resize() {
    RECT rect{};
    GetClientRect(window_, &rect);
    if (rect.right <= 0 || rect.bottom <= 0) return;
    state_.scale = GetDpiForWindow(window_) / 96.0;
    width_ = std::max(1, static_cast<int>(std::lround(rect.right / state_.scale)));
    height_ = std::max(1, static_cast<int>(std::lround(rect.bottom / state_.scale)));
    const auto bounds = DiagnosticScene::web_bounds(width_, height_);
    state_.view_width = bounds.width;
    state_.view_height = bounds.height;
    if (client_ && client_->browser()) {
        client_->browser()->GetHost()->NotifyScreenInfoChanged();
        client_->browser()->GetHost()->WasResized();
    }
}
void DiagnosticWindow::paint() {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window_, &paint);
    RECT rect{};
    GetClientRect(window_, &rect);
    if (rect.right <= 0 || rect.bottom <= 0) { EndPaint(window_, &paint); return; }
    try {
        const auto draw = [&](SkCanvas& canvas) {
            canvas.save();
            canvas.scale(static_cast<float>(state_.scale), static_cast<float>(state_.scale));
            scene_.draw(canvas, width_, height_, state_, theme_, 0);
            SkPaint ink;
            ink.setColor(design::themes[theme_].text);
            ink.setStrokeWidth(1.2f);
            const float right = static_cast<float>(width_);
            canvas.drawLine(right-122, 33, right-110, 33, ink);
            ink.setStyle(SkPaint::kStroke_Style);
            canvas.drawRect(SkRect::MakeXYWH(right-78, 27, 10, 10), ink);
            canvas.drawLine(right-34, 27, right-24, 37, ink);
            canvas.drawLine(right-24, 27, right-34, 37, ink);
            canvas.restore();
        };
        if (gpu_) {
            draw(gpu_->begin(rect.right, rect.bottom));
            gpu_->present();
        } else {
            auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(rect.right, rect.bottom));
            if (!surface) throw std::runtime_error("Raster allocation failed");
            draw(*surface->getCanvas());
            SkPixmap pixels;
            surface->peekPixels(&pixels);
            BITMAPINFO bitmap{};
            bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), rect.right, -rect.bottom, 1, 32, BI_RGB};
            StretchDIBits(dc, 0, 0, rect.right, rect.bottom, 0, 0, rect.right, rect.bottom,
                          pixels.addr(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
        }
    } catch (...) { EndPaint(window_, &paint); throw; }
    EndPaint(window_, &paint);
}
CefMouseEvent DiagnosticWindow::mouse_event(int x, int y) const {
    const auto bounds = DiagnosticScene::web_bounds(width_, height_);
    CefMouseEvent event;
    event.x = static_cast<int>(std::lround(x / state_.scale)) - bounds.x;
    event.y = static_cast<int>(std::lround(y / state_.scale)) - bounds.y;
    event.modifiers = modifiers();
    return event;
}
void DiagnosticWindow::ime(LPARAM flags) {
    if (!client_ || !client_->browser()) return;
    const auto host = client_->browser()->GetHost();
    const auto context = ImmGetContext(window_);
    if (!context) return;
    const auto text = [&](DWORD type) {
        const auto bytes = ImmGetCompositionStringW(context, type, nullptr, 0);
        if (bytes <= 0 || bytes > 65536) return std::wstring{};
        std::wstring value(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
        ImmGetCompositionStringW(context, type, value.data(), static_cast<DWORD>(bytes));
        return value;
    };
    if (flags & GCS_RESULTSTR) host->ImeCommitText(text(GCS_RESULTSTR), CefRange(UINT_MAX, UINT_MAX), 0);
    if (flags & GCS_COMPSTR) {
        const auto composition = text(GCS_COMPSTR);
        const auto cursor = std::clamp<LONG>(ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0),
                                           0, static_cast<LONG>(composition.size()));
        CefCompositionUnderline underline;
        underline.range = CefRange(0, static_cast<std::uint32_t>(composition.size()));
        underline.color = 0xFF000000;
        host->ImeSetComposition(composition, {underline}, CefRange(UINT_MAX, UINT_MAX), CefRange(cursor, cursor));
    }
    ImmReleaseContext(window_, context);
}
LRESULT DiagnosticWindow::message(UINT event, WPARAM wparam, LPARAM lparam) {
    const auto browser = client_ ? client_->browser() : nullptr;
    switch (event) {
    case WM_NCCALCSIZE:
        if (wparam) {
            if (IsZoomed(window_)) {
                MONITORINFO monitor{sizeof(MONITORINFO)};
                GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
                reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0] = monitor.rcWork;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window_, &point);
        RECT rect{}; GetClientRect(window_, &rect);
        const int border = std::max(4, pixels_from_dip(6, state_.scale));
        if (!IsZoomed(window_)) {
            const bool left = point.x < border, right = point.x >= rect.right-border;
            const bool top = point.y < border, bottom = point.y >= rect.bottom-border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT; if (right) return HTRIGHT;
            if (top) return HTTOP; if (bottom) return HTBOTTOM;
        }
        if (point.y < pixels_from_dip(64, state_.scale)) {
            if (point.x > rect.right-pixels_from_dip(48, state_.scale)) return HTCLOSE;
            if (point.x > rect.right-pixels_from_dip(96, state_.scale)) return HTMAXBUTTON;
            if (point.x > rect.right-pixels_from_dip(144, state_.scale)) return HTMINBUTTON;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {pixels_from_dip(900, state_.scale), pixels_from_dip(740, state_.scale)};
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right-rect->left,
                     rect->bottom-rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        resize(); return 0;
    }
    case WM_SIZE:
        resize();
        if (browser) browser->GetHost()->WasHidden(wparam == SIZE_MINIMIZED);
        return 0;
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: close_requested_ = true; return 0;
    case WM_SETFOCUS: if (browser) browser->GetHost()->SetFocus(true); return 0;
    case WM_KILLFOCUS: if (browser) browser->GetHost()->SetFocus(false); return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F6) { theme_ = (theme_ + 2) % 6; InvalidateRect(window_, nullptr, FALSE); return 0; }
        if (wparam == VK_F7) { theme_ ^= 1; InvalidateRect(window_, nullptr, FALSE); return 0; }
        [[fallthrough]];
    case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        if (browser) {
            CefKeyEvent key;
            key.type = (event == WM_CHAR || event == WM_SYSCHAR) ? KEYEVENT_CHAR :
                (event == WM_KEYUP || event == WM_SYSKEYUP) ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;
            key.windows_key_code = static_cast<int>(wparam);
            key.native_key_code = static_cast<int>(lparam);
            key.modifiers = modifiers();
            key.is_system_key = event == WM_SYSCHAR || event == WM_SYSKEYDOWN || event == WM_SYSKEYUP;
            browser->GetHost()->SendKeyEvent(key);
            if (!key.is_system_key) return 0;
        }
        break;
    case WM_IME_COMPOSITION: ime(lparam); return 0;
    case WM_IME_STARTCOMPOSITION: return 0;
    case WM_IME_ENDCOMPOSITION: if (browser) browser->GetHost()->ImeCancelComposition(); return 0;
    case WM_IME_SETCONTEXT:
        lparam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
        break;
    case WM_MOUSEMOVE:
        if (browser) {
            if (!tracking_) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track); tracking_ = true; }
            browser->GetHost()->SendMouseMoveEvent(mouse_event(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), false);
        }
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (browser) browser->GetHost()->SendMouseMoveEvent(mouse_event(-1, -1), true);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        if (browser) {
            const bool up = event == WM_LBUTTONUP || event == WM_RBUTTONUP || event == WM_MBUTTONUP;
            const auto button = (event == WM_RBUTTONDOWN || event == WM_RBUTTONUP || event == WM_RBUTTONDBLCLK)
                ? MBT_RIGHT : (event == WM_MBUTTONDOWN || event == WM_MBUTTONUP || event == WM_MBUTTONDBLCLK) ? MBT_MIDDLE : MBT_LEFT;
            const bool twice = event == WM_LBUTTONDBLCLK || event == WM_RBUTTONDBLCLK || event == WM_MBUTTONDBLCLK;
            if (up) ReleaseCapture(); else { SetFocus(window_); SetCapture(window_); }
            browser->GetHost()->SendMouseClickEvent(mouse_event(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), button, up, twice ? 2 : 1);
        }
        return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        if (browser) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(window_, &point);
            browser->GetHost()->SendMouseWheelEvent(mouse_event(point.x, point.y),
                event == WM_MOUSEHWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0,
                event == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wparam) : 0);
        }
        return 0;
    }
    return DefWindowProcW(window_, event, wparam, lparam);
}
}
