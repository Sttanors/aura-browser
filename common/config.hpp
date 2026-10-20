#pragma once
#include <filesystem>
#include <string>
namespace aura {
struct Config {
    int width = 1280;
    int height = 800;
    std::string aura = "aurora";
    std::string url = "https://example.com/";
};
Config load_config(const std::filesystem::path& path);
std::string json_escape(const std::string& text);
std::string utf8_path(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(const std::string& value);
}
