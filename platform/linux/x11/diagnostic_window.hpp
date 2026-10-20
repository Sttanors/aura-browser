#pragma once
#include "render/diagnostic_scene.hpp"
#include <memory>

namespace aura {
class LinuxDiagnosticWindow {
public:
    LinuxDiagnosticWindow(DiagnosticScene& scene, DiagnosticState& state,
                          int width, int height, unsigned theme, bool accelerated = false);
    ~LinuxDiagnosticWindow();
    LinuxDiagnosticWindow(const LinuxDiagnosticWindow&) = delete;
    LinuxDiagnosticWindow& operator=(const LinuxDiagnosticWindow&) = delete;
    unsigned long handle() const;
    void attach(CefRefPtr<DiagnosticClient> client);
    void dispatch();
    void render_if_needed();
    void wait(int milliseconds);
    bool wants_close() const;
    unsigned theme() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
