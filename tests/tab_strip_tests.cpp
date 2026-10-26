#include "core/browser/profile.hpp"
#include "core/browser/tab_strip.hpp"
#include <chrono>
#include <gtest/gtest.h>
using namespace aura;
namespace {
bool overlaps(Rect a, Rect b) { return intersect(a, b).width > 0 && intersect(a, b).height > 0; }
} // namespace
TEST(TabStrip, SupportedWidthsKeepSlotsAndControlsDisjointWithRoomToDrag) {
    for (int width = 900; width <= 2200; width += 37)
        for (int count = 0; count <= 35; ++count) {
            const auto layout = tab_strip_layout(width, count, 999);
            EXPECT_GE(layout.caption.width, 48);
            EXPECT_LE(layout.caption.x + layout.caption.width, width - 144);
            EXPECT_GE(layout.add.x, 12);
            EXPECT_LE(layout.add.x + layout.add.width, width - 144);
            int end = 0;
            for (const auto &slot : layout.slots) {
                EXPECT_GE(slot.bounds.x, end);
                end = slot.bounds.x + slot.bounds.width;
                EXPECT_GE(slot.bounds.width, 112);
                EXPECT_LE(slot.bounds.width, 220);
                EXPECT_FALSE(overlaps(slot.bounds, layout.add));
                EXPECT_FALSE(overlaps(slot.bounds, layout.previous));
                EXPECT_FALSE(overlaps(slot.bounds, layout.next));
                EXPECT_FALSE(overlaps(slot.bounds, layout.caption));
                EXPECT_GE(slot.position, 0);
                EXPECT_LT(slot.position, count);
            }
        }
}
TEST(TabStrip, OverflowCanReachBothEndsAndClampsStaleScroll) {
    const auto first = tab_strip_layout(900, 50, -50), last = tab_strip_layout(900, 50, 9000);
    ASSERT_TRUE(first.overflow);
    ASSERT_TRUE(last.overflow);
    ASSERT_FALSE(first.slots.empty());
    ASSERT_FALSE(last.slots.empty());
    EXPECT_EQ(first.slots.front().position, 0);
    EXPECT_EQ(last.slots.back().position, 49);
    const auto reduced = tab_strip_layout(900, 2, last.first);
    EXPECT_FALSE(reduced.overflow);
    EXPECT_EQ(reduced.first, 0);
    EXPECT_EQ(reduced.slots.size(), 2u);
}
TEST(TabStrip, NativeCaptionHitTestingRejectsEveryInteractiveControl) {
    for (int count : {1, 3, 50}) {
        const auto l = tab_strip_layout(900, count, 0);
        EXPECT_TRUE(tab_strip_caption(l, l.caption.x + l.caption.width / 2, 16));
        EXPECT_FALSE(tab_strip_caption(l, 899, 16));
        EXPECT_FALSE(tab_strip_caption(l, l.caption.x, 40));
        for (const auto &slot : l.slots)
            EXPECT_FALSE(tab_strip_caption(l, slot.bounds.x + slot.bounds.width / 2, 16));
        for (auto r : {l.add, l.previous, l.next})
            if (r.width)
                EXPECT_FALSE(tab_strip_caption(l, r.x + r.width / 2, 16));
    }
}
TEST(TabStrip, EmptyWorkspaceStillHasNewTabAndCaptionWithoutPhantomSlots) {
    const auto l = tab_strip_layout(900, 0, 20);
    EXPECT_TRUE(l.slots.empty());
    EXPECT_FALSE(l.overflow);
    EXPECT_EQ(l.first, 0);
    EXPECT_GT(l.add.width, 0);
    EXPECT_GT(l.caption.width, 0);
}
TEST(TabStrip, LayoutPreferenceValidatesAndSurvivesProfileRoundTrip) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("aura-strip-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Profile p(path);
    const auto setting = find_setting("tab_layout");
    ASSERT_NE(setting, nullptr);
    EXPECT_EQ(p.tab_layout, "vertical");
    ASSERT_TRUE(set_setting(p, *setting, "horizontal"));
    EXPECT_FALSE(set_setting(p, *setting, "diagonal"));
    EXPECT_EQ(p.tab_layout, "horizontal");
    p.save();
    Profile q(path);
    q.load();
    EXPECT_EQ(q.tab_layout, "horizontal");
    std::filesystem::remove_all(path);
}
