#pragma once

#include <quickjs.h>

namespace Script {

class Engine;

// Attach the `mb.layout.*` binding surface to the given `mb` object.
// Called once per JS context from Engine::setupGlobals. All bindings resolve
// the shared LayoutTree via the Engine pointer stored on the runtime opaque
// (engineFromCtx), so there is no per-context state to manage here.
void installLayoutBindings(Engine& eng, JSContext* ctx, JSValue mb);

} // namespace Script
