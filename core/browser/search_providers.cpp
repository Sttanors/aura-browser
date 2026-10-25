#include "core/browser/search_providers.hpp"
#include "core/browser/preferences.hpp"
#include <algorithm>
namespace aura {
bool valid_search_provider(const SearchProvider &p) {
    if (p.id <= 0 || p.id > 1000000 || p.name.empty() || p.name.size() > 120 || p.keyword.empty() ||
        p.keyword.size() > 16 || p.keyword[0] < 'a' || p.keyword[0] > 'z' ||
        std::any_of(p.name.begin(), p.name.end(),
                    [](unsigned char c) { return c < 32 || c == 127; }) ||
        !std::all_of(p.keyword.begin(), p.keyword.end(),
                     [](char c) {
                         return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                                c == '-';
                     }) ||
        p.url.size() > 4096)
        return false;
    const std::string token = "{searchTerms}";
    const auto pos = p.url.find(token);
    if (pos == std::string::npos || p.url.find(token, pos + token.size()) != std::string::npos)
        return false;
    // A query cannot change the provider's authority, credentials or port.
    const size_t authority = p.url.starts_with("https://")  ? 8
                             : p.url.starts_with("http://") ? 7
                                                            : 0;
    const auto end = p.url.find_first_of("/?#", authority);
    if (!authority || end == std::string::npos || pos <= end)
        return false;
    std::string example = p.url;
    example.replace(pos, token.size(), "query");
    return example.find_first_of("{}") == std::string::npos && valid_home_address(example);
}
std::string search_address(const SearchProvider &p, const std::string &query) {
    if (!valid_search_provider(p) || query.empty() || query.size() > 16384)
        return {};
    std::string encoded;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : query) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            encoded += c;
        else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    auto url = p.url;
    url.replace(url.find("{searchTerms}"), 13, encoded);
    return url;
}
const SearchProvider *find_search_provider(std::span<const SearchProvider> providers, int id) {
    for (const auto &p : providers)
        if (p.id == id)
            return &p;
    return nullptr;
}
} // namespace aura
