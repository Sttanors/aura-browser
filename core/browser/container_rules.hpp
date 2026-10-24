#pragma once
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace aura {
struct ContainerRule {
    std::string host;
    int container = 0;
    bool subdomains = false;
};
std::optional<ContainerRule> match_container_rule(std::span<const ContainerRule>,
                                                  const std::string &host);
bool set_container_rule(std::vector<ContainerRule> &, const std::string &host, int container,
                        bool subdomains);
} // namespace aura
