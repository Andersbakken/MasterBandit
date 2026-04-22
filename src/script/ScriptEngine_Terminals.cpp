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

// --- paneId ↔ Uuid global index --------------------------------------------

int Engine::allocatePaneId()
{
    return nextPaneId_++;
}

void Engine::registerPaneSlot(int paneId, Uuid nodeId)
{
    if (paneId < 0 || nodeId.isNil()) return;
    paneIdToUuid_[paneId] = nodeId;
    uuidToPaneId_[nodeId] = paneId;
}

void Engine::unregisterPaneSlot(int paneId)
{
    auto it = paneIdToUuid_.find(paneId);
    if (it == paneIdToUuid_.end()) return;
    uuidToPaneId_.erase(it->second);
    paneIdToUuid_.erase(it);
}

Uuid Engine::uuidForPaneId(int paneId) const
{
    auto it = paneIdToUuid_.find(paneId);
    return it == paneIdToUuid_.end() ? Uuid{} : it->second;
}

int Engine::paneIdForUuid(Uuid nodeId) const
{
    auto it = uuidToPaneId_.find(nodeId);
    return it == uuidToPaneId_.end() ? -1 : it->second;
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

} // namespace Script
