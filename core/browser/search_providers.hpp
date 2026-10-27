#pragma once
#include <span>
#include <string>
namespace aura {
struct SearchProvider {
    int id = 0;
    std::string name, keyword, url;
};
bool valid_search_provider(const SearchProvider &);
std::string search_address(const SearchProvider &, const std::string &query);
const SearchProvider *find_search_provider(std::span<const SearchProvider>, int id);
} // namespace aura
