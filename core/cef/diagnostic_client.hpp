#pragma once
#include "common/paint_buffer.hpp"
#include "common/pump_schedule.hpp"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include <functional>
#include <string>
#ifdef _WIN32
#include "render/cef_bridge/shared_texture_probe.hpp"
#endif

namespace aura {
struct DiagnosticState {
    PaintBuffer view;
    PaintBuffer popup;
    Rect popup_bounds;
    bool popup_visible = false;
    bool loaded = false;
    bool closed = false;
    bool failed = false;
    bool closing = false;
    int renderer_crashes = 0;
    int http_status = 0;
    int view_width = 864;
    int view_height = 596;
    double scale = 1;
    std::string title = "Connecting to Chromium";
    std::string url;
    std::string error;
    std::string compositor = "Skia raster";
    std::string graphics_renderer = "not_recorded";
    std::uint64_t presentation_frames = 0;
    std::uint64_t accelerated_frames = 0;
    std::string extension_status = "not_requested";
    std::function<void()> invalidate;
    std::function<void()> repaint;
    std::function<void(const CefRect&)> ime_caret;
};

class DiagnosticApp final : public CefApp, public CefBrowserProcessHandler {
public:
    DiagnosticApp(PumpSchedule& schedule, bool capture, bool software, std::string extension)
        : schedule_(schedule), capture_(capture), software_(software), extension_(std::move(extension)) {}
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnScheduleMessagePumpWork(int64_t delay_ms) override { schedule_.schedule(delay_ms); }
    void OnContextInitialized() override { context_ready_ = true; }
    bool context_ready() const { return context_ready_; }
    std::function<void(CefRefPtr<CefCommandLine>)> relaunch;
    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command,
                                     const CefString&) override {
        if (!relaunch) return false;
        relaunch(command);
        return true;
    }
    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command) override;
private:
    PumpSchedule& schedule_;
    bool capture_, software_;
    std::string extension_;
    bool context_ready_ = false;
    IMPLEMENT_REFCOUNTING(DiagnosticApp);
};

class DiagnosticClient : public CefClient, public CefLifeSpanHandler,
    public CefRenderHandler, public CefLoadHandler, public CefDisplayHandler, public CefRequestHandler {
public:
    explicit DiagnosticClient(DiagnosticState& state) : state_(state) {}
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status,
                                   int error_code, const CefString& error_string) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
        const CefString&, const CefString&, WindowOpenDisposition, bool, const CefPopupFeatures&,
        CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&,
        CefRefPtr<CefDictionaryValue>&, bool*) override { return true; }
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirty,
                 const void* buffer, int width, int height) override;
    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override;
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int status) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                     ErrorCode code, const CefString& text, const CefString& url) override;
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
    void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         const CefString& url) override;
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
        const CefString& message, const CefString& source, int line) override;
    void OnImeCompositionRangeChanged(CefRefPtr<CefBrowser> browser, const CefRange& range,
                                       const RectList& bounds) override;
#ifdef _WIN32
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
        const RectList& dirty, const CefAcceleratedPaintInfo& info) override;
#endif
    [[nodiscard]] CefRefPtr<CefBrowser> browser() const { return browser_; }
private:
    DiagnosticState& state_;
    CefRefPtr<CefBrowser> browser_;
#ifdef _WIN32
    SharedTextureProbe texture_probe_;
#endif
    IMPLEMENT_REFCOUNTING(DiagnosticClient);
};
}
