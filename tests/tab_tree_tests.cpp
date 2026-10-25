#include "core/browser/profile.hpp"
#include "core/browser/tab_tree.hpp"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
using namespace aura;
namespace {
struct TreeProfile : testing::Test {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("aura-tree-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(path); }
    void TearDown() override { std::filesystem::remove_all(path); }
};
std::vector<int> order(const std::vector<TreeRow> &rows) {
    std::vector<int> result;
    for (const auto &r : rows)
        result.push_back(r.index);
    return result;
}
} // namespace
TEST(TabTree, PreorderPreservesSiblingOrderEvenWhenParentsAppearLater) {
    const std::vector<TreeTab> tabs{{2, 1}, {5}, {4, 2}, {1}, {3, 1}};
    const auto rows = tab_tree_rows(tabs);
    EXPECT_EQ(order(rows), (std::vector<int>{1, 3, 0, 2, 4}));
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[1].descendants, 3);
    EXPECT_EQ(rows[3].depth, 2);
    EXPECT_EQ(rows[2].descendants, 1);
}
TEST(TabTree, CollapseHidesDescendantsAndPreservesNestedCollapse) {
    std::vector<TreeTab> tabs{
        {1, 0, 0, 0, 0, false, true}, {2, 1, 0, 0, 0, false, true}, {3, 2}, {4}};
    EXPECT_EQ(order(tab_tree_rows(tabs)), (std::vector<int>{0, 3}));
    EXPECT_EQ(tab_tree_rows(tabs)[0].descendants, 2);
    EXPECT_EQ(tab_tree_rows(tabs, false).size(), 4u);
    tabs[0].collapsed = false;
    EXPECT_EQ(order(tab_tree_rows(tabs)), (std::vector<int>{0, 1, 3}));
    EXPECT_TRUE(tabs[1].collapsed);
}
TEST(TabTree, RejectsCyclesMissingParentsAndSelfReferencesWithoutLosingTabs) {
    const std::vector<TreeTab> tabs{{1, 2}, {2, 3}, {3, 1}, {4, 2}, {5, 88}, {6, 6}};
    EXPECT_EQ(tab_tree_parents(tabs), (std::vector<int>{-1, -1, -1, 1, -1, -1}));
    EXPECT_EQ(tab_tree_rows(tabs).size(), tabs.size());
}
TEST(TabTree, ContextBoundariesAndAmbiguousParentIdsNeverCreateEdges) {
    const std::vector<TreeTab> tabs{
        {1}, {2, 1, 1}, {3, 1, 0, 1}, {4, 1, 0, 0, 1}, {5, 1, 0, 0, 0, true}, {6, 1}, {6}, {7, 6}};
    EXPECT_EQ(tab_tree_parents(tabs), (std::vector<int>{-1, -1, -1, -1, -1, 0, -1, -1}));
}
TEST(TabTree, DeepUntrustedChainsUseAnIterativeTraversal) {
    std::vector<TreeTab> tabs;
    for (int i = 1; i <= 20000; ++i)
        tabs.push_back({i, i - 1});
    const auto rows = tab_tree_rows(tabs);
    ASSERT_EQ(rows.size(), tabs.size());
    EXPECT_EQ(rows.front().descendants, 19999);
    EXPECT_EQ(rows.back().depth, 19999);
}
TEST(TabTree, FilterRemapsParentsActiveAndSplitWithoutAttachingToAnotherTab) {
    SavedWindow w{{{"https://a.test", false, 0, false, 0, 0, 2},
                   {"https://b.test", false, 0, false, 0, 0, 1},
                   {"https://c.test", false, 0, false, 0, 0, -1}},
                  2,
                  0,
                  0,
                  2};
    remap_saved_tabs(w, std::vector<int>{0, 2, 4});
    EXPECT_EQ(w.tabs[0].parent, 1);
    EXPECT_EQ(w.tabs[1].parent, -1);
    EXPECT_EQ(w.active, 1);
    EXPECT_EQ(w.split_left, 0);
    EXPECT_EQ(w.split_right, 1);
}
TEST_F(TreeProfile, ProfileRoundTripPreservesWindowLegacyAndSnapshotTrees) {
    Profile p(path);
    const int space = p.add_workspace("Research"), group = p.add_group(space, "Tree");
    SavedWindow w{{{"https://parent.test", true, space, false, group, 0, -1, true},
                   {"https://child.test", false, space, true, group, 0, 0}},
                  1,
                  space};
    p.windows = {w};
    p.session = w.tabs;
    p.active = 1;
    const int snapshot = p.save_named_session("Branch", {w});
    ASSERT_GT(snapshot, 0);
    p.tree_tabs = false;
    p.save();
    Profile q(path);
    q.load();
    ASSERT_EQ(q.windows.size(), 1u);
    EXPECT_EQ(q.windows[0].tabs[1].parent, 0);
    EXPECT_TRUE(q.windows[0].tabs[0].tree_collapsed);
    EXPECT_EQ(q.session[1].parent, 0);
    EXPECT_FALSE(q.tree_tabs);
    const auto restored = q.prepare_named_session(snapshot);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->at(0).tabs[1].parent, 0);
    EXPECT_TRUE(restored->at(0).tabs[0].tree_collapsed);
}
TEST_F(TreeProfile, LoadingFilteredAndCorruptedParentsCannotChangeTheirMeaning) {
    std::ofstream(path / "profile.toml") << R"(
[[windows]]
active=2
[[windows.tabs]]
url="https://removed.test"
container=999
[[windows.tabs]]
url="https://parent.test"
[[windows.tabs]]
url="https://child.test"
parent=1
[[windows.tabs]]
url="https://orphan.test"
parent=0
[[windows.tabs]]
url="https://cycle.test"
parent=4
)";
    Profile p(path);
    p.load();
    ASSERT_EQ(p.windows.size(), 1u);
    const auto &w = p.windows[0];
    ASSERT_EQ(w.tabs.size(), 4u);
    EXPECT_EQ(w.active, 1);
    EXPECT_EQ(w.tabs[1].parent, 0);
    EXPECT_EQ(w.tabs[2].parent, -1);
    EXPECT_EQ(w.tabs[3].parent, -1);
}
TEST_F(TreeProfile, RemovingContainerRemapsUnaffectedBranches) {
    Profile p(path);
    const int container = p.add_container("Work");
    p.windows = {SavedWindow{{{"https://removed.test", false, 0, false, 0, container},
                              {"https://parent.test"},
                              {"https://child.test", false, 0, false, 0, 0, 1}},
                             2}};
    ASSERT_TRUE(p.remove_container(container));
    const auto &w = p.windows[0];
    ASSERT_EQ(w.tabs.size(), 2u);
    EXPECT_EQ(w.tabs[1].parent, 0);
    EXPECT_EQ(w.active, 1);
}
TEST_F(TreeProfile, SnapshotsRejectMalformedTreesInsteadOfSilentlyRewritingThem) {
    Profile p(path);
    SavedWindow w{{{"https://parent.test"}, {"https://child.test", false, 0, false, 0, 0, 0}}};
    ASSERT_GT(p.save_named_session("Valid", {w}), 0);
    w.tabs[0].parent = 1;
    EXPECT_EQ(p.save_named_session("Cycle", {w}), -1);
    w.tabs[0].parent = -1;
    w.tabs[1].container = p.add_container("Other");
    EXPECT_EQ(p.save_named_session("Cross context", {w}), -1);
    w.tabs[1].parent = -2;
    EXPECT_EQ(p.save_named_session("Negative parent", {w}), -1);
}
