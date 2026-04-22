// Engine's per-pane / per-tab ownership accessors, split out of
// ScriptEngine.cpp so Layout.cpp + Tab.cpp (which mb-tests links directly
// without pulling in QuickJS / libwebsockets) can resolve the symbols.
// Both mb and mb-tests link this TU.

#include "ScriptEngine.h"
#include "Layout.h"
#include "Terminal.h"

namespace Script {

namespace {
const std::string& kEmptyString() {
    static const std::string s;
    return s;
}
} // namespace

// --- Terminal map (keyed by tree node UUID) --------------------------------

::Terminal* Engine::terminal(Uuid nodeId)
{
    auto it = terminals_.find(nodeId);
    return it == terminals_.end() ? nullptr : it->second.get();
}

const ::Terminal* Engine::terminal(Uuid nodeId) const
{
    auto it = terminals_.find(nodeId);
    return it == terminals_.end() ? nullptr : it->second.get();
}

::Terminal* Engine::insertTerminal(Uuid nodeId, std::unique_ptr<::Terminal> t)
{
    if (!t || nodeId.isNil()) return nullptr;
    ::Terminal* raw = t.get();
    terminals_[nodeId] = std::move(t);
    return raw;
}

std::unique_ptr<::Terminal> Engine::extractTerminal(Uuid nodeId)
{
    auto it = terminals_.find(nodeId);
    if (it == terminals_.end()) return nullptr;
    std::unique_ptr<::Terminal> out = std::move(it->second);
    terminals_.erase(it);
    return out;
}

// --- Per-tab Layout map (keyed by tab subtreeRoot) -------------------------

::Layout* Engine::tabLayout(Uuid subtreeRoot)
{
    auto it = tabLayouts_.find(subtreeRoot);
    return it == tabLayouts_.end() ? nullptr : it->second.get();
}

const ::Layout* Engine::tabLayout(Uuid subtreeRoot) const
{
    auto it = tabLayouts_.find(subtreeRoot);
    return it == tabLayouts_.end() ? nullptr : it->second.get();
}

::Layout* Engine::insertTabLayout(Uuid subtreeRoot, std::unique_ptr<::Layout> l)
{
    if (!l || subtreeRoot.isNil()) return nullptr;
    ::Layout* raw = l.get();
    tabLayouts_[subtreeRoot] = std::move(l);
    return raw;
}

std::unique_ptr<::Layout> Engine::extractTabLayout(Uuid subtreeRoot)
{
    auto it = tabLayouts_.find(subtreeRoot);
    if (it == tabLayouts_.end()) return nullptr;
    std::unique_ptr<::Layout> out = std::move(it->second);
    tabLayouts_.erase(it);
    return out;
}

// --- Per-tab icon map ------------------------------------------------------

const std::string& Engine::tabIcon(Uuid subtreeRoot) const
{
    auto it = tabIcons_.find(subtreeRoot);
    return it == tabIcons_.end() ? kEmptyString() : it->second;
}

void Engine::setTabIcon(Uuid subtreeRoot, const std::string& s)
{
    if (subtreeRoot.isNil()) return;
    tabIcons_[subtreeRoot] = s;
}

void Engine::eraseTabIcon(Uuid subtreeRoot)
{
    tabIcons_.erase(subtreeRoot);
}

// --- Per-tab overlay stack -------------------------------------------------

bool Engine::hasTabOverlay(Uuid subtreeRoot) const
{
    auto it = tabOverlays_.find(subtreeRoot);
    return it != tabOverlays_.end() && !it->second.empty();
}

::Terminal* Engine::topTabOverlay(Uuid subtreeRoot)
{
    auto it = tabOverlays_.find(subtreeRoot);
    if (it == tabOverlays_.end() || it->second.empty()) return nullptr;
    return it->second.back().get();
}

void Engine::pushTabOverlay(Uuid subtreeRoot, std::unique_ptr<::Terminal> t)
{
    if (!t || subtreeRoot.isNil()) return;
    tabOverlays_[subtreeRoot].push_back(std::move(t));
}

std::unique_ptr<::Terminal> Engine::popTabOverlay(Uuid subtreeRoot)
{
    auto it = tabOverlays_.find(subtreeRoot);
    if (it == tabOverlays_.end() || it->second.empty()) return nullptr;
    std::unique_ptr<::Terminal> out = std::move(it->second.back());
    it->second.pop_back();
    if (it->second.empty()) tabOverlays_.erase(it);
    return out;
}

std::vector<std::unique_ptr<::Terminal>>
Engine::extractAllTabOverlays(Uuid subtreeRoot)
{
    auto it = tabOverlays_.find(subtreeRoot);
    if (it == tabOverlays_.end()) return {};
    std::vector<std::unique_ptr<::Terminal>> out = std::move(it->second);
    tabOverlays_.erase(it);
    return out;
}

} // namespace Script
