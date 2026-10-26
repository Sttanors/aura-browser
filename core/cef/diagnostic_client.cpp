#include "core/cef/diagnostic_client.hpp"
#include "include/cef_frame.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_helpers.h"
#include <spdlog/spdlog.h>

namespace aura {
void DiagnosticClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus,
    int error_code, const CefString& error_string) {
    if (state_.closing) return;
    ++state_.renderer_crashes;
    state_.failed = true;
    state_.error = "Renderer terminated: " + std::to_string(error_code) + " " + error_string.ToString();
    spdlog::error("{}", state_.error);
}
void DiagnosticApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                                 CefRefPtr<CefCommandLine> command) {
    if (!process_type.empty()) return;
    if (!verbose_logging_) {
        for (const char *flag : {"enable-logging", "log-file", "log-severity", "v", "vmodule",
                                 "log-net-log", "net-log-capture-mode"})
            command->RemoveSwitch(flag);
        command->AppendSwitchWithValue("log-severity", "disable");
    }
    if (safe_mode_) {
        command->RemoveSwitch("load-extension");
        command->RemoveSwitch("disable-extensions-except");
        command->AppendSwitch("disable-extensions");
    }
    if (!safe_mode_) {
        auto blink_features = command->GetSwitchValue("enable-blink-features").ToString();
        if (!blink_features.empty())
            blink_features += ",";
        command->AppendSwitchWithValue("enable-blink-features",
                                       blink_features + "SpellCheckCustomDictionaryAPI");
    }
#ifndef _WIN32
    command->AppendSwitchWithValue("ozone-platform", capture_ ? "headless" : "x11");
#endif
    if (software_) {
        command->AppendSwitch("disable-gpu");
        command->AppendSwitch("disable-gpu-compositing");
        command->AppendSwitch("disable-accelerated-video-decode");
        command->AppendSwitch("disable-accelerated-video-encode");
    }
    if (safe_mode_ || extension_ == "-")
        command->AppendSwitch("disable-extensions");
    else if (!extension_.empty()) {command->AppendSwitchWithValue("load-extension", extension_);command->AppendSwitchWithValue("disable-extensions-except", extension_); }
}
void DiagnosticClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    browser_ = browser;
    spdlog::info("CEF browser created: {}", browser->GetIdentifier());
}
void DiagnosticClient::OnBeforeClose(CefRefPtr<CefBrowser>) {
    CEF_REQUIRE_UI_THREAD();
    browser_ = nullptr;
    state_.closed = true;
    spdlog::info("CEF OnBeforeClose received");
}
void DiagnosticClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
    rect = {0, 0, state_.view_width, state_.view_height};
}
bool DiagnosticClient::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) {
    info.device_scale_factor = static_cast<float>(state_.scale);
    info.depth = 32;
    info.depth_per_component = 8;
    info.rect = {0, 0, state_.view_width, state_.view_height};
    info.available_rect = info.rect;
    return true;
}
void DiagnosticClient::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                                const RectList& dirty, const void* data, int width, int height) {
    CEF_REQUIRE_UI_THREAD();
    try {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
            throw std::runtime_error("CEF supplied unsupported frame dimensions");
        std::vector<Rect> rectangles;
        for (const auto& r : dirty) rectangles.push_back({r.x, r.y, r.width, r.height});
        auto& target = type == PET_VIEW ? state_.view : state_.popup;
        target.update(width, height,
            {static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(width) * height * 4}, rectangles);
        if (state_.repaint) state_.repaint();
        else if (state_.invalidate) state_.invalidate();
    } catch (const std::exception& error) {
        state_.failed = true;
        state_.error = error.what();
        spdlog::error("OSR paint failed: {}", error.what());
    }
}
void DiagnosticClient::OnPopupShow(CefRefPtr<CefBrowser>, bool show) {
    state_.popup_visible = show;
    if (state_.invalidate) state_.invalidate();
}
void DiagnosticClient::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) {
    state_.popup_bounds = {rect.x, rect.y, rect.width, rect.height};
}
void DiagnosticClient::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int status) {
    if (!frame->IsMain()) return;
    state_.loaded = true;
    state_.http_status = status;
    spdlog::info("Main frame loaded; HTTP {}", status);
    if (state_.invalidate) state_.invalidate();
}
void DiagnosticClient::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
    ErrorCode code, const CefString& text, const CefString&) {
    if (!frame->IsMain() || code == ERR_ABORTED) return;
    state_.failed = true;
    state_.error = text.ToString();
    spdlog::error("Main frame load failed: {} ({})", text.ToString(), static_cast<int>(code));
}
void DiagnosticClient::OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) {
    state_.title = title.ToString();
    if (state_.invalidate) state_.invalidate();
}
void DiagnosticClient::OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                                      const CefString& url) {
    if (frame->IsMain()) state_.url = url.ToString();
}
bool DiagnosticClient::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t,
    const CefString& message, const CefString&, int) {
    const auto text = message.ToString();
    constexpr std::string_view prefix = "AURA_MV3_PROBE:";
    if (text.starts_with(prefix)) {
        state_.extension_status = text.substr(prefix.size());
        spdlog::info("MV3 diagnostic: {}", state_.extension_status);
        return true;
    }
    return false;
}
void DiagnosticClient::OnImeCompositionRangeChanged(CefRefPtr<CefBrowser>, const CefRange&,
                                                    const RectList& bounds) {
    if (!bounds.empty() && state_.ime_caret) state_.ime_caret(bounds.back());
}
#ifdef _WIN32
void DiagnosticClient::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
    const RectList&, const CefAcceleratedPaintInfo& info) {
    try {
        texture_probe_.copy_and_read(info.shared_texture_handle,
            type == PET_VIEW ? state_.view : state_.popup);
        ++state_.accelerated_frames;
        if (state_.repaint) state_.repaint();
        else if (state_.invalidate) state_.invalidate();
    } catch (const std::exception& error) {
        state_.failed = true;
        state_.error = error.what();
        spdlog::error("Shared texture probe failed: {}", error.what());
    }
}
#endif
}
