#include "common/config.hpp"
#include "common/platform.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
#include <fstream>
namespace aura {
namespace {
CefRefPtr<CefDictionaryValue> dictionary(const std::string &json) {
    auto value = CefParseJSON(json, JSON_PARSER_RFC);
    return value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
}
} // namespace
void BrowserController::save_reading_article() {
    if (safe_mode()) {
        ui->toast(tr("This tool is paused in safe mode."));
        return;
    }
    auto t = current();
    if (!t || t->private_tab || private_window) {
        ui->toast(tr("Private pages cannot be saved to the reading list."));
        return;
    }
    if (!t->browser || !t->devtools || t->ntp || t->loading || !t->page.loaded ||
        !(t->page.url.starts_with("http://") || t->page.url.starts_with("https://"))) {
        ui->toast(tr("Open a loaded article on a website first."));
        return;
    }
    if (t->reading_pending || t->reader_pending)
        return;
    const auto source = read_resource(resources / "vendor/readability.js") + "\n" +
                        read_resource(resources / "reader/extract.js");
    const int id = t->id;
    const auto serial = t->navigation_serial;
    const auto url = t->page.url;
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    const auto generation = ++t->reading_generation;
    t->reading_pending = true;
    t->reader_started = std::chrono::steady_clock::now();
    ui->toast(tr("Saving article…"));
    changed();
    auto valid = [this, weak, id, serial, generation]() -> Tab * {
        if (weak.expired())
            return nullptr;
        auto t = tab(id);
        return t && !t->closing && t->reading_pending && t->devtools &&
                       t->navigation_serial == serial && t->reading_generation == generation
                   ? t
                   : nullptr;
    };
    auto fail = [this, valid] {
        if (auto t = valid()) {
            t->reading_pending = false;
            ui->toast(tr("Could not save the article. Check the page and available disk space."));
            changed();
        }
    };
    t->devtools->call(
        "Page.getFrameTree", "{}",
        [this, source, valid, fail, url](bool ok, const std::string &json) {
            auto t = valid();
            if (!t)
                return;
            auto data = dictionary(json);
            auto tree = ok && data ? data->GetDictionary("frameTree") : nullptr;
            auto frame = tree ? tree->GetDictionary("frame") : nullptr;
            if (!frame) {
                fail();
                return;
            }
            t->devtools->call(
                "Page.createIsolatedWorld",
                "{\"frameId\":" + json_quote(frame->GetString("id").ToString()) +
                    ",\"worldName\":\"Aura Saved Article\"}",
                [this, source, valid, fail, url](bool ok, const std::string &json) {
                    auto t = valid();
                    if (!t)
                        return;
                    auto data = dictionary(json);
                    int context = ok && data ? data->GetInt("executionContextId") : 0;
                    if (!context) {
                        fail();
                        return;
                    }
                    const auto expression = "(()=>{\n" + source +
                                            "\nconst a=auraExtractArticle();if(a&&typeof "
                                            "a==='object')delete a.textContent;return a;})()";
                    t->devtools->call(
                        "Runtime.evaluate",
                        "{\"contextId\":" + std::to_string(context) +
                            ",\"returnByValue\":true,\"expression\":" + json_quote(expression) +
                            "}",
                        [this, valid, fail, url](bool ok, const std::string &json) {
                            auto t = valid();
                            if (!t)
                                return;
                            auto data = dictionary(json);
                            auto result = ok && data && !data->HasKey("exceptionDetails")
                                              ? data->GetDictionary("result")
                                              : nullptr;
                            auto value = result ? result->GetDictionary("value") : nullptr;
                            auto blocks = value ? value->GetList("blocks") : nullptr;
                            if (!blocks || blocks->GetSize() > 2000) {
                                fail();
                                return;
                            }
                            SavedArticle a;
                            a.url = url;
                            a.title = value->GetString("title").ToString();
                            a.byline = value->GetString("byline").ToString();
                            for (size_t i = 0; i < blocks->GetSize(); ++i) {
                                auto b = blocks->GetDictionary(i);
                                if (!b) {
                                    fail();
                                    return;
                                }
                                a.blocks.push_back({b->GetString("tag").ToString(),
                                                    b->GetString("text").ToString()});
                            }
                            if (!state_->reading_list.save(std::move(a))) {
                                fail();
                                return;
                            }
                            t->reading_pending = false;
                            ++audit["reading_articles_saved"];
                            ui->toast(tr("Article saved for offline reading."));
                            changed();
                        });
                });
        });
}
void BrowserController::open_reading_article(int id) {
    const auto a = state_->reading_list.find(id);
    if (!a)
        return;
    try {
        const auto directory = profile.directory / "reading-copies";
        std::filesystem::create_directories(directory);
        const auto path = directory / (std::to_string(id) + ".html");
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << offline_article_html(*a, profile.theme % 2 == 0, profile.reader_font_size,
                                     profile.reader_width, profile.reader_font == "serif");
        file.flush();
        if (!file)
            throw std::runtime_error("write");
        file.close();
        if (!file)
            throw std::runtime_error("close");
        replace_file(temporary, path);
        std::string encoded;
        for (unsigned char ch : utf8_path(std::filesystem::absolute(path))) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '/' || ch == ':' || ch == '-' || ch == '_' || ch == '.')
                encoded += ch;
            else {
                const char *hex = "0123456789ABCDEF";
                encoded += '%';
                encoded += hex[ch >> 4];
                encoded += hex[ch & 15];
            }
        }
#ifdef _WIN32
        // generic_string provides forward slashes for file URLs.
        for (size_t p = 0; (p = encoded.find("%5C", p)) != std::string::npos;)
            encoded.replace(p, 3, "/");
        encoded = "/" + encoded;
#endif
        ui->panel(BrowserUi::None);
        new_tab("file://" + encoded, private_window, true, false, -1, 0, 0);
        ++audit["reading_articles_opened"];
        changed();
    } catch (const std::exception &) {
        ui->toast(tr("Could not open the offline copy. Check available disk space."));
    }
}
void BrowserController::toggle_reader() {
    if (safe_mode()) {
        ui->toast(tr("This tool is paused in safe mode."));
        return;
    }
    auto t = current();
    if (!t || !t->devtools || !t->browser || t->ntp || t->closing)
        return;
    if (t->reader_active && t->reader_context) {
        t->devtools->call("Runtime.evaluate",
                          "{\"contextId\":" + std::to_string(t->reader_context) +
                              ",\"expression\":\"globalThis.__auraReader?.close()\"}");
        return;
    }
    if (t->reader_pending || t->reading_pending)
        return;
    const auto &url = t->page.url;
    if ((!url.starts_with("http://") && !url.starts_with("https://")) || t->loading) {
        ui->toast(tr("Open a loaded article on a website first."));
        return;
    }
    auto source = read_resource(resources / "vendor/readability.js"),
         view = read_resource(resources / "reader/extract.js") + "\n" +
                read_resource(resources / "reader/view.js");
    if (source.empty() || view.empty()) {
        ui->toast(tr("Reader files are missing. Reinstall Aura."));
        return;
    }
    const int id = t->id;
    const auto serial = t->navigation_serial;
    const auto weak = std::weak_ptr<void>(callback_lifetime);
    t->reader_pending = true;
    t->reader_started = std::chrono::steady_clock::now();
    ui->toast(tr("Preparing reader view..."));
    changed();
    auto valid = [this, id, serial, weak]() -> Tab * {
        if (weak.expired())
            return nullptr;
        auto t = tab(id);
        return t && !t->closing && t->devtools && t->reader_pending &&
                       t->navigation_serial == serial
                   ? t
                   : nullptr;
    };
    auto fail = [this, valid] {
        if (auto t = valid()) {
            t->reader_pending = false;
            if (t == current())
                ui->toast(tr("Could not prepare reader view. Try again after the page loads."));
            changed();
        }
    };
    t->devtools->call(
        "Page.getFrameTree", "{}",
        [this, source = std::move(source), view = std::move(view), valid,
         fail](bool ok, const std::string &json) {
            auto t = valid();
            if (!t)
                return;
            auto data = dictionary(json);
            auto tree = ok && data ? data->GetDictionary("frameTree") : nullptr;
            auto frame = tree ? tree->GetDictionary("frame") : nullptr;
            if (!frame) {
                fail();
                return;
            }
            t->devtools->call(
                "Page.createIsolatedWorld",
                "{\"frameId\":" + json_quote(frame->GetString("id").ToString()) +
                    ",\"worldName\":\"Aura Reader\"}",
                [this, source, view, valid, fail](bool ok, const std::string &json) {
                    auto t = valid();
                    if (!t)
                        return;
                    auto data = dictionary(json);
                    const int context = ok && data ? data->GetInt("executionContextId") : 0;
                    if (!context) {
                        fail();
                        return;
                    }
                    t->reader_context = context;
                    t->devtools->call(
                        "Runtime.addBinding",
                        "{\"name\":\"auraReaderState\",\"executionContextName\":\"Aura Reader\"}",
                        [this, source, view, context, valid, fail](bool ok, const std::string &) {
                            auto t = valid();
                            if (!t)
                                return;
                            if (!ok) {
                                fail();
                                return;
                            }
                            const auto &p = profile;
                            std::string labels = "{";
                            for (const auto *label : {"Reader mode", "Exit reader", "Text size",
                                                      "Reader font", "Reader column width", "Serif",
                                                      "Sans serif", "Narrow", "Medium", "Wide"}) {
                                if (labels.size() > 1)
                                    labels += ",";
                                labels += json_quote(label) + ":" + json_quote(tr(label));
                            }
                            labels += "}";
                            const auto config =
                                "{\"size\":" + std::to_string(p.reader_font_size) +
                                ",\"width\":" + std::to_string(p.reader_width) +
                                ",\"font\":" + json_quote(p.reader_font) +
                                ",\"dark\":" + (p.theme % 2 == 0 ? "true" : "false") +
                                ",\"labels\":" + labels + "}";
                            const auto expression = "(()=>{\n" + source + "\n" + view +
                                                    "\nreturn auraOpenReader(" + config + ");})()";
                            t->devtools->call(
                                "Runtime.evaluate",
                                "{\"contextId\":" + std::to_string(context) +
                                    ",\"returnByValue\":true,\"expression\":" +
                                    json_quote(expression) + "}",
                                [this, valid, fail](bool ok, const std::string &json) {
                                    auto t = valid();
                                    if (!t)
                                        return;
                                    auto data = dictionary(json);
                                    auto result =
                                        ok && data ? data->GetDictionary("result") : nullptr;
                                    if (!result || data->HasKey("exceptionDetails")) {
                                        fail();
                                        return;
                                    }
                                    t->reader_pending = false;
                                    t->reader_active = result->GetType("value") == VTYPE_BOOL &&
                                                       result->GetBool("value");
                                    if (result->GetString("value") == "too_large") {
                                        ++audit["reader_large_articles"];
                                        if (t == current())
                                            ui->toast(tr("This article is too large for reader "
                                                         "mode. The original page is unchanged."));
                                    } else if (!t->reader_active && t == current())
                                        ui->toast(tr("No article found on this page."));
                                    else if (t == current())
                                        ui->toast("");
                                    changed();
                                });
                        });
                });
        });
}
void BrowserController::reader_event(Tab &t, CefRefPtr<CefDictionaryValue> value) {
    const auto action = value->GetString("action").ToString();
    if (action == "closed") {
        t.reader_active = false;
        t.reader_pending = false;
        t.reader_ui = "null";
        t.selected_text.clear();
        changed();
        return;
    }
    if (action == "view" && (t.reader_active || t.reader_pending)) {
        auto result = CefDictionaryValue::Create();
        for (const auto *key : {"blocks", "characters", "scroll"})
            result->SetInt(key, std::clamp(value->GetInt(key), 0, 2000000));
        for (const auto *key : {"exit", "smaller", "larger", "font", "width", "body", "text"}) {
            auto rectangle = value->GetList(key);
            if (!rectangle || rectangle->GetSize() != 4)
                return;
            auto copy = CefListValue::Create();
            copy->SetSize(4);
            for (int i = 0; i < 4; i++)
                copy->SetInt(i, std::clamp(rectangle->GetInt(i), -1000000, 1000000));
            result->SetList(key, copy);
        }
        auto wrapper = CefValue::Create();
        wrapper->SetDictionary(result);
        t.reader_ui = CefWriteJSON(wrapper, JSON_WRITER_DEFAULT).ToString();
        changed();
        return;
    }
    if (action != "preferences" || !t.reader_active)
        return;
    const int size = value->GetInt("size"), width = value->GetInt("width");
    const auto family = value->GetString("font").ToString();
    if (size < 18 || size > 32 || size % 2 || (width != 560 && width != 720 && width != 900) ||
        (family != "serif" && family != "sans"))
        return;
    profile.reader_font_size = size;
    profile.reader_width = width;
    profile.reader_font = family;
    changed(true);
}
} // namespace aura
