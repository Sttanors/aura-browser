#include "core/browser/browser.hpp"
#include "include/cef_parser.h"
#include "ui/browser_ui.hpp"
#include <algorithm>
#include <optional>
namespace aura {
namespace {
std::optional<cef_content_setting_types_t> content_type(const std::string &id) {
    if (id == "notifications")
        return CEF_CONTENT_SETTING_TYPE_NOTIFICATIONS;
    if (id == "camera")
        return CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_CAMERA;
    if (id == "microphone")
        return CEF_CONTENT_SETTING_TYPE_MEDIASTREAM_MIC;
    if (id == "geolocation")
        return CEF_CONTENT_SETTING_TYPE_GEOLOCATION;
    if (id == "clipboard")
        return CEF_CONTENT_SETTING_TYPE_CLIPBOARD_READ_WRITE;
    if (id == "midi")
        return CEF_CONTENT_SETTING_TYPE_MIDI_SYSEX;
    if (id == "sensors")
        return CEF_CONTENT_SETTING_TYPE_SENSORS;
    if (id == "camera_controls")
        return CEF_CONTENT_SETTING_TYPE_CAMERA_PAN_TILT_ZOOM;
    return {};
}
cef_content_setting_values_t native_value(const std::string &value) {
    return value == "allow"   ? CEF_CONTENT_SETTING_VALUE_ALLOW
           : value == "block" ? CEF_CONTENT_SETTING_VALUE_BLOCK
           : value == "ask"   ? CEF_CONTENT_SETTING_VALUE_ASK
                              : CEF_CONTENT_SETTING_VALUE_DEFAULT;
}
std::string text_value(cef_content_setting_values_t value) {
    return value == CEF_CONTENT_SETTING_VALUE_ALLOW   ? "allow"
           : value == CEF_CONTENT_SETTING_VALUE_BLOCK ? "block"
           : value == CEF_CONTENT_SETTING_VALUE_ASK   ? "ask"
                                                      : "default";
}
void apply_rule(CefRefPtr<CefRequestContext> context, const BrowserPreferences &preferences,
                const std::vector<SitePermission> &rules, const std::string &origin,
                const std::string &top, int container, bool private_mode,
                const PermissionSpec &spec) {
    const auto scope = permission_scope(origin, top, spec.id);
    if (scope.empty())
        return;
    auto value = permission_override(rules, scope, container, spec.id);
    auto effective = effective_permission(preferences, rules, scope, container, spec.id);
    if (private_mode && std::string(spec.id) == "notifications")
        value = effective = "block";
    else if (!trustworthy_permission_origin(scope))
        value = effective = "block";
    else if (private_mode && value == "default")
        value = effective;
    if (const auto type = content_type(spec.id))
        context->SetContentSetting(origin, top, *type, native_value(value));
}
} // namespace
std::string permission_origin(const std::string &url) {
    CefURLParts parts;
    if (!CefParseURL(url, parts))
        return {};
    const auto scheme = CefString(&parts.scheme).ToString();
    if (scheme != "https" && scheme != "http")
        return {};
    auto origin = CefString(&parts.origin).ToString();
    if (origin.ends_with('/'))
        origin.pop_back();
    return valid_permission_origin(origin) ? origin : std::string{};
}
std::string BrowserController::permission_site(const Tab &tab, const std::string &requesting,
                                               const std::string &id) const {
    auto top = permission_origin(tab.browser ? tab.browser->GetMainFrame()->GetURL().ToString()
                                             : tab.page.url);
    return permission_scope(permission_origin(requesting), top, id);
}
std::string BrowserController::permission_value(const Tab &tab, const std::string &requesting,
                                                const std::string &id) const {
    if (tab.private_tab && id == "notifications")
        return "block";
    const auto origin = permission_site(tab, requesting, id);
    if (!trustworthy_permission_origin(origin))
        return "block";
    return effective_permission(
        profile, tab.private_tab ? state_->private_site_permissions : profile.site_permissions,
        origin, tab.container, id);
}
CefRefPtr<CefRequestContext> BrowserController::permission_context(int container,
                                                                   bool private_mode) const {
    if (private_mode)
        return state_->private_context;
    if (!container)
        return CefRequestContext::GetGlobalContext();
    const auto found = state_->container_contexts.find(container);
    return found != state_->container_contexts.end() &&
                   state_->container_contexts_ready.contains(container)
               ? found->second
               : nullptr;
}
void BrowserController::configure_permissions(CefRefPtr<CefRequestContext> context,
                                              const std::string &url, const std::string &top_url,
                                              int container, bool private_mode) {
    if (!context)
        return;
    const auto &rules = private_mode ? state_->private_site_permissions : profile.site_permissions;
    if (url.empty() && top_url.empty()) {
        if (!private_mode) {
            for (const auto &spec : permission_catalog())
                if (auto type = content_type(spec.id))
                    context->SetContentSetting("", "", *type, native_value(profile.*(spec.member)));
            context->SetContentSetting("", "", CEF_CONTENT_SETTING_TYPE_POPUPS,
                                       profile.block_popups ? CEF_CONTENT_SETTING_VALUE_BLOCK
                                                            : CEF_CONTENT_SETTING_VALUE_ALLOW);
        }
        for (const auto &rule : rules)
            if (rule.container == container)
                if (const auto spec = find_permission(rule.permission))
                    apply_rule(context, profile, rules, rule.origin, rule.origin, container,
                               private_mode, *spec);
        return;
    }
    const auto origin = permission_origin(url);
    auto top = permission_origin(top_url);
    if (origin.empty())
        return;
    if (top.empty())
        top = origin;
    for (const auto &spec : permission_catalog())
        apply_rule(context, profile, rules, origin, top, container, private_mode, spec);
    context->SetContentSetting(origin, top, CEF_CONTENT_SETTING_TYPE_POPUPS,
                               profile.block_popups ? CEF_CONTENT_SETTING_VALUE_BLOCK
                                                    : CEF_CONTENT_SETTING_VALUE_ALLOW);
}
bool BrowserController::change_site_permission(const std::string &origin, int container,
                                               bool private_mode, const std::string &id,
                                               const std::string &value, bool native) {
    const auto spec = find_permission(id);
    if (!spec || !profile.container(container) || (private_mode && container != 0) ||
        (private_mode && id == "notifications" && (value == "allow" || value == "ask")))
        return false;
    auto &rules = private_mode ? state_->private_site_permissions : profile.site_permissions;
    if (!set_site_permission(rules, origin, container, id, value))
        return false;
    if (native) {
        // Navigation/dismissal cancels requests without remembering an explicit user decision.
        std::vector<std::function<void(bool, const std::string &)>> cancel;
        for (auto *window : state_->windows)
            for (auto it = window->dialogs.begin(); it != window->dialogs.end();) {
                const bool matches = it->permission && it->permission_private == private_mode &&
                                     std::any_of(it->permission_rules.begin(),
                                                 it->permission_rules.end(), [&](const auto &rule) {
                                                     return rule.origin == origin &&
                                                            rule.container == container &&
                                                            rule.permission == id;
                                                 });
                if (matches) {
                    cancel.push_back(std::move(it->action));
                    it = window->dialogs.erase(it);
                } else
                    ++it;
            }
        for (const auto &action : cancel)
            if (action)
                action(false, "");
        if (auto context = permission_context(container, private_mode))
            apply_rule(context, profile, rules, origin, origin, container, private_mode, *spec);
    }
    changed(!private_mode);
    return true;
}
void BrowserController::reset_site_permissions(const std::string &origin, int container,
                                               bool private_mode) {
    for (const auto &spec : permission_catalog())
        change_site_permission(origin, container, private_mode, spec.id, "default");
}
std::string BrowserController::native_permission_value(const std::string &origin, int container,
                                                       bool private_mode,
                                                       const std::string &id) const {
    if (!valid_permission_origin(origin))
        return "unavailable";
    if (private_mode && id == "notifications")
        return "block";
    if (!trustworthy_permission_origin(origin))
        return "unavailable";
    auto context = permission_context(container, private_mode);
    const auto type = content_type(id);
    return context && type ? text_value(context->GetContentSetting(origin, origin, *type))
                           : "pending";
}
} // namespace aura
