#include "core/browser/permissions.hpp"
#include <algorithm>
#include <charconv>
#include <openssl/x509v3.h>
#include <string_view>
namespace aura {
namespace {
const PermissionSpec catalog[] = {
    {"camera", "Camera", &BrowserPreferences::camera},
    {"microphone", "Microphone", &BrowserPreferences::microphone},
    {"notifications", "Notifications", &BrowserPreferences::notifications, true, true},
    {"geolocation", "Location", &BrowserPreferences::geolocation},
    {"clipboard", "Clipboard", &BrowserPreferences::clipboard},
    {"midi", "MIDI devices", &BrowserPreferences::midi},
    {"sensors", "Motion sensors", &BrowserPreferences::sensors},
    {"camera_controls", "Camera movement and zoom", &BrowserPreferences::camera},
};
std::string_view host(const std::string &origin) {
    const auto start = origin.find("://");
    if (start == std::string::npos)
        return {};
    const auto authority = std::string_view(origin).substr(start + 3);
    if (authority.starts_with('['))
        return authority.substr(0, authority.find(']') + 1);
    return authority.substr(0, authority.find(':'));
}
} // namespace
std::span<const PermissionSpec> permission_catalog() { return catalog; }
const PermissionSpec *find_permission(const std::string &id) {
    for (const auto &spec : catalog)
        if (id == spec.id)
            return &spec;
    return nullptr;
}
bool valid_permission_origin(const std::string &origin) {
    const auto start = origin.starts_with("https://") ? 8 : origin.starts_with("http://") ? 7 : 0;
    if (!start || origin.size() <= std::size_t(start) || origin.size() > 2048 ||
        origin.find_first_of("/?#@\\ \r\n\t", start) != std::string::npos)
        return false;
    if (std::any_of(origin.begin(), origin.end(),
                    [](unsigned char c) { return c < 33 || c > 126; }))
        return false;
    const auto authority = origin.substr(start);
    std::string hostname, port;
    if (authority.starts_with('[')) {
        const auto end = authority.find(']');
        if (end == std::string::npos)
            return false;
        hostname = authority.substr(1, end - 1);
        auto address = a2i_IPADDRESS(hostname.c_str());
        const bool ipv6 = address && ASN1_STRING_length(address) == 16;
        ASN1_OCTET_STRING_free(address);
        if (!ipv6)
            return false;
        const auto rest = authority.substr(end + 1);
        if (!rest.empty()) {
            if (!rest.starts_with(':'))
                return false;
            port = rest.substr(1);
            if (port.empty())
                return false;
        }
    } else {
        const auto colon = authority.find(':');
        hostname = authority.substr(0, colon);
        if (hostname.empty() || hostname.size() > 253 || hostname.starts_with('.') ||
            hostname.ends_with('.') || hostname.find("..") != std::string::npos ||
            std::any_of(hostname.begin(), hostname.end(), [](char c) {
                return !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '.' && c != '-' &&
                       c != '_';
            }))
            return false;
        const auto last = hostname.substr(
            hostname.find_last_of('.') == std::string::npos ? 0 : hostname.find_last_of('.') + 1);
        if (std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            auto address = a2i_IPADDRESS(hostname.c_str());
            bool ipv4 = address && ASN1_STRING_length(address) == 4;
            if (ipv4) {
                const auto bytes = ASN1_STRING_get0_data(address);
                const auto canonical = std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) +
                                       "." + std::to_string(bytes[2]) + "." +
                                       std::to_string(bytes[3]);
                ipv4 = hostname == canonical;
            }
            ASN1_OCTET_STRING_free(address);
            if (!ipv4)
                return false;
        }
        if (colon != std::string::npos) {
            port = authority.substr(colon + 1);
            if (port.empty())
                return false;
        }
    }
    if (!port.empty()) {
        unsigned number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || number == 0 ||
            number > 65535 || port != std::to_string(number))
            return false;
        if ((start == 8 && number == 443) || (start == 7 && number == 80))
            return false;
    }
    return true;
}
bool trustworthy_permission_origin(const std::string &origin) {
    if (!valid_permission_origin(origin))
        return false;
    if (origin.starts_with("https://"))
        return true;
    auto hostname = host(origin);
    if (hostname == "localhost" || hostname.ends_with(".localhost"))
        return true;
    if (hostname.starts_with('[')) {
        auto address = a2i_IPADDRESS(std::string(hostname.substr(1, hostname.size() - 2)).c_str());
        const auto bytes = address ? ASN1_STRING_get0_data(address) : nullptr;
        const bool loopback = bytes && ASN1_STRING_length(address) == 16 &&
                              std::all_of(bytes, bytes + 15, [](auto c) { return c == 0; }) &&
                              bytes[15] == 1;
        ASN1_OCTET_STRING_free(address);
        return loopback;
    }
    if (!hostname.starts_with("127."))
        return false;
    auto address = a2i_IPADDRESS(std::string(hostname).c_str());
    const bool loopback = address && ASN1_STRING_length(address) == 4;
    ASN1_OCTET_STRING_free(address);
    return loopback;
}
std::string permission_scope(const std::string &requesting, const std::string &top,
                             const std::string &id) {
    const auto spec = find_permission(id);
    if (!spec)
        return {};
    const auto &origin = spec->requesting_origin ? requesting : top;
    return valid_permission_origin(origin)                                   ? origin
           : !spec->requesting_origin && valid_permission_origin(requesting) ? requesting
                                                                             : std::string{};
}
std::string permission_override(const std::vector<SitePermission> &rules, const std::string &origin,
                                int container, const std::string &id) {
    for (const auto &rule : rules)
        if (rule.origin == origin && rule.container == container && rule.permission == id)
            return rule.value;
    return "default";
}
std::string effective_permission(const BrowserPreferences &preferences,
                                 const std::vector<SitePermission> &rules,
                                 const std::string &origin, int container, const std::string &id) {
    const auto spec = find_permission(id);
    if (!spec || !trustworthy_permission_origin(origin))
        return "block";
    auto value = permission_override(rules, origin, container, id);
    return value == "default" ? preferences.*(spec->member) : value;
}
bool set_site_permission(std::vector<SitePermission> &rules, std::string origin, int container,
                         std::string id, std::string value) {
    const auto spec = find_permission(id);
    if (!spec || !valid_permission_origin(origin) || container < 0 || container > 1000000 ||
        (value != "default" && value != "block" && value != "allow" &&
         (value != "ask" || !spec->ask)) ||
        ((value == "allow" || value == "ask") && !trustworthy_permission_origin(origin)))
        return false;
    for (auto &rule : rules)
        if (rule.origin == origin && rule.container == container && rule.permission == id) {
            rule.value = std::move(value);
            return true;
        }
    if (rules.size() >= 4096)
        return false;
    rules.push_back({std::move(origin), std::move(id), std::move(value), container});
    return true;
}
} // namespace aura
