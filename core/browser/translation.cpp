#include "core/browser/translation.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
namespace aura {
namespace {
constexpr std::array<std::string_view, 7> languages{"en", "ru", "de", "es", "fr", "zh", "ja"};
bool valid_language(const std::string &language) {
    return std::find(languages.begin(), languages.end(), language) != languages.end();
}
std::string encode(const std::string &text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            result += char(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}
} // namespace
std::string translation_target(const std::string &target, const std::string &interface_language) {
    auto value = target == "interface" ? (interface_language == "ru" ? "ru" : "en") : target;
    if (!valid_language(value))
        throw std::invalid_argument("Choose a supported translation language.");
    return value;
}
std::string translation_url(const std::string &provider, const std::string &target, bool page,
                            const std::string &content) {
    if (provider != "google" && provider != "bing")
        throw std::invalid_argument("Choose a translation provider.");
    if (!valid_language(target))
        throw std::invalid_argument("Choose a supported translation language.");
    if (page) {
        if (provider != "google")
            throw std::invalid_argument(
                "Microsoft translates selected text here. Choose Google Translate for pages.");
        if (content.size() > 16384 ||
            (!content.starts_with("https://") && !content.starts_with("http://")) ||
            content.find_first_of("\r\n\t") != std::string::npos)
            throw std::invalid_argument("Translation is available for HTTP and HTTPS pages.");
        const auto start = content.find("://") + 3, end = content.find_first_of("/?#", start);
        const auto authority = content.substr(start, end == std::string::npos ? end : end - start);
        if (authority.empty() || authority.find('@') != std::string::npos)
            throw std::invalid_argument("Translation is available for HTTP and HTTPS pages.");
        return "https://translate.google.com/translate?sl=auto&tl=" + target +
               "&u=" + encode(content);
    }
    if (content.empty() || content.find_first_not_of(" \r\n\t") == std::string::npos)
        throw std::invalid_argument("Select some text on the page first.");
    if (content.size() > 5000)
        throw std::invalid_argument("Select a shorter passage to translate.");
    if (provider == "bing")
        return "https://www.bing.com/translator?from=auto&to=" +
               (target == "zh" ? std::string("zh-Hans") : target) + "&text=" + encode(content);
    return "https://translate.google.com/?sl=auto&tl=" + target + "&text=" + encode(content) +
           "&op=translate";
}
} // namespace aura
