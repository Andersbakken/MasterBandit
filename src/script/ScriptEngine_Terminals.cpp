// Engine's Terminal-map methods, split out of ScriptEngine.cpp so Layout.cpp
// (which mb-tests links directly without pulling in QuickJS / libwebsockets)
// can resolve the symbols. Both mb and mb-tests link this TU.

#include "ScriptEngine.h"
#include "Terminal.h"

namespace Script {

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

} // namespace Script
