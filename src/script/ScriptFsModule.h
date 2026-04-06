#pragma once

#include <quickjs.h>

namespace Script { class Engine; }

JSModuleDef* createFsNativeModule(JSContext* ctx, Script::Engine* eng);
