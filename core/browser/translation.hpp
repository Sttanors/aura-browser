#pragma once
#include <string>
namespace aura {
std::string translation_target(const std::string &target, const std::string &interface_language);
std::string translation_url(const std::string &provider, const std::string &target, bool page,
                            const std::string &content);
} // namespace aura
