#include "common/config.hpp"
#include "core/browser/browser.hpp"
#include "include/cef_frame.h"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
namespace aura {
namespace {
std::string config(const Profile &p) {
    return "{\"ads\":" + std::string(p.adblock ? "true" : "false") +
           ",\"cosmetics\":" + (p.cosmetic_filtering ? "true" : "false") + ",\"dark\":" +
           ((p.darkreader_mode == "on" || (p.darkreader_mode == "theme" && p.theme % 2 == 0))
                ? "true"
                : "false") +
           ",\"darkExceptions\":" + json_quote(p.darkreader_exceptions) +
           ",\"brightness\":" + std::to_string(p.darkreader_brightness) +
           ",\"contrast\":" + std::to_string(p.darkreader_contrast) +
           ",\"sepia\":" + std::to_string(p.darkreader_sepia) + "}";
}
std::string script(const std::filesystem::path &resources, const Profile &profile) {
    // Extensions, browser pages and the Web Store never receive site injections.
    return "(()=>{if(!/^https?:$/"
           ".test(location.protocol)||location.hostname==='chromewebstore.google.com')return;\n" +
           read_resource(resources / "vendor/darkreader.js") +
           "\nDarkReader.setFetchMethod(url=>fetch(url,{credentials:'omit',cache:'force-cache'}));"
           "\n" +
           read_resource(resources / "protection/site-tools.js") + "\nglobalThis.__auraApply(" +
           config(profile) + ");})();";
}
} // namespace
void BrowserController::configure_font_preferences(CefRefPtr<CefRequestContext> context) {
    // Chrome uses profile preferences when recreating WebPreferences after DevTools attaches.
    // Keep them consistent with CefBrowserSettings, including across the bootstrap navigation.
    const std::pair<const char *, int> sizes[] = {
        {"webkit.webprefs.default_font_size", profile.default_font_size},
        {"webkit.webprefs.default_fixed_font_size", profile.default_font_size},
        {"webkit.webprefs.minimum_font_size", profile.minimum_font_size},
        {"webkit.webprefs.minimum_logical_font_size", profile.minimum_font_size}};
    for (const auto &[name, size] : sizes) {
        auto value = CefValue::Create();
        value->SetInt(size);
        CefString error;
        if (!context->SetPreference(name, value, error))
            ++audit["font_preference_errors"];
    }
    auto value = CefValue::Create();
    value->SetString(profile.font_family == "mono"    ? "monospace"
                     : profile.font_family == "serif" ? "serif"
                                                      : "sans-serif");
    CefString error;
    if (!context->SetPreference("webkit.webprefs.fonts.standard.Zyyy", value, error))
        ++audit["font_preference_errors"];
}
void BrowserController::setup_site_tools(int id, std::function<void()> ready) {
    auto t = tab(id);
    if (!t || !t->devtools)
        return;
    t->devtools->event = [this, id](bool, const std::string &event) {
        auto t = tab(id);
        if (!t || !t->devtools || t->closing)
            return;
        auto nl = event.find('\n');
        auto method = event.substr(0, nl);
        auto value = CefParseJSON(event.substr(nl + 1), JSON_PARSER_RFC);
        if (!value || value->GetType() != VTYPE_DICTIONARY)
            return;
        auto p = value->GetDictionary();
        if (method == "Runtime.executionContextCreated") {
            ++audit["protection_context_events"];
            auto c = p->GetDictionary("context");
            if (c && c->GetString("name") == "Aura Site Protection")
                t->protection_contexts[c->GetInt("id")] = c->GetString("origin").ToString();
        } else if (method == "Runtime.executionContextDestroyed")
            t->protection_contexts.erase(p->GetInt("executionContextId"));
        else if (method == "Runtime.executionContextsCleared")
            t->protection_contexts.clear();
        else if (method == "Runtime.bindingCalled" && p->GetString("name") == "auraProtection") {
            ++audit["protection_bindings"];
            const int context = p->GetInt("executionContextId");
            if (!t->protection_contexts.contains(context))
                return;
            auto payload = p->GetString("payload").ToString();
            if (payload.size() > 1024 * 1024)
                return;
            auto data = CefParseJSON(payload, JSON_PARSER_RFC);
            if (!data || data->GetType() != VTYPE_DICTIONARY)
                return;
            auto d = data->GetDictionary();
            auto url = d->GetString("url").ToString();
            if (!(url.starts_with("http://") || url.starts_with("https://")))
                return;
            auto classes = d->GetValue("classes"), ids = d->GetValue("ids");
            if (!classes || !ids || classes->GetType() != VTYPE_LIST ||
                ids->GetType() != VTYPE_LIST || classes->GetList()->GetSize() > 3000 ||
                ids->GetList()->GetSize() > 3000)
                return;
            ++audit["protection_scans"];
            auto selectors = request_policy->protection->selectors(
                url, CefWriteJSON(classes, JSON_WRITER_DEFAULT).ToString(),
                CefWriteJSON(ids, JSON_WRITER_DEFAULT).ToString());
            if (selectors != "[]")
                ++audit["protection_cosmetics"];
            defer([this, id, context, selectors] {
                if (auto t = tab(id); t && t->devtools && t->protection_contexts.contains(context))
                    t->devtools->call(
                        "Runtime.evaluate",
                        "{\"contextId\":" + std::to_string(context) + ",\"expression\":" +
                            json_quote("globalThis.__auraSelectors?.(" + selectors + ")") + "}");
            });
        }
    };
    t->devtools->call("Runtime.enable");
    t->devtools->call("Page.enable");
    t->devtools->call(
        "Runtime.addBinding",
        "{\"name\":\"auraProtection\",\"executionContextName\":\"Aura Site Protection\"}");
    t->devtools->call(
        "Page.addScriptToEvaluateOnNewDocument",
        "{\"worldName\":\"Aura Site Protection\",\"runImmediately\":true,\"source\":" +
            json_quote(script(resources, profile)) + "}",
        [this, id, ready](bool success, const std::string &result) {
            if (auto t = tab(id); t && t->devtools) {
                auto v = CefParseJSON(result, JSON_PARSER_RFC);
                if (success && v && v->GetType() == VTYPE_DICTIONARY)
                    t->protection_script = v->GetDictionary()->GetString("identifier").ToString();
                defer(ready);
            }
        });
}
void BrowserController::update_site_tools(Tab &t) {
    if (!t.devtools)
        return;
    for (const auto &[context, origin] : t.protection_contexts)
        t.devtools->call("Runtime.evaluate",
                         "{\"contextId\":" + std::to_string(context) + ",\"expression\":" +
                             json_quote("globalThis.__auraApply?.(" + config(profile) + ")") + "}");
    if (!t.protection_script.empty())
        t.devtools->call("Page.removeScriptToEvaluateOnNewDocument",
                         "{\"identifier\":" + json_quote(t.protection_script) + "}");
    const auto id = t.id;
    t.devtools->call("Page.addScriptToEvaluateOnNewDocument",
                     "{\"worldName\":\"Aura Site Protection\",\"source\":" +
                         json_quote(script(resources, profile)) + "}",
                     [this, id](bool success, const std::string &result) {
                         if (auto t = tab(id); t && success) {
                             auto v = CefParseJSON(result, JSON_PARSER_RFC);
                             if (v && v->GetType() == VTYPE_DICTIONARY)
                                 t->protection_script =
                                     v->GetDictionary()->GetString("identifier").ToString();
                         }
                     });
}
void BrowserController::protection_menu() {
    auto t = current();
    bool web =
        t && !t->ntp && (t->page.url.starts_with("http://") || t->page.url.starts_with("https://"));
    auto url = web ? t->page.url : "";
    close_menu();
    menu = {
        {0, tr("Blocked requests: ") + std::to_string(request_policy->protection->blocked.load()),
         false},
        {1,
         tr(site_exception(url, profile.adblock_exceptions) ? "Enable ad blocking here"
                                                            : "Pause ad blocking here"),
         web},
        {2,
         tr(site_exception(url, profile.darkreader_exceptions) ? "Enable Dark Reader here"
                                                               : "Pause Dark Reader here"),
         web && profile.darkreader_mode != "off"},
        {3, tr(profile.darkreader_mode == "off" ? "Turn on Dark Reader" : "Turn off Dark Reader")},
        {4, tr("Protection settings")},
        {5, tr("Extensions")}};
    menu_x = width - 310;
    menu_y = 82;
    menu_action = [this, url](int id) {
        if (id == 1) {
            profile.adblock_exceptions = toggle_site_exception(url, profile.adblock_exceptions);
            apply_settings();
            reload();
        } else if (id == 2) {
            profile.darkreader_exceptions =
                toggle_site_exception(url, profile.darkreader_exceptions);
            apply_settings();
        } else if (id == 3) {
            profile.darkreader_mode = profile.darkreader_mode == "off" ? "on" : "off";
            apply_settings();
        } else if (id == 4)
            ui->show_protection_settings();
        else if (id == 5)
            ui->panel(BrowserUi::Extensions);
    };
    changed();
}
void BrowserController::extension_page(size_t index, bool options) {
    if (index >= extensions.items.size())
        return;
    auto &e = extensions.items[index];
    if (!e.loaded) {
        ui->toast(tr("Restart Aura to load the extension."));
        return;
    }
    if (!options) {
        open_extension_popup(index);
        return;
    }
    auto path = options ? e.options : e.popup;
    if (path.empty()) {
        ui->toast(tr("This extension has no page to open."));
        return;
    }
    ui->panel(BrowserUi::None);
    new_tab("chrome-extension://" + e.id + "/" + path);
}
} // namespace aura
