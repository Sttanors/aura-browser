#include "core/browser/container_rules.hpp"
#include "core/browser/permissions.hpp"
#include <algorithm>
namespace aura {
namespace {
bool valid_host(const std::string &host) {
    return !host.empty() && host.size() <= 253 &&
           host.find_first_of("/@?#\\") == std::string::npos &&
           (host.starts_with('[') ? host.ends_with(']') : host.find(':') == std::string::npos) &&
           std::none_of(host.begin(), host.end(),
                        [](unsigned char c) { return c <= 32 || (c >= 'A' && c <= 'Z'); }) &&
           valid_permission_origin("https://" + host);
}
} // namespace
std::optional<ContainerRule> match_container_rule(std::span<const ContainerRule> rules,
                                                  const std::string &host) {
    if (!valid_host(host))
        return {};
    std::optional<ContainerRule> best;
    for (const auto &rule : rules) {
        const bool matches = host == rule.host ||
                             (rule.subdomains && !rule.host.starts_with('[') &&
                              host.size() > rule.host.size() && host.ends_with("." + rule.host));
        if (matches && (!best || rule.host.size() > best->host.size()))
            best = rule;
    }
    return best;
}
bool set_container_rule(std::vector<ContainerRule> &rules, const std::string &host, int container,
                        bool subdomains) {
    if (!valid_host(host) || container < 0 || container > 1000000)
        return false;
    auto it =
        std::find_if(rules.begin(), rules.end(), [&](const auto &r) { return r.host == host; });
    if (it != rules.end())
        *it = {host, container, subdomains};
    else {
        if (rules.size() >= 256)
            return false;
        rules.push_back({host, container, subdomains});
    }
    return true;
}
} // namespace aura
