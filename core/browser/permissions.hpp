#pragma once
#include "core/browser/preferences.hpp"
#include <span>
#include <string>
#include <vector>
namespace aura {
struct PermissionSpec {
    const char *id, *title;
    std::string BrowserPreferences::*member;
    bool ask = true;
    bool requesting_origin = false;
};
struct SitePermission {
    std::string origin, permission, value;
    int container = 0;
};
std::span<const PermissionSpec> permission_catalog();
const PermissionSpec *find_permission(const std::string &id);
bool valid_permission_origin(const std::string &origin);
std::string extension_site_pattern(std::string input);
bool trustworthy_permission_origin(const std::string &origin);
std::string permission_scope(const std::string &requesting, const std::string &top,
                             const std::string &id);
std::string permission_override(const std::vector<SitePermission> &, const std::string &origin,
                                int container, const std::string &id);
std::string effective_permission(const BrowserPreferences &, const std::vector<SitePermission> &,
                                 const std::string &origin, int container, const std::string &id);
bool set_site_permission(std::vector<SitePermission> &, std::string origin, int container,
                         std::string id, std::string value);
} // namespace aura
