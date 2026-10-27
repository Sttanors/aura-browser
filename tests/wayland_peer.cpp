#include "include/core/SkCanvas.h"
#include "platform/linux/wayland/browser_window.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
namespace {
std::string clipboard_text, clipboard_status;
const void *payload(void *, const char *, size_t *size) {
    *size = clipboard_text.size();
    std::ofstream(clipboard_status) << *size;
    // Test-only external producer: hold the pipe open until the receiver changes UI.
    const auto pause = clipboard_status + ".pause";
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::filesystem::exists(pause) && std::chrono::steady_clock::now() < deadline)
        SDL_Delay(5);
    return clipboard_text.data();
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 3 || !std::getenv("AURA_TEST_ISOLATED_DISPLAY") || std::getenv("DISPLAY"))
        return 2;
    bool running = true;
    std::unique_ptr<aura::WaylandWindow> window;
    aura::WindowCallbacks cb;
    cb.draw = [](SkCanvas &c) { c.clear(0xff182033); };
    cb.close = [&] { running = false; };
    cb.key = [&](const CefKeyEvent &e) {
        if (e.type != KEYEVENT_RAWKEYDOWN)
            return false;
        if (e.windows_key_code == 0x71) {
            std::ifstream f(argv[2], std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), {});
            clipboard_text = std::move(text);
            clipboard_status = std::string(argv[2]) + ".status";
            const char *mimes[] = {"text/plain;charset=utf-8"};
            SDL_SetClipboardData(payload, nullptr, nullptr, mimes, 1);
            return true;
        }
        if (e.windows_key_code == 0x72) {
            char *text = SDL_GetClipboardText();
            std::ofstream(argv[2], std::ios::binary) << (text ? text : "");
            SDL_free(text);
            return true;
        }
        return false;
    };
    window = std::make_unique<aura::WaylandWindow>(900, 620, 1, std::move(cb), false, 1, false);
    window->show();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (running && std::chrono::steady_clock::now() < deadline) {
        window->dispatch();
        window->paint();
        window->wait(4);
    }
}
