#pragma once

#include "ScriptEngine.h" // Engine + Engine::Instance

#include <quickjs.h>

#include <string>

JSModuleDef *createFsNativeModule(JSContext *ctx, Script::Engine *eng);

// Validate that `rawPath` is writable by `inst` under the same sandbox
// rules as fs.writeFileSync: built-in scripts may write anywhere; user
// scripts may write only under <configDir>/<scriptStem>/. Returns the
// path string to use for the actual write (currently unchanged from
// `rawPath` — see ScriptFsModule.cpp for the rationale and TOCTOU
// caveat). On failure throws a TypeError into `ctx` and returns an
// empty string. Note: does NOT check the FsWrite permission bit; the
// caller is responsible (the public fs.* functions check it; non-fs
// callers like pane.writeRangeToFile do their own gating). This helper
// is shared so non-fs APIs that produce files can plug into the same
// sandbox without duplicating the path logic.
std::string scriptCheckWritePath(JSContext *ctx,
                                 Script::Engine *eng,
                                 Script::Engine::Instance *inst,
                                 const std::string &rawPath);

// Ensure the parent directory of `filePath` exists (recursive). Returns
// false on failure (and logs via spdlog). Used by writers that
// auto-create the destination directory.
bool scriptEnsureParentDir(const std::string &filePath);
