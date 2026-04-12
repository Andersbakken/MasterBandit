#pragma once

#include <quickjs.h>

#include <cstdint>

namespace Script {
class Engine;
using InstanceId = uint64_t;
}

// Register the `mb:ws` module with the given context.
JSModuleDef* createWsNativeModule(JSContext* ctx, Script::Engine* eng);

// Called from Engine::unload to close all WS servers + connections owned by
// the instance being unloaded. Safe to call when no servers exist.
void wsUnloadInstance(Script::Engine* eng, Script::InstanceId id);

// Called from Engine destructor to tear down the shared lws_context and any
// remaining module state for this engine.
void wsDestroyEngine(Script::Engine* eng);
