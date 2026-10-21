#include "platform/linux/x11/wayland_refresh.hpp"
#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
namespace {
struct Environment {
    const char *name;
    std::optional<std::string> previous;
    Environment(const char *key, const std::string &value) : name(key) {
        if (const auto old = std::getenv(key))
            previous = old;
        setenv(key, value.c_str(), 1);
    }
    ~Environment() {
        if (previous)
            setenv(name, previous->c_str(), 1);
        else
            unsetenv(name);
    }
};
}
TEST(WaylandRefresh, SilentCompositorNeverBlocksStartupOrPolling) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    Environment display("WAYLAND_DISPLAY", "aura-test-silent");
    Environment socket("WAYLAND_SOCKET", std::to_string(sockets[0]));
    const auto start = std::chrono::steady_clock::now();
    {
        aura::WaylandRefresh refresh; // wl_display_connect takes ownership of sockets[0].
        for (int i = 0; i < 100; ++i)
            EXPECT_EQ(refresh.rate({0, 0, 900, 620}), 0);
        close(sockets[1]);
        EXPECT_EQ(refresh.rate({0, 0, 900, 620}), 0);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(250));
}
