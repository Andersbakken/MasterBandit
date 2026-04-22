// Regression tests for the tree-cutover target shape.
//
// After step 9 the tree looks like:
//
//   Container (root, vertical)
//   ├── TabBar (bound to tabsStack)
//   └── Stack (tabsStack)
//       └── Stack (tab 1)  ← activeChild
//           └── Container (content)
//               └── Terminal (first pane)
//
// Step 14's Done Criteria include a grep audit plus manual-smoke behavior.
// This file asserts the observable shape via the IPC stats endpoint, so any
// future structural regression (e.g. spawning tabs as Container children
// instead of Stack children, or losing the subtreeRoot UUID) fails CI
// rather than waiting for a human smoke run.

#include <doctest/doctest.h>
#include "MBConnection.h"
#include <glaze/glaze.hpp>
#include <string>

namespace {

// Pull the top-level `tabs` array from a stats-JSON payload.
const glz::generic::array_t* tabsArr(const glz::generic& j)
{
    auto* obj = std::get_if<glz::generic::object_t>(&j.data);
    if (!obj) return nullptr;
    auto it = obj->find("tabs");
    if (it == obj->end()) return nullptr;
    return std::get_if<glz::generic::array_t>(&it->second.data);
}

// UUID is 36 chars in canonical form, dashes at 8/13/18/23.
bool looksLikeUuid(const std::string& s)
{
    if (s.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else {
            char c = s[i];
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!hex) return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("tree-shape: startup has one tab with valid subtree nodeId"
          * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);
    rt.wait(200);

    std::string stats = rt.queryStats();
    REQUIRE(!stats.empty());

    glz::generic j;
    REQUIRE(glz::read_json(j, stats) == 0);

    const auto* tabs = tabsArr(j);
    REQUIRE(tabs != nullptr);
    // test_cwd may have left other tabs hanging; there's at least one, and
    // the first must be well-formed. Strict "== 1" would couple this test
    // to suite ordering, so just require ≥ 1 here.
    REQUIRE(tabs->size() >= 1);

    const auto* tab0 = std::get_if<glz::generic::object_t>(&(*tabs)[0].data);
    REQUIRE(tab0 != nullptr);

    auto nodeIdIt = tab0->find("nodeId");
    REQUIRE(nodeIdIt != tab0->end());
    const auto* nodeIdStr = std::get_if<std::string>(&nodeIdIt->second.data);
    REQUIRE(nodeIdStr != nullptr);
    CHECK(looksLikeUuid(*nodeIdStr));
}

TEST_CASE("tree-shape: active tab is exactly one and has at least one pane"
          * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);
    rt.wait(200);

    std::string stats = rt.queryStats();
    REQUIRE(!stats.empty());

    glz::generic j;
    REQUIRE(glz::read_json(j, stats) == 0);

    const auto* tabs = tabsArr(j);
    REQUIRE(tabs != nullptr);

    int activeCount = 0;
    int activePanes = -1;
    for (const auto& t : *tabs) {
        const auto* obj = std::get_if<glz::generic::object_t>(&t.data);
        if (!obj) continue;
        auto actIt = obj->find("active");
        if (actIt == obj->end()) continue;
        auto* active = std::get_if<bool>(&actIt->second.data);
        if (!active || !*active) continue;
        ++activeCount;
        auto panesIt = obj->find("panes");
        if (panesIt != obj->end()) {
            if (auto* panes = std::get_if<glz::generic::array_t>(&panesIt->second.data)) {
                activePanes = static_cast<int>(panes->size());
            }
        }
    }
    CHECK(activeCount == 1);
    CHECK(activePanes >= 1);
}

TEST_CASE("tree-shape: subtreeRoot UUIDs are distinct across tabs"
          * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);
    rt.reset();
    rt.wait(200);

    // Fire new_tab twice to ensure at least 3 tabs total.
    REQUIRE(rt.sendAction("new_tab"));
    rt.wait(500);
    REQUIRE(rt.sendAction("new_tab"));
    rt.wait(500);

    std::string stats = rt.queryStats();
    REQUIRE(!stats.empty());

    glz::generic j;
    REQUIRE(glz::read_json(j, stats) == 0);
    const auto* tabs = tabsArr(j);
    REQUIRE(tabs != nullptr);
    REQUIRE(tabs->size() >= 3);

    // Collect nodeId strings; they must all be present and pairwise unique.
    std::vector<std::string> ids;
    for (const auto& t : *tabs) {
        const auto* obj = std::get_if<glz::generic::object_t>(&t.data);
        if (!obj) continue;
        auto it = obj->find("nodeId");
        if (it == obj->end()) continue;
        if (auto* s = std::get_if<std::string>(&it->second.data)) {
            CHECK(looksLikeUuid(*s));
            ids.push_back(*s);
        }
    }
    REQUIRE(ids.size() == tabs->size());
    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t k = i + 1; k < ids.size(); ++k) {
            CHECK(ids[i] != ids[k]);
        }
    }

    // Close the spawned tabs to restore state for other tests in the shared
    // child process. close_tab closes the active tab; do it twice.
    rt.sendAction("close_tab");
    rt.wait(300);
    rt.sendAction("close_tab");
    rt.wait(300);
}
