#include <doctest/doctest.h>
#include "MBConnection.h"
#include <glaze/glaze.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

struct StatsTab {
    int index = -1;
    std::string nodeId;
    bool active = false;
};

std::vector<StatsTab> parseTabs(const std::string& statsJson)
{
    std::vector<StatsTab> out;
    glz::generic j;
    if (glz::read_json(j, statsJson)) return out;
    auto* obj = std::get_if<glz::generic::object_t>(&j.data);
    if (!obj) return out;
    auto tabsIt = obj->find("tabs");
    if (tabsIt == obj->end()) return out;
    auto* tabs = std::get_if<glz::generic::array_t>(&tabsIt->second.data);
    if (!tabs) return out;
    for (size_t i = 0; i < tabs->size(); ++i) {
        auto* t = std::get_if<glz::generic::object_t>(&(*tabs)[i].data);
        if (!t) continue;
        StatsTab s;
        s.index = static_cast<int>(i);
        if (auto it = t->find("nodeId"); it != t->end()) {
            if (auto* str = std::get_if<std::string>(&it->second.data)) s.nodeId = *str;
        }
        if (auto it = t->find("active"); it != t->end()) {
            if (auto* b = std::get_if<bool>(&it->second.data)) s.active = *b;
        }
        out.push_back(std::move(s));
    }
    return out;
}

const StatsTab* activeTab(const std::vector<StatsTab>& v)
{
    for (const auto& t : v) if (t.active) return &t;
    return nullptr;
}

} // namespace

TEST_CASE("tabs UUID: each new tab gets a unique UUID" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);
    rt.reset();
    rt.wait(500);

    auto tabs0 = parseTabs(rt.queryStats());
    REQUIRE(tabs0.size() == 1);
    REQUIRE(!tabs0[0].nodeId.empty());

    REQUIRE(rt.sendAction("new_tab"));
    rt.wait(500);
    REQUIRE(rt.sendAction("new_tab"));
    rt.wait(500);

    auto tabs2 = parseTabs(rt.queryStats());
    REQUIRE(tabs2.size() == 3);

    std::unordered_set<std::string> uniq;
    for (const auto& t : tabs2) {
        REQUIRE(!t.nodeId.empty());
        // UUIDs are 36 chars: 8-4-4-4-12
        CHECK(t.nodeId.size() == 36);
        uniq.insert(t.nodeId);
    }
    CHECK(uniq.size() == 3);
    // Original tab UUID from before survives (no positional remap).
    CHECK(uniq.count(tabs0[0].nodeId) == 1);

    // Restore.
    rt.sendAction("close_tab");
    rt.wait(300);
    rt.sendAction("close_tab");
    rt.wait(300);
}

TEST_CASE("tabs UUID: positional activate via index resolves to the right UUID" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);
    rt.reset();
    rt.wait(500);

    REQUIRE(rt.sendAction("new_tab")); rt.wait(300);
    REQUIRE(rt.sendAction("new_tab")); rt.wait(300);

    auto tabs = parseTabs(rt.queryStats());
    REQUIRE(tabs.size() == 3);

    // After two `new_tab`s the third tab is active.
    const auto* cur = activeTab(tabs);
    REQUIRE(cur);
    CHECK(cur->index == 2);
    const std::string activeUuid2 = cur->nodeId;

    // Use the keybinding-style Action::ActivateTab path: arg is a positional
    // index resolved through `_tabUuidByIndex` in default-ui.js. Index 0
    // should land us on the original tab.
    REQUIRE(rt.sendAction("activate_tab", {"0"}));
    rt.wait(300);

    auto tabs2 = parseTabs(rt.queryStats());
    const auto* cur2 = activeTab(tabs2);
    REQUIRE(cur2);
    CHECK(cur2->index == 0);
    CHECK(cur2->nodeId == tabs[0].nodeId);

    // Index 2 → back to the third tab; verify the UUID matches the one we
    // captured before any reordering.
    REQUIRE(rt.sendAction("activate_tab", {"2"}));
    rt.wait(300);
    auto tabs3 = parseTabs(rt.queryStats());
    const auto* cur3 = activeTab(tabs3);
    REQUIRE(cur3);
    CHECK(cur3->nodeId == activeUuid2);

    rt.sendAction("close_tab"); rt.wait(300);
    rt.sendAction("close_tab"); rt.wait(300);
}

TEST_CASE("tabs UUID: closing a non-last tab preserves remaining UUIDs" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);
    rt.reset();
    rt.wait(500);

    REQUIRE(rt.sendAction("new_tab")); rt.wait(300);
    REQUIRE(rt.sendAction("new_tab")); rt.wait(300);

    auto before = parseTabs(rt.queryStats());
    REQUIRE(before.size() == 3);
    const std::string uuid0 = before[0].nodeId;
    const std::string uuid2 = before[2].nodeId;

    // Close the middle tab. Default-ui's closeTab(args.index) resolves
    // positionally → tabs[1] is the target.
    REQUIRE(rt.sendAction("close_tab", {"1"}));
    rt.wait(500);

    auto after = parseTabs(rt.queryStats());
    REQUIRE(after.size() == 2);
    // The remaining tabs keep their original UUIDs even though their
    // positional index shifted.
    CHECK(after[0].nodeId == uuid0);
    CHECK(after[1].nodeId == uuid2);

    rt.sendAction("close_tab"); rt.wait(300);
}
