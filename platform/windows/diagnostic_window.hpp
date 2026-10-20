#pragma once
#include "render/diagnostic_scene.hpp"
#include "render/rhi/angle_surface.hpp"
#include <windows.h>
#include <memory>
namespace aura {
class DiagnosticWindow {
public:
    DiagnosticWindow(HINSTANCE instance, DiagnosticScene& scene, DiagnosticState& state,
                     int width, int height, unsigned theme);
    ~DiagnosticWindow();
    HWND handle() const { return window_; }
    void attach(CefRefPtr<DiagnosticClient> client) { client_ = client; }
    void dispatch();
    bool wants_close() const { return close_requested_; }
    unsigned theme() const { return theme_; }
    int width() const { return width_; }
    int height() const { return height_; }
private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
    void paint();
    void resize();
    CefMouseEvent mouse_event(int x, int y) const;
    void ime(LPARAM flags);
    HWND window_{};
    DiagnosticScene& scene_;
    DiagnosticState& state_;
    CefRefPtr<DiagnosticClient> client_;
    std::unique_ptr<AngleSurface> gpu_;
    int width_, height_;
    unsigned theme_;
    bool close_requested_ = false;
    bool tracking_ = false;
};
}
