#include "common/config.hpp"
#include <toml++/toml.hpp>
#include <stdexcept>
namespace aura {
Config load_config(const std::filesystem::path& path) {
    Config result;
    if (!std::filesystem::exists(path)) return result;
    const auto table = toml::parse_file(utf8_path(path));
    result.width = table["window"]["width"].value_or(result.width);
    result.height = table["window"]["height"].value_or(result.height);
    result.aura = table["appearance"]["aura"].value_or(result.aura);
    result.url = table["browser"]["url"].value_or(result.url);
    if (result.width < 800 || result.width > 3840 || result.height < 600 || result.height > 2160)
        throw std::runtime_error("Window size outside M0 diagnostic limits");
    if (result.aura != "aurora" && result.aura != "ember" && result.aura != "monolith")
        throw std::runtime_error("Unknown aura");
    return result;
}
std::string json_escape(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result;
}
std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::filesystem::path path_from_utf8(const std::string& value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
}
