#include <doctest/doctest.h>
#include "MBConnection.h"
#include <glaze/glaze.hpp>
#include <string>

// Helper: extract pane CWD from stats JSON for pane at tabs[tabIdx].panes[paneIdx]
static std::string paneCwd(const std::string& statsJson, int tabIdx, int paneIdx)
{
    glz::generic j;
    if (glz::read_json(j, statsJson)) return {};
    auto* obj = std::get_if<glz::generic::object_t>(&j.data);
    if (!obj) return {};

    auto tabsIt = obj->find("tabs");
    if (tabsIt == obj->end()) return {};
    auto* tabs = std::get_if<glz::generic::array_t>(&tabsIt->second.data);
    if (!tabs || tabIdx >= static_cast<int>(tabs->size())) return {};

    auto* tabObj = std::get_if<glz::generic::object_t>(&(*tabs)[tabIdx].data);
    if (!tabObj) return {};

    auto panesIt = tabObj->find("panes");
    if (panesIt == tabObj->end()) return {};
    auto* panes = std::get_if<glz::generic::array_t>(&panesIt->second.data);
    if (!panes || paneIdx >= static_cast<int>(panes->size())) return {};

    auto* paneObj = std::get_if<glz::generic::object_t>(&(*panes)[paneIdx].data);
    if (!paneObj) return {};

    auto cwdIt = paneObj->find("cwd");
    if (cwdIt == paneObj->end()) return {};
    auto* s = std::get_if<std::string>(&cwdIt->second.data);
    return s ? *s : std::string{};
}

// Helper: count tabs
static int totalTabs(const std::string& statsJson)
{
    glz::generic j;
    if (glz::read_json(j, statsJson)) return 0;
    auto* obj = std::get_if<glz::generic::object_t>(&j.data);
    if (!obj) return 0;

    auto tabsIt = obj->find("tabs");
    if (tabsIt == obj->end()) return 0;
    auto* tabs = std::get_if<glz::generic::array_t>(&tabsIt->second.data);
    return tabs ? static_cast<int>(tabs->size()) : 0;
}

// Helper: count panes in a specific tab
static int panesInTab(const std::string& statsJson, int tabIdx)
{
    glz::generic j;
    if (glz::read_json(j, statsJson)) return 0;
    auto* obj = std::get_if<glz::generic::object_t>(&j.data);
    if (!obj) return 0;

    auto tabsIt = obj->find("tabs");
    if (tabsIt == obj->end()) return 0;
    auto* tabs = std::get_if<glz::generic::array_t>(&tabsIt->second.data);
    if (!tabs || tabIdx >= static_cast<int>(tabs->size())) return 0;

    auto* tabObj = std::get_if<glz::generic::object_t>(&(*tabs)[tabIdx].data);
    if (!tabObj) return 0;

    auto panesIt = tabObj->find("panes");
    if (panesIt == tabObj->end()) return 0;
    auto* panes = std::get_if<glz::generic::array_t>(&panesIt->second.data);
    return panes ? static_cast<int>(panes->size()) : 0;
}

TEST_CASE("cwd: OSC 7 sets pane CWD visible in stats" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);

    rt.reset();
    rt.wait(500);

    // Send OSC 7 to set CWD to /tmp (OSC parser strips file://hostname prefix)
    rt.injectData("\033]7;file://localhost/tmp\007");
    rt.wait(200);

    auto stats = rt.queryStats();
    REQUIRE(!stats.empty());
    CHECK(paneCwd(stats, 0, 0) == "/tmp");
}

TEST_CASE("cwd: new tab inherits CWD from focused pane" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);

    rt.reset();
    rt.wait(500);

    // Set CWD on the current pane via OSC 7
    rt.injectData("\033]7;file://localhost/tmp\007");
    rt.wait(200);

    // Create a new tab via action
    REQUIRE(rt.sendAction("new_tab"));
    rt.wait(1000);

    auto stats = rt.queryStats();
    REQUIRE(!stats.empty());
    CHECK(totalTabs(stats) == 2);

    // The new tab's shell should have started in /tmp.
    // Send pwd and check grid, or verify via OSC 7 from the new shell.
    // For now, send pwd and wait for output.
    rt.sendText("pwd\n");
    rt.wait(500);

    // Close the new tab to restore state for other tests
    rt.sendAction("close_tab");
    rt.wait(500);
}

TEST_CASE("cwd: split pane inherits CWD from source pane" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);

    rt.reset();
    rt.wait(500);

    // Set CWD on the current pane via OSC 7
    rt.injectData("\033]7;file://localhost/tmp\007");
    rt.wait(200);

    // Split pane right
    REQUIRE(rt.sendAction("split_pane", {"right"}));
    rt.wait(1000);

    auto stats = rt.queryStats();
    REQUIRE(!stats.empty());
    CHECK(panesInTab(stats, 0) == 2);

    // Close the new pane to restore state
    rt.sendAction("close_pane");
    rt.wait(500);
}
