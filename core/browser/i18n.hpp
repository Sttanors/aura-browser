#pragma once
#include <span>
#include <string>
#include <string_view>
namespace aura {
struct Translation {
    std::string_view english, russian;
};
std::span<const Translation> translations();
std::string translate(std::string_view language, std::string_view english);
} // namespace aura
