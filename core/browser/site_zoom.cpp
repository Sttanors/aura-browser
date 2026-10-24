#include "core/browser/site_zoom.hpp"
#include "core/browser/permissions.hpp"
#include <algorithm>
#include <cmath>
namespace aura {
std::optional<double> find_site_zoom(std::span<const SiteZoom> rules, const std::string &site,
                                     int container) {
    for (const auto &rule : rules)
        if (rule.site == site && rule.container == container)
            return rule.level;
    return {};
}
bool update_site_zoom(std::vector<SiteZoom> &rules, const std::string &site, int container,
                      std::optional<double> level) {
    if (container < 0 || container > 1000000 || site.empty() ||
        (site.starts_with('[') ? !site.ends_with(']') : site.find(':') != std::string::npos) ||
        !valid_permission_origin("https://" + site) ||
        (level && (!std::isfinite(*level) || *level < -4 || *level > 5)))
        return false;
    auto it = std::find_if(rules.begin(), rules.end(), [&](const auto &r) {
        return r.site == site && r.container == container;
    });
    if (!level) {
        if (it != rules.end())
            rules.erase(it);
    } else if (it != rules.end())
        it->level = *level;
    else {
        if (rules.size() >= 1000)
            return false;
        rules.push_back({site, container, *level});
    }
    return true;
}
} // namespace aura
