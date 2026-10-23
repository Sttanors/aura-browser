#include "common/platform.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
namespace aura {
namespace {
CefRefPtr<CefDictionaryValue> dictionary(const std::string &json) {
    auto value = CefParseJSON(json, JSON_PARSER_RFC);
    return value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
}
double number(CefRefPtr<CefDictionaryValue> value, const char *key) {
    if (!value)
        return 0;
    return value->GetType(key) == VTYPE_INT ? value->GetInt(key) : value->GetDouble(key);
}
bool page_url(const std::string &url) {
    return url.starts_with("http://") || url.starts_with("https://") || url.starts_with("file://");
}
} // namespace
void BrowserController::cancel_screenshot() {
    ++screenshot.serial;
    screenshot.pending = false;
    screenshot.selecting = false;
    changed();
}
void BrowserController::capture_screenshot(CaptureRect relative) {
    auto t = tab(screenshot.tab);
    if (screenshot.native_running) {
        screenshot.error = tr("Wait for the previous capture to finish.");
        changed();
        return;
    }
    if (ui->export_dialog_open()) {
        screenshot.error = tr("Finish the open file dialog first.");
        changed();
        return;
    }
    if (!t || !t->browser || !t->devtools || t->closing || t->ntp || t->loading ||
        !t->page.loaded || t->page.failed || !tab_visible(t->id) || !page_url(t->page.url) ||
        t->navigation_serial != screenshot.navigation || t->page.url != screenshot.source) {
        screenshot.error = tr("The page changed. Use the current page and try again.");
        changed();
        return;
    }
    const int mode = screenshot.mode;
    if (mode < 0 || mode > 2 || (mode == 2 && t->reader_active)) {
        screenshot.error = tr("Exit reader view to capture the full page.");
        changed();
        return;
    }
    const bool prepare = mode == 1 && relative.width == 0 && relative.height == 0;
    if (mode == 1 && !prepare &&
        (!std::isfinite(relative.x) || !std::isfinite(relative.y) ||
         !std::isfinite(relative.width) || !std::isfinite(relative.height) || relative.x < 0 ||
         relative.y < 0 || relative.width <= 0 || relative.height <= 0 ||
         relative.x + relative.width > 1.000001 || relative.y + relative.height > 1.000001)) {
        screenshot.error = tr("Select an area inside the visible page.");
        changed();
        return;
    }
    const int id = t->id, view_width = t->page.view_width, view_height = t->page.view_height;
    const auto navigation = t->navigation_serial;
    const double host_zoom = t->browser->GetHost()->GetZoomLevel(), device = t->page.scale;
    const uint64_t job = ++screenshot.serial;
    screenshot.job = job;
    screenshot.job_tab = id;
    screenshot.pending = screenshot.native_running = true;
    screenshot.error.clear();
    screenshot.width = screenshot.height = 0;
    screenshot.reduced = false;
    screenshot.started = std::chrono::steady_clock::now();
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    auto valid = [this, weak, id, navigation, job, host_zoom, device, view_width,
                  view_height]() -> Tab * {
        if (weak.expired() || screenshot.serial != job)
            return nullptr;
        auto t = tab(id);
        return t && t->browser && t->devtools && !t->closing && tab_visible(id) &&
                       t->navigation_serial == navigation && t->page.url == screenshot.source &&
                       t->page.view_width == view_width && t->page.view_height == view_height &&
                       std::abs(t->page.scale - device) < .000001 &&
                       std::abs(t->browser->GetHost()->GetZoomLevel() - host_zoom) < .000001
                   ? t
                   : nullptr;
    };
    auto fail = [this, weak, job](const char *text) {
        if (weak.expired())
            return;
        if (screenshot.job == job) {
            screenshot.surface_width = screenshot.surface_height = 0;
            if (screenshot.emulating) {
                screenshot.emulating = false;
                if (auto source = tab(screenshot.job_tab);
                    source && source->browser && source->devtools) {
                    source->devtools->call("Emulation.clearDeviceMetricsOverride", "{}");
                    source->browser->GetHost()->WasResized();
                }
            }
            if (auto source = tab(screenshot.job_tab);
                source && source->devtools && screenshot.restore_context) {
                auto value = CefDictionaryValue::Create();
                value->SetInt("contextId", screenshot.restore_context);
                value->SetString("expression",
                                 "window.scrollTo({left:" + std::to_string(screenshot.scroll_x) +
                                     ",top:" + std::to_string(screenshot.scroll_y) +
                                     ",behavior:'instant'})");
                auto argument = CefValue::Create();
                argument->SetDictionary(value);
                source->devtools->call("Runtime.evaluate",
                                       CefWriteJSON(argument, JSON_WRITER_DEFAULT).ToString());
            }
            screenshot.restore_context = 0;
            screenshot.native_running = false;
        }
        if (screenshot.serial == job) {
            screenshot.pending = false;
            screenshot.error = tr(text);
        }
        changed();
    };
    t->devtools->call(
        "Page.getLayoutMetrics", "{}",
        [this, weak, valid, fail, job, id, mode, prepare, relative, device,
         host_zoom](bool ok, const std::string &json) {
            auto t = valid();
            if (!t) {
                fail("The page changed. Use the current page and try again.");
                return;
            }
            auto data = ok ? dictionary(json) : nullptr;
            auto viewport = data ? data->GetDictionary("cssVisualViewport") : nullptr;
            auto content = data ? data->GetDictionary("cssContentSize") : nullptr;
            if (!viewport || !content) {
                fail("Could not measure this page. Try again after it loads.");
                return;
            }
            const double width = number(viewport, "clientWidth"),
                         height = number(viewport, "clientHeight");
            double zoom = number(viewport, "zoom");
            if (!std::isfinite(zoom) || zoom <= 0)
                zoom = std::pow(1.2, host_zoom);
            if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
                fail("Could not measure this page. Try again after it loads.");
                return;
            }
            if (prepare) {
                screenshot.client_width = width;
                screenshot.client_height = height;
                screenshot.area_zoom = zoom;
                screenshot.area_view_width = t->page.view_width;
                screenshot.area_view_height = t->page.view_height;
                screenshot.pending = screenshot.native_running = false;
                ui->begin_screenshot_area();
                changed();
                return;
            }
            CaptureRect css{number(viewport, "pageX"), number(viewport, "pageY"), width, height};
            screenshot.scroll_x = css.x;
            screenshot.scroll_y = css.y;
            screenshot.restore_context = 0;
            if (mode == 2)
                css = {number(content, "x"), number(content, "y"),
                       std::max(number(content, "width"), t->page.view_width / zoom),
                       std::max(number(content, "height"), t->page.view_height / zoom)};
            if (mode == 1) {
                if (std::abs(width - screenshot.client_width) > 1 ||
                    std::abs(height - screenshot.client_height) > 1 ||
                    std::abs(zoom - screenshot.area_zoom) > .00001) {
                    fail("The page layout changed. Select the area again.");
                    return;
                }
                css = {css.x + relative.x * width, css.y + relative.y * height,
                       relative.width * width, relative.height * height};
            }
            const auto plan = screenshot_plan(css, zoom, device);
            if (!plan) {
                fail("This page is too large. Try a smaller area.");
                return;
            }
            auto params = CefDictionaryValue::Create(), clip = CefDictionaryValue::Create();
            params->SetString("format", "png");
            params->SetBool("fromSurface", true);
            params->SetBool("captureBeyondViewport", mode == 2);
            clip->SetDouble("x", plan->clip.x);
            clip->SetDouble("y", plan->clip.y);
            clip->SetDouble("width", plan->clip.width);
            clip->SetDouble("height", plan->clip.height);
            clip->SetDouble("scale", plan->scale);
            params->SetDictionary("clip", clip);
            auto value = CefValue::Create();
            value->SetDictionary(params);
            const auto payload = CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
            auto capture = [this, weak, valid, fail, job, id, plan = *plan, payload,
                            device](bool emulated, const std::string &) {
                auto t = valid();
                if (!t) {
                    fail("The page changed. Use the current page and try again.");
                    return;
                }
                if (!emulated) {
                    fail("Could not capture this page. Try visible area or a smaller region.");
                    return;
                }
                // CEF OSR's native SetSize is a no-op. Supply an actual render surface;
                // device metrics keep the original layout viewport while capturing it.
                screenshot.surface_width = std::max(1, int(std::ceil(plan.width / device)));
                screenshot.surface_height = std::max(1, int(std::ceil(plan.height / device)));
                t->browser->GetHost()->WasResized();
                t->devtools->call(
                    "Page.captureScreenshot", payload,
                    [this, weak, valid, fail, job, id, plan](bool ok, const std::string &json) {
                        if (weak.expired())
                            return;
                        auto t = tab(id);
                        if (!t || !t->devtools) {
                            fail("The page changed. Use the current page and try again.");
                            return;
                        }
                        screenshot.surface_width = screenshot.surface_height = 0;
                        screenshot.emulating = false;
                        t->devtools->call(
                            "Emulation.clearDeviceMetricsOverride", "{}",
                            [this, weak, valid, fail, job, id, plan, ok,
                             json](bool cleared, const std::string &) {
                                auto t = valid();
                                if (!t) {
                                    fail("The page changed. Use the current page and try again.");
                                    return;
                                }
                                auto scroll = CefDictionaryValue::Create();
                                scroll->SetInt("contextId", screenshot.restore_context);
                                scroll->SetString(
                                    "expression",
                                    "window.scrollTo({left:" + std::to_string(screenshot.scroll_x) +
                                        ",top:" + std::to_string(screenshot.scroll_y) +
                                        ",behavior:'instant'})");
                                auto scroll_value = CefValue::Create();
                                scroll_value->SetDictionary(scroll);
                                t->browser->GetHost()->WasResized();
                                t->devtools->call(
                                    "Runtime.evaluate",
                                    CefWriteJSON(scroll_value, JSON_WRITER_DEFAULT).ToString(),
                                    [this, weak, valid, fail, job, id, plan, ok, json,
                                     cleared](bool restored, const std::string &) {
                                        if (weak.expired())
                                            return;
                                        restored = restored && cleared;
                                        screenshot.restore_context = 0;
                                        if (weak.expired())
                                            return;
                                        if (auto source = tab(id); source && source->browser)
                                            source->browser->GetHost()->WasResized();
                                        if (screenshot.job == job)
                                            screenshot.native_running = false;
                                        if (!valid()) {
                                            fail("The page changed. Use the current page and try "
                                                 "again.");
                                            return;
                                        }
                                        if (!restored || !ok ||
                                            json.size() > 180ULL * 1024 * 1024) {
                                            fail("Could not capture this page. Try visible area or "
                                                 "a smaller region.");
                                            return;
                                        }
                                        auto data = dictionary(json);
                                        if (!data || !data->HasKey("data")) {
                                            fail("Could not capture this page. Try visible area or "
                                                 "a smaller region.");
                                            return;
                                        }
                                        const auto encoded = data->GetString("data");
                                        if (encoded.length() > 180ULL * 1024 * 1024) {
                                            fail("The captured image is too large. Choose a "
                                                 "smaller area.");
                                            return;
                                        }
                                        auto decoded = CefBase64Decode(encoded);
                                        if (!decoded || decoded->GetSize() > 128ULL * 1024 * 1024) {
                                            fail("The captured image is too large. Choose a "
                                                 "smaller area.");
                                            return;
                                        }
                                        std::string png(decoded->GetSize(), '\0');
                                        if (decoded->GetData(png.data(), png.size(), 0) !=
                                            png.size()) {
                                            fail("Could not read the captured image.");
                                            return;
                                        }
                                        const auto size = screenshot_png_size(png);
                                        if (!size || std::abs(size->first - plan.width) > 2 ||
                                            std::abs(size->second - plan.height) > 2) {
                                            fail("Could not capture the requested size. Try "
                                                 "visible area.");
                                            return;
                                        }
                                        screenshot.pending = false;
                                        screenshot.width = size->first;
                                        screenshot.height = size->second;
                                        screenshot.reduced = plan.reduced;
                                        ++audit["page_screenshots"];
                                        auto time = local_time(std::time(nullptr));
                                        char filename[80]{};
                                        std::strftime(filename, sizeof(filename),
                                                      "aura-page-%Y%m%d-%H%M%S.png", &time);
                                        ui->save_export(std::move(png), filename, "PNG | *.png",
                                                        "Save screenshot");
                                        changed();
                                    });
                            });
                    });
            };
            auto metrics = CefDictionaryValue::Create();
            metrics->SetInt("width", t->page.view_width);
            metrics->SetInt("height", t->page.view_height);
            metrics->SetDouble("deviceScaleFactor", device);
            metrics->SetBool("mobile", false);
            metrics->SetBool("dontSetVisibleSize", true);
            auto metrics_value = CefValue::Create();
            metrics_value->SetDictionary(metrics);
            const auto metric_json = CefWriteJSON(metrics_value, JSON_WRITER_DEFAULT).ToString();
            t->devtools->call(
                "Page.getFrameTree", "{}",
                [this, weak, valid, fail, capture = std::move(capture),
                 metric_json](bool ok, const std::string &json) mutable {
                    auto t = valid();
                    if (!t) {
                        fail("The page changed. Use the current page and try again.");
                        return;
                    }
                    auto result = ok ? dictionary(json) : nullptr;
                    auto tree = result ? result->GetDictionary("frameTree") : nullptr;
                    auto frame = tree ? tree->GetDictionary("frame") : nullptr;
                    if (!frame) {
                        fail("Could not measure this page. Try again after it loads.");
                        return;
                    }
                    auto args = CefDictionaryValue::Create();
                    args->SetString("frameId", frame->GetString("id"));
                    args->SetString("worldName", "Aura screenshot restore");
                    auto value = CefValue::Create();
                    value->SetDictionary(args);
                    t->devtools->call(
                        "Page.createIsolatedWorld",
                        CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString(),
                        [this, weak, valid, fail, capture = std::move(capture),
                         metric_json](bool ok, const std::string &json) mutable {
                            auto t = valid();
                            if (!t) {
                                fail("The page changed. Use the current page and try again.");
                                return;
                            }
                            auto result = ok ? dictionary(json) : nullptr;
                            const int context = result ? result->GetInt("executionContextId") : 0;
                            if (!context) {
                                fail("Could not measure this page. Try again after it loads.");
                                return;
                            }
                            screenshot.restore_context = context;
                            screenshot.emulating = true;
                            t->devtools->call("Emulation.setDeviceMetricsOverride", metric_json,
                                              std::move(capture));
                        });
                });
        });
    changed();
}
} // namespace aura
