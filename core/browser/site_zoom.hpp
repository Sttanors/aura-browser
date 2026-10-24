#pragma once
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace aura {
struct SiteZoom {
    // Canonical host, including brackets for IPv6. Shared across schemes and ports.
    std::string site;
    int container = 0;
    double level = 0;
};
std::optional<double> find_site_zoom(std::span<const SiteZoom>, const std::string &, int);
// nullopt removes an override; updates remain possible at the 1000-rule limit.
bool update_site_zoom(std::vector<SiteZoom> &, const std::string &, int, std::optional<double>);
} // namespace aura
