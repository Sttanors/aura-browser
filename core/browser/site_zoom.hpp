#pragma once
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace aura {
struct SiteZoom {
    std::string site;
    int container = 0;
    double level = 0;
};
std::optional<double> find_site_zoom(std::span<const SiteZoom>, const std::string &, int);
bool update_site_zoom(std::vector<SiteZoom> &, const std::string &, int, std::optional<double>);
} // namespace aura
