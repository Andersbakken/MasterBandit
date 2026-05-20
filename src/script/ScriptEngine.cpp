#include "ScriptEngine.h"
#include "Action.h"
#include "LayoutTree.h"
#include "ScriptFsModule.h"
#include "ScriptLayoutBindings.h"
#include "ScriptWsModule.h"
#include "Sha256.h"
#include "Terminal.h"
#include "TsTransform.h"
#include "Utils.h"
#include "Uuid.h"

#include <eventloop/EventLoop.h>
#include <quickjs.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef __APPLE__
#include <stdlib.h> // arc4random_buf
#else
#include <sys/random.h> // getrandom
#endif
#include <unistd.h> // gethostname

namespace fs = std::filesystem;

static void fillSecureRandom(unsigned char *buf, size_t len)
{
#ifdef __APPLE__
    arc4random_buf(buf, len);
#else
    size_t off = 0;
    while (off < len) {
        ssize_t r = getrandom(buf + off, len - off, 0);
        if (r > 0) {
            off += static_cast<size_t>(r);
        } else if (r < 0 && errno != EINTR) {
            // Fall back to /dev/urandom if getrandom is unavailable.
            std::ifstream f("/dev/urandom", std::ios::binary);
            f.read(reinterpret_cast<char *>(buf + off), static_cast<std::streamsize>(len - off));
            break;
        }
    }
#endif
}

namespace Script {

static spdlog::logger &sLog()
{
    static auto l = spdlog::get("script");
    return l ? *l : *spdlog::default_logger();
}

// ============================================================================
// Module loader — resolves and validates import paths
// ============================================================================

// Resolve a canonical path, rejecting symlinks that escape the sandbox.
// Returns empty string if the path is outside the allowed directory.
static std::string resolveAndValidate(const std::string &path, const std::string &allowedDir)
{
    std::error_code ec;

    // Resolve the allowed directory to a canonical path
    auto canonicalAllowed = fs::canonical(allowedDir, ec);
    if (ec) {
        return {};
    }

    // Check if the target exists (without following the final symlink yet)
    if (!fs::exists(path, ec) || ec) {
        return {};
    }

    // Resolve to canonical path (resolves all symlinks)
    auto canonicalPath = fs::canonical(path, ec);
    if (ec) {
        return {};
    }

    // Verify the resolved path is under the allowed directory
    auto rel = canonicalPath.lexically_relative(canonicalAllowed);
    if (rel.empty() || rel.string().starts_with("..")) {
        return {};
    }

    return canonicalPath.string();
}

// Module name normalizer: resolves import specifiers to absolute paths.
// Called by QuickJS before the module loader.
static char *moduleNormalize(JSContext *ctx, const char *base_name,
                             const char *name, void *opaque)
{
    auto *eng = static_cast<Engine *>(opaque);

    // Built-in module (mb:fs, mb:tui, etc.)
    if (strncmp(name, "mb:", 3) == 0) {
        // Native C++ modules — returned as-is, handled in moduleLoader
        static const char *kNativeModules[] = { "fs", "ws", nullptr };
        std::string moduleName              = name + 3;
        for (int i = 0; kNativeModules[i]; ++i) {
            if (moduleName == kNativeModules[i]) {
                return js_strdup(ctx, name); // pass through unchanged
            }
        }

        // JS file modules in builtinModulesDir
        const auto &modulesDir = eng->builtinModulesDir();
        if (modulesDir.empty()) {
            JS_ThrowReferenceError(ctx, "No built-in modules directory configured");
            return nullptr;
        }
        std::string modulePath = modulesDir + "/" + moduleName + ".js";
        auto resolved          = resolveAndValidate(modulePath, modulesDir);
        if (resolved.empty()) {
            JS_ThrowReferenceError(ctx, "Built-in module '%s' not found", name);
            return nullptr;
        }
        return js_strdup(ctx, resolved.c_str());
    }

    // Relative import — resolve against the importing script's directory
    fs::path basePath(base_name);
    fs::path baseDir  = basePath.parent_path();
    fs::path resolved = baseDir / name;

    // Add .js / .ts extension if not present. Prefer .ts when both exist on
    // disk so a TS author who imports `./foo` (the JS convention) gets the
    // typed source rather than a stale sibling .js. Falls back to .js when
    // neither exists so error messages stay consistent with the pre-TS world.
    if (!resolved.has_extension()) {
        std::error_code ec;
        fs::path tsCandidate = resolved;
        tsCandidate.replace_extension(".ts");
        if (fs::exists(tsCandidate, ec) && !ec) {
            resolved = tsCandidate;
        } else {
            resolved.replace_extension(".js");
        }
    }

    std::string resolvedStr = resolved.string();

    // Validate: must be under the script's directory or the built-in modules dir
    auto *inst = eng->findInstanceByCtx(ctx);
    if (!inst) {
        JS_ThrowReferenceError(ctx, "Module '%s': no script context for import", name);
        return nullptr;
    }

    fs::path scriptDir = fs::path(inst->path).parent_path();
    auto validated     = resolveAndValidate(resolvedStr, scriptDir.string());
    if (validated.empty()) {
        // Try built-in modules dir as fallback
        const auto &modulesDir = eng->builtinModulesDir();
        if (!modulesDir.empty()) {
            validated = resolveAndValidate(resolvedStr, modulesDir);
        }
    }
    if (validated.empty()) {
        JS_ThrowReferenceError(ctx, "Module '%s' is outside allowed directories", name);
        return nullptr;
    }

    char *result = js_strdup(ctx, validated.c_str());
    return result;
}

// Module loader: reads the source file for a resolved module path.
static JSModuleDef *moduleLoader(JSContext *ctx, const char *module_name, void *opaque)
{
    auto *eng = static_cast<Engine *>(opaque);

    // Native C++ modules
    if (strcmp(module_name, "mb:fs") == 0) {
        return createFsNativeModule(ctx, eng);
    }
    if (strcmp(module_name, "mb:ws") == 0) {
        return createWsNativeModule(ctx, eng);
    }

    // mb::tsx::loadAsJs handles .ts files transparently (type-stripped via
    // whiteout, with a per-source-hash disk cache). For .js files this is a
    // straight read. Distinguish read failures (err.message empty) from
    // whiteout failures so JS sees the precise reason: ReferenceError for
    // missing/unreadable files (matches the pre-TS behaviour and is what
    // QuickJS itself emits for unresolvable imports), SyntaxError carrying
    // the whiteout diagnostic for type-strip failures.
    mb::tsx::TransformError tsErr;
    std::string src = mb::tsx::loadAsJs(module_name, &tsErr);
    if (src.empty()) {
        if (!tsErr.message.empty()) {
            JS_ThrowSyntaxError(ctx,
                                "TypeScript transform failed for '%s' at byte %zu: %s",
                                module_name,
                                tsErr.offset,
                                tsErr.message.c_str());
        } else {
            JS_ThrowReferenceError(ctx, "Cannot load module '%s'", module_name);
        }
        return nullptr;
    }

    JSValue func = JS_Eval(ctx, src.c_str(), src.size(), module_name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) {
        return nullptr;
    }

    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return m;
}

static Engine *engineFromCtx(JSContext *ctx)
{
    return static_cast<Engine *>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

static Engine::Instance *instanceFromCtx(JSContext *ctx)
{
    auto *eng    = engineFromCtx(ctx);
    uintptr_t id = reinterpret_cast<uintptr_t>(JS_GetContextOpaque(ctx));
    return eng->findInstance(static_cast<InstanceId>(id));
}

static bool checkPerm(JSContext *ctx, uint32_t required)
{
    auto *inst = instanceFromCtx(ctx);
    return inst && (inst->permissions & required) != 0;
}

// Schedule termination of a script that violated permissions
static void scheduleTermination(JSContext *ctx)
{
    auto *eng  = engineFromCtx(ctx);
    auto *inst = instanceFromCtx(ctx);
    if (!inst || inst->builtIn) {
        return;
    }
    InstanceId id = inst->id;
    sLog().error("ScriptEngine: terminating '{}' (id={}) for permission violation",
                 inst->path,
                 id);
    if (!eng->loop()) {
        eng->unload(id);
        return;
    }
    eng->loop()->addTimer(0, false, [eng, id]()
                          {
                              eng->unload(id);
                          });
}

#define REQUIRE_PERM(ctx, perm)                                                        \
    do {                                                                               \
        if (!checkPerm(ctx, Script::Perm::perm)) {                                     \
            scheduleTermination(ctx);                                                  \
            return JS_ThrowTypeError(ctx, "permission denied: " #perm " not granted"); \
        }                                                                              \
    } while (0)

// ============================================================================
// Pane JS class
// ============================================================================

static JSClassID jsPaneClassId;

struct JsPaneData
{
    PaneId id;
    bool alive;
};

static void jsPaneFinalize(JSRuntime *, JSValue val)
{
    delete static_cast<JsPaneData *>(JS_GetOpaque(val, jsPaneClassId));
}

static JSClassDef jsPaneClassDef = { "Pane", jsPaneFinalize };

// Memoised per-uuid (per-ctx) — returns the same JS object on every call
// for a given PaneId so listeners and other per-pane state attach to a
// stable identity. Two `mb.activePane` reads in a row are `===`. The cache
// lives at `__pane_proxies` on the global; cleared per pane in
// `cleanupPane`. The cached entry holds one ref, so the JS object survives
// until cleanupPane fires (or the ctx is torn down with the script).
static JSValue jsPaneNew(JSContext *ctx, PaneId id)
{
    std::string key = id.toString();
    JSValue global  = JS_GetGlobalObject(ctx);
    JSValue cache   = JS_GetPropertyStr(ctx, global, "__pane_proxies");
    if (JS_IsUndefined(cache)) {
        cache = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__pane_proxies", JS_DupValue(ctx, cache));
    }
    JS_FreeValue(ctx, global);
    JSValue existing = JS_GetPropertyStr(ctx, cache, key.c_str());
    if (!JS_IsUndefined(existing)) {
        JS_FreeValue(ctx, cache);
        return existing;
    }
    JS_FreeValue(ctx, existing);
    JSValue obj = JS_NewObjectClass(ctx, jsPaneClassId);
    JS_SetOpaque(obj, new JsPaneData { id, true });
    JS_SetPropertyStr(ctx, cache, key.c_str(), JS_DupValue(ctx, obj));
    JS_FreeValue(ctx, cache);
    return obj;
}

static JsPaneData *jsPaneGet(JSContext *ctx, JSValueConst val)
{
    return static_cast<JsPaneData *>(JS_GetOpaque(val, jsPaneClassId));
}

// Register a JS object in a global registry by integer key, so filters can find it.
static void registerInGlobal(JSContext *ctx, const char *registryName,
                             uint32_t key, JSValueConst obj)
{
    JSValue global   = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, registryName);
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, registryName, JS_DupValue(ctx, registry));
    }
    // Only set if not already registered
    JSValue existing = JS_GetPropertyUint32(ctx, registry, key);
    if (JS_IsUndefined(existing)) {
        JS_SetPropertyUint32(ctx, registry, key, JS_DupValue(ctx, obj));
    }
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);
}

// String-keyed variant — needed for pane/tab registries since pane ids are
// now Uuids (their string form) rather than ints.
static void registerInGlobal(JSContext *ctx, const char *registryName,
                             const std::string &key, JSValueConst obj)
{
    JSValue global   = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, registryName);
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, registryName, JS_DupValue(ctx, registry));
    }
    JSValue existing = JS_GetPropertyStr(ctx, registry, key.c_str());
    if (JS_IsUndefined(existing)) {
        JS_SetPropertyStr(ctx, registry, key.c_str(), JS_DupValue(ctx, obj));
    }
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);
}

// Remove a callback from a JS array by identity (indexOf + splice).
static void removeFromJSArray(JSContext *ctx, JSValue arr, JSValueConst fn)
{
    if (JS_IsUndefined(arr)) {
        return;
    }
    JSValue indexOfFn = JS_GetPropertyStr(ctx, arr, "indexOf");
    JSValue idxVal    = JS_Call(ctx, indexOfFn, arr, 1, &fn);
    JS_FreeValue(ctx, indexOfFn);
    int32_t idx = -1;
    JS_ToInt32(ctx, &idx, idxVal);
    JS_FreeValue(ctx, idxVal);
    if (idx >= 0) {
        JSValue spliceFn = JS_GetPropertyStr(ctx, arr, "splice");
        JSValue args[2]  = { JS_NewInt32(ctx, idx), JS_NewInt32(ctx, 1) };
        JSValue res      = JS_Call(ctx, spliceFn, arr, 2, args);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, spliceFn);
    }
}

static JSValue jsPaneAddEventListener(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
    }
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }

    auto *inst        = instanceFromCtx(ctx);
    InstanceId instId = inst ? inst->id : 0;

    std::string prop;
    if (strcmp(event, "output") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterOutput)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: IoFilterOutput");
        }
        prop = "__output_filters";
        eng->addPaneOutputFilter(pane->id, instId);
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput");
        }
        prop = "__input_filters";
        eng->addPaneInputFilter(pane->id, instId);
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "mouse") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: ui");
        }
        prop = "__mouse_listeners";
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "mousemove") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: ui");
        }
        prop = "__evt_mousemove";
        eng->addPaneMouseMoveListener(pane->id, instId);
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "commandComplete") == 0) {
        if (!checkPerm(ctx, Perm::ShellReadCommands)) {
            JS_FreeCString(ctx, event);
            scheduleTermination(ctx);
            return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
        }
        prop = std::string("__evt_") + event;
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "rowEvicted") == 0) {
        // Eviction notification reveals scrollback churn rate even
        // without text access, but more importantly is only useful
        // alongside getTextFromRows / linkAt — gating both under the
        // same bit keeps the permission story coherent. Prefer the
        // soft-fail style (return false) over scheduleTermination here:
        // unlike shell.commands, an unprivileged script subscribing to
        // rowEvicted is a misconfiguration, not necessarily an attack.
        if (!checkPerm(ctx, Perm::PaneRead)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: pane.read not granted");
        }
        prop = std::string("__evt_") + event;
        eng->addPaneRowEvictedListener(pane->id, instId);
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else {
        prop = std::string("__evt_") + event;
        // Register for lifecycle events too (so destroyed can be found)
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    }
    JS_FreeCString(ctx, event);

    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    if (JS_IsUndefined(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, prop.c_str(), JS_DupValue(ctx, arr));
    }
    JSValue pushFn = JS_GetPropertyStr(ctx, arr, "push");
    JS_Call(ctx, pushFn, arr, 1, &argv[1]);
    JS_FreeValue(ctx, pushFn);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsPaneRemoveEventListener(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
    }
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }
    std::string prop;
    bool isRowEvicted = false;
    if (strcmp(event, "output") == 0) {
        prop = "__output_filters";
    } else if (strcmp(event, "input") == 0) {
        prop = "__input_filters";
    } else if (strcmp(event, "mouse") == 0) {
        prop = "__mouse_listeners";
    } else {
        prop         = std::string("__evt_") + event;
        isRowEvicted = (strcmp(event, "rowEvicted") == 0);
    }
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    // Capture pre-remove length to detect whether anything actually
    // matched — we only decrement the row-eviction counter on a real
    // removal (matches the existing semantics of removeFromJSArray
    // which is idempotent when the listener isn't registered).
    int preLen  = 0;
    if (isRowEvicted) {
        JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
        JS_ToInt32(ctx, &preLen, lenVal);
        JS_FreeValue(ctx, lenVal);
    }
    removeFromJSArray(ctx, arr, argv[1]);
    if (isRowEvicted) {
        JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
        int postLen    = 0;
        JS_ToInt32(ctx, &postLen, lenVal);
        JS_FreeValue(ctx, lenVal);
        if (postLen < preLen) {
            Engine *eng       = engineFromCtx(ctx);
            auto *inst        = instanceFromCtx(ctx);
            InstanceId instId = inst ? inst->id : 0;
            eng->removePaneRowEvictedListener(pane->id, instId);
        }
    }
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsPaneWrite(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "write requires (string)");
    }
    REQUIRE_PERM(ctx, ShellWrite);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneHasPty(pane->id)) {
        return JS_ThrowTypeError(ctx, "pane has no PTY");
    }
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) {
        return JS_EXCEPTION;
    }
    eng->callbacks().writePaneToShell(pane->id, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue jsPanePaste(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "paste requires (string)");
    }
    REQUIRE_PERM(ctx, ShellWrite);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneHasPty(pane->id)) {
        return JS_ThrowTypeError(ctx, "pane has no PTY");
    }
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) {
        return JS_EXCEPTION;
    }
    eng->callbacks().pastePaneText(pane->id, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// getText extracts text from a logical-line-id range with col bounds.
using GetTextFn = std::function<std::string(uint64_t startLineId, int startCol,
                                            uint64_t endLineId, int endCol)>;

// ----- Command object class -----
//
// Each entry in pane.commands / pane.selectedCommand / commandComplete event
// is an instance of a JS class ("Command") backed by CmdObjData. Scalar
// properties (id, cwd, exitCode, timings, position objects) are cheap getters
// that read from the opaque struct. The two expensive properties (.command
// and .output) decode text on first access and cache the resulting JSValue,
// so iterating with `.map(c => c.id)` / `.find(c => c.id === X)` never pays
// the text-extraction cost.
//
// Class id + finalizer + class def are declared up here (near other class
// infra) so the Engine ctor can reference them. The getter body, proto table,
// and buildCommandObject live together further down.

struct CmdObjData
{
    Script::CommandInfo info;
    Script::PaneId paneId;
    // Sentinel means "not decoded yet". Freed via JS_FreeValueRT in finalizer.
    JSValue cachedCommand = JS_UNINITIALIZED;
    JSValue cachedOutput  = JS_UNINITIALIZED;
};

static JSClassID jsCommandClassId;

static void jsCommandFinalize(JSRuntime *rt, JSValue val)
{
    auto *d = static_cast<CmdObjData *>(JS_GetOpaque(val, jsCommandClassId));
    if (!d) {
        return;
    }
    JS_FreeValueRT(rt, d->cachedCommand);
    JS_FreeValueRT(rt, d->cachedOutput);
    delete d;
}

static JSClassDef jsCommandClassDef = { "Command", jsCommandFinalize };

// Forward-declare so createContext() can install it as the class proto.
extern const JSCFunctionListEntry jsCommandProto[];
extern const size_t jsCommandProtoCount;

// Callers pass the owning paneId; at property-access time the getter uses
// engineFromCtx(ctx) + Engine::callbacks().paneGetText(paneId, ..), which
// safely returns an empty string if the pane has been destroyed.
static JSValue buildCommandObject(JSContext *ctx, const Script::CommandInfo &p, PaneId paneId);

static JSValue jsPaneGetTextFromRows(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "getTextFromRows requires (startRowId, startCol, endRowId, endCol)");
    }
    // Reading arbitrary scrollback ranges exposes credentials, history,
    // and command output. Gate under pane.read (formerly pane.selection).
    REQUIRE_PERM(ctx, PaneRead);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneGetText) {
        return JS_NewString(ctx, "");
    }
    uint64_t startId = 0, endId = 0;
    int32_t startCol = 0, endCol = 0;
    JS_ToIndex(ctx, &startId, argv[0]);
    JS_ToInt32(ctx, &startCol, argv[1]);
    JS_ToIndex(ctx, &endId, argv[2]);
    JS_ToInt32(ctx, &endCol, argv[3]);
    auto text = eng->callbacks().paneGetText(pane->id, startId, startCol, endId, endCol);
    return JS_NewStringLen(ctx, text.data(), text.size());
}

// pane.writeRangeToFile(path, startRowId, startCol, endRowId, endCol) -> number
//
// Stream the same text getTextFromRows would return directly to a
// file. Atomic on success: writes to <path>.tmp under the same
// fs-sandbox rules as fs.writeFileSync, then renames into place.
//
// PERMISSIONS: PaneRead (the text we're reading) AND FsWrite (the
// destination we're writing). Failing either short-circuits before
// any I/O — `path` is not even resolved until both bits are checked.
//
// SANDBOX: identical to fs.writeFileSync — built-ins may write
// anywhere, user scripts may write only under <configDir>/<scriptStem>/.
//
// THROWS on missing args, permission denial, sandbox violation,
// path-resolution failure, or write failure. Returns the number of
// bytes written on success (== text.size()).
//
// Forward declarations for the MbOutputCapture class infrastructure
// defined further down (between the embedded-class block and the
// Terminal-base proto). jsPaneCaptureOutputToFile (immediately
// below) needs to construct a handle, and Engine::notifyCaptureStopped
// (much further down) needs to read its opaque data.
struct JsOutputCaptureData;
static JSValue jsOutputCaptureNew(JSContext *ctx,
                                  PaneId paneId,
                                  const std::string &path,
                                  const std::string &format,
                                  InstanceId instId);
static JsOutputCaptureData *jsOutputCaptureGet(JSContext *ctx, JSValueConst val);

// NOT STREAMING: the entire range is materialised in memory first.
// For unbounded ranges this is the same memory profile as
// getTextFromRows. A future enhancement could iterate logical lines
// from Document and write per-row, capping resident memory; not
// worth the complexity until a concrete use case shows up.
static JSValue jsPaneWriteRangeToFile(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    if (argc < 5) {
        return JS_ThrowTypeError(ctx,
                                 "writeRangeToFile requires (path, startRowId, startCol, endRowId, endCol)");
    }

    // Gate before extracting *any* arguments to avoid leaking even
    // path strings via thrown error formatting.
    REQUIRE_PERM(ctx, PaneRead);
    REQUIRE_PERM(ctx, FsWrite);

    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    auto *inst  = eng->findInstanceByCtx(ctx);
    if (!inst) {
        return JS_ThrowTypeError(ctx, "writeRangeToFile: no script context");
    }

    // Path
    const char *rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) {
        return JS_EXCEPTION;
    }
    std::string pathStr(rawPath);
    JS_FreeCString(ctx, rawPath);

    // Range
    uint64_t startId = 0, endId = 0;
    int32_t startCol = 0, endCol = 0;
    if (JS_ToIndex(ctx, &startId, argv[1]) < 0) {
        return JS_EXCEPTION;
    }
    if (JS_ToInt32(ctx, &startCol, argv[2]) < 0) {
        return JS_EXCEPTION;
    }
    if (JS_ToIndex(ctx, &endId, argv[3]) < 0) {
        return JS_EXCEPTION;
    }
    if (JS_ToInt32(ctx, &endCol, argv[4]) < 0) {
        return JS_EXCEPTION;
    }

    // Sandbox-validate. scriptCheckWritePath enforces the
    // <configDir>/<stem>/ containment for user scripts; built-ins are
    // unrestricted. FsWrite was already pre-checked above.
    std::string validatedPath = scriptCheckWritePath(ctx, eng, inst, pathStr);
    if (validatedPath.empty()) {
        return JS_EXCEPTION;
    }

    // Pull text via the same callback getTextFromRows uses. Empty
    // when paneGetText is unwired (headless tests) or when the
    // range is empty / out of range — match getTextFromRows behaviour
    // and write an empty file in that case rather than throwing,
    // since "the range had no text" is a legitimate result.
    std::string text;
    if (eng->callbacks().paneGetText) {
        text = eng->callbacks().paneGetText(pane->id, startId, startCol, endId, endCol);
    }

    // Auto-create parent dir (matches fs.writeFileSync). Failure
    // here is logged inside scriptEnsureParentDir.
    if (!scriptEnsureParentDir(validatedPath)) {
        return JS_ThrowTypeError(ctx, "writeRangeToFile: cannot create directory for '%s'", validatedPath.c_str());
    }

    // Atomic write: <path>.tmp -> rename. The .tmp lands in the same
    // directory so rename is a single inode swap (no cross-device
    // copy). On crash the partial .tmp remains; the user's prior file
    // is untouched. We don't fsync — the kernel will flush on close,
    // and a crash mid-write is rare enough that the cost of fsync on
    // every applet save isn't justified. If durability matters more
    // than throughput, callers should fsync explicitly via fs APIs
    // (which they don't have for this path; future work).
    std::string tmpPath = validatedPath + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            return JS_ThrowTypeError(ctx, "writeRangeToFile: cannot open '%s'", tmpPath.c_str());
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) {
            return JS_ThrowTypeError(ctx, "writeRangeToFile: write failed for '%s'", tmpPath.c_str());
        }
        // RAII close on scope exit.
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, validatedPath, ec);
    if (ec) {
        // Best-effort cleanup; ignore secondary failure.
        std::error_code _ignore;
        std::filesystem::remove(tmpPath, _ignore);
        return JS_ThrowTypeError(ctx,
                                 "writeRangeToFile: rename '%s' -> '%s' failed: %s",
                                 tmpPath.c_str(),
                                 validatedPath.c_str(),
                                 ec.message().c_str());
    }

    return JS_NewInt64(ctx, static_cast<int64_t>(text.size()));
}

// pane.captureOutputToFile(path, opts?) -> MbOutputCapture
//
// Open a streaming capture of this pane's PTY output. Writes are
// applied at the post-coalesce / pre-filter point inside Terminal,
// so the captured byte stream matches what the emulator sees
// regardless of script-level output filters.
//
// PERMISSIONS: PaneRead (the source content) AND FsWrite (the
// destination file). Both pre-checked before the path is even
// touched, so a permission failure can't leak even the raw path
// through error formatting.
//
// SANDBOX: validated by scriptCheckWritePath — same rules as
// fs.writeFileSync. Built-in scripts unrestricted; user scripts
// confined to <configDir>/<scriptStem>/ or /tmp/masterbandit/.
//
// UNIQUENESS: a Terminal accepts multiple concurrent captures, but
// each must point at a distinct path. Calling with a path already
// in use throws — the caller is expected to stop the previous
// capture first (or pick a different path).
//
// FORMAT: opts.format is "raw" (default) or "asciicast".
//   - "raw": every byte the emulator received, written verbatim.
//   - "asciicast": asciicast v2 (https://docs.asciinema.org/...).
//                  Header line written at open; each PTY chunk
//                  becomes one [<elapsed_seconds>, "o", <chunk>]
//                  record, JSON-escaped per RFC 8259.
//
// AUTO-STOP: if a write fails (disk full, EIO, fd revoked), the
// capture is closed and a "stopped" event fires on the returned
// handle with reason="io-error" and the strerror text. Other
// captures on the same Terminal are unaffected.
//
// CLEANUP: the capture is tracked on the calling instance's
// owned-resource list; unloading the script auto-stops it.
static JSValue jsPaneCaptureOutputToFile(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx,
                                 "captureOutputToFile requires (path: string, opts?: object)");
    }
    REQUIRE_PERM(ctx, PaneRead);
    REQUIRE_PERM(ctx, FsWrite);

    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    auto *inst  = eng->findInstanceByCtx(ctx);
    if (!inst) {
        return JS_ThrowTypeError(ctx,
                                 "captureOutputToFile: no script context");
    }

    Terminal *t = eng->terminal(pane->id);
    if (!t) {
        return JS_ThrowTypeError(ctx,
                                 "captureOutputToFile: pane has no live Terminal");
    }

    // path
    const char *rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) {
        return JS_EXCEPTION;
    }
    std::string pathStr(rawPath);
    JS_FreeCString(ctx, rawPath);

    // opts.format (default "raw")
    std::string formatStr = "raw";
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (!JS_IsObject(argv[1])) {
            return JS_ThrowTypeError(ctx,
                                     "captureOutputToFile: opts must be an object");
        }
        JSValue f = JS_GetPropertyStr(ctx, argv[1], "format");
        if (!JS_IsUndefined(f) && !JS_IsNull(f)) {
            if (!JS_IsString(f)) {
                JS_FreeValue(ctx, f);
                return JS_ThrowTypeError(ctx,
                                         "captureOutputToFile: opts.format must be a string");
            }
            const char *s = JS_ToCString(ctx, f);
            if (s) {
                formatStr = s;
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, f);
    }

    Terminal::CaptureFormat cf;
    if (formatStr == "raw") {
        cf = Terminal::CaptureFormat::Raw;
    } else if (formatStr == "asciicast") {
        cf = Terminal::CaptureFormat::Asciicast;
    } else if (formatStr == "text") {
        cf = Terminal::CaptureFormat::Text;
    } else {
        return JS_ThrowTypeError(ctx,
                                 "captureOutputToFile: opts.format must be \"raw\", \"asciicast\", or \"text\" (got \"%s\")",
                                 formatStr.c_str());
    }

    // Sandbox-validate the path. scriptCheckWritePath already pre-
    // creates /tmp/masterbandit/ if the path lands there, and
    // produces a TypeError on sandbox violation.
    std::string validatedPath = scriptCheckWritePath(ctx, eng, inst, pathStr);
    if (validatedPath.empty()) {
        return JS_EXCEPTION;
    }

    // Auto-create destination directory before fopen — without this
    // a path like /tmp/masterbandit/captures/foo.cast would fail
    // at addOutputCapture's fopen, even though the sandbox check
    // accepted it.
    if (!scriptEnsureParentDir(validatedPath)) {
        return JS_ThrowTypeError(ctx,
                                 "captureOutputToFile: cannot create directory for '%s'",
                                 validatedPath.c_str());
    }

    std::string err;
    bool ok = t->addOutputCapture(validatedPath, cf, &err);
    if (!ok) {
        return JS_ThrowTypeError(ctx,
                                 "captureOutputToFile: %s",
                                 err.c_str());
    }

    // Track on the instance for cleanup-on-unload.
    inst->ownedOutputCaptures.push_back({ pane->id, validatedPath });

    // Build the handle. Register it in a global registry keyed by
    // (paneId:path) so notifyCaptureStopped can find it later.
    JSValue handle     = jsOutputCaptureNew(ctx, pane->id, validatedPath, formatStr, inst->id);
    std::string regKey = pane->id.toString() + ":" + validatedPath;
    registerInGlobal(ctx, "__capture_registry", regKey, handle);
    return handle;
}

static JSValue jsPaneRowIdAt(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "rowIdAt requires (screenRow)");
    }
    // rowId is only useful as input to text/link extraction or for
    // cross-referencing selection/command records. Gate consistently
    // under pane.read so a text-denied script can't probe row positions.
    REQUIRE_PERM(ctx, PaneRead);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng       = engineFromCtx(ctx);
    auto info         = eng->callbacks().paneInfo(pane->id);
    int32_t screenRow = 0;
    JS_ToInt32(ctx, &screenRow, argv[0]);
    if (screenRow < 0 || screenRow >= info.rows) {
        return JS_NULL;
    }
    if (!eng->callbacks().paneLineIdAt) {
        return JS_NULL;
    }
    auto result = eng->callbacks().paneLineIdAt(pane->id, screenRow);
    if (!result.has_value()) {
        return JS_NULL;
    }
    return JS_NewInt64(ctx, static_cast<int64_t>(*result));
}

// pane.findText(needle, opts?) -> Array<{startRowId, startCol, endRowId, endCol}>
//
// Walks scrollback + visible grid for substring or regex hits. Each match
// is confined to one logical line; startRowId === endRowId always. Cell
// columns are logical-line offsets (cumulative across visual wraps),
// directly usable as decoration startCol/endCol. Empty needle, missing
// pane, or regex syntax error yield an empty array. Requires `pane.read`.
static JSValue jsPaneFindText(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "findText requires (needle: string, opts?)");
    }
    REQUIRE_PERM(ctx, PaneRead);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneFindText) {
        return JS_NewArray(ctx);
    }

    const char *needleC = JS_ToCString(ctx, argv[0]);
    if (!needleC) {
        return JS_EXCEPTION;
    }
    std::string needle(needleC);
    JS_FreeCString(ctx, needleC);

    Script::AppCallbacks::FindOptions opts;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        auto readBool = [&](const char *name, bool &out)
        {
            JSValue v = JS_GetPropertyStr(ctx, argv[1], name);
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                out = JS_ToBool(ctx, v) != 0;
            }
            JS_FreeValue(ctx, v);
        };
        readBool("regex", opts.regex);
        readBool("caseSensitive", opts.caseSensitive);
        readBool("wholeWord", opts.wholeWord);
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "limit");
        if (!JS_IsUndefined(lv) && !JS_IsNull(lv)) {
            int32_t n = 0;
            if (JS_ToInt32(ctx, &n, lv) >= 0) {
                opts.limit = n;
            }
        }
        JS_FreeValue(ctx, lv);
    }

    auto matches = eng->callbacks().paneFindText(pane->id, needle, opts);
    JSValue arr  = JS_NewArray(ctx);
    for (size_t i = 0; i < matches.size(); ++i) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "startRowId", JS_NewInt64(ctx, static_cast<int64_t>(matches[i].startLineId)));
        JS_SetPropertyStr(ctx, obj, "startCol", JS_NewInt32(ctx, matches[i].startCol));
        JS_SetPropertyStr(ctx, obj, "endRowId", JS_NewInt64(ctx, static_cast<int64_t>(matches[i].endLineId)));
        JS_SetPropertyStr(ctx, obj, "endCol", JS_NewInt32(ctx, matches[i].endCol));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

// pane.scrollToRow(rowId) -> boolean
//
// Scrolls the pane's viewport so the given logical row is at the top.
// Returns true iff the viewport actually changed; false if the row was
// evicted, the row is already in the live viewport, or the row is already
// at the visible top. Triggers a redraw when the viewport changed.
// Requires `pane.read` (mutating viewport position is observable to TUI
// apps; same gating shape as the rest of the read-side scrollback API).
static JSValue jsPaneScrollToRow(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "scrollToRow requires (rowId)");
    }
    REQUIRE_PERM(ctx, PaneRead);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneScrollToRow) {
        return JS_NewBool(ctx, false);
    }
    int64_t rowId = 0;
    if (JS_ToInt64(ctx, &rowId, argv[0]) < 0) {
        return JS_EXCEPTION;
    }
    bool changed = eng->callbacks().paneScrollToRow(pane->id, static_cast<uint64_t>(rowId));
    if (changed) {
        if (auto &cb = eng->callbacks().requestRedraw) {
            cb();
        }
    }
    return JS_NewBool(ctx, changed);
}

static JSValue jsPaneLinkAt(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "linkAt requires (rowId, col)");
    }
    REQUIRE_PERM(ctx, PaneRead);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneUrlAt) {
        return JS_NULL;
    }
    uint64_t lineId = 0;
    int32_t col     = 0;
    JS_ToIndex(ctx, &lineId, argv[0]);
    JS_ToInt32(ctx, &col, argv[1]);
    auto url = eng->callbacks().paneUrlAt(pane->id, lineId, col);
    if (url.empty()) {
        return JS_NULL;
    }
    return JS_NewStringLen(ctx, url.data(), url.size());
}

static JSValue jsPaneGetLinksFromRows(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "getLinksFromRows requires (startRowId, endRowId, limit?)");
    }
    REQUIRE_PERM(ctx, PaneRead);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneGetLinksFromRows) {
        return JS_NewArray(ctx);
    }
    uint64_t startId = 0, endId = 0;
    int32_t limit = 0;
    JS_ToIndex(ctx, &startId, argv[0]);
    JS_ToIndex(ctx, &endId, argv[1]);
    if (argc >= 3) {
        JS_ToInt32(ctx, &limit, argv[2]);
    }
    auto links  = eng->callbacks().paneGetLinksFromRows(pane->id, startId, endId, limit);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < links.size(); ++i) {
        const auto &l = links[i];
        JSValue obj   = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "url", JS_NewStringLen(ctx, l.url.data(), l.url.size()));
        JS_SetPropertyStr(ctx, obj, "startRowId", JS_NewInt64(ctx, static_cast<int64_t>(l.startLineId)));
        JS_SetPropertyStr(ctx, obj, "startCol", JS_NewInt32(ctx, l.startCol));
        JS_SetPropertyStr(ctx, obj, "endRowId", JS_NewInt64(ctx, static_cast<int64_t>(l.endLineId)));
        JS_SetPropertyStr(ctx, obj, "endCol", JS_NewInt32(ctx, l.endCol));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static JSValue jsPaneGetProp(JSContext *ctx, JSValueConst this_val, int magic)
{
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_UNDEFINED;
    }
    Engine *eng = engineFromCtx(ctx);
    auto info   = eng->callbacks().paneInfo(pane->id);
    switch (magic) {
        case 0: {
            std::string s = pane->id.toString();
            return JS_NewStringLen(ctx, s.data(), s.size());
        }
        case 1: return JS_NewInt32(ctx, info.cols);
        case 2: return JS_NewInt32(ctx, info.rows);
        case 3: return JS_NewString(ctx, info.title.c_str());
        case 4: return JS_NewString(ctx, info.cwd.c_str());
        case 5: return JS_NewBool(ctx, info.hasPty);
        case 6: return JS_NewBool(ctx, info.focused);
        case 7: return info.focusedPopupId.empty()
            ? JS_NULL
            : JS_NewString(ctx, info.focusedPopupId.c_str());
        case 8: return JS_NewString(ctx, info.foregroundProcess.c_str());
        case 9: { // selectedCommand — full record of the OSC 133 command currently
                  // highlighted via Cmd+click / Cmd+double-click / scroll_to_prompt,
                  // or null if no selection. Gated on shell.commands (same as .commands
                  // and .selectedCommandId — both of which expose the same underlying
                  // id, just in lighter form).
            if (!checkPerm(ctx, Perm::ShellReadCommands)) {
                scheduleTermination(ctx);
                return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
            }
            if (!info.selectedCommandId) {
                return JS_NULL;
            }
            if (!eng->callbacks().paneCommands) {
                return JS_NULL;
            }
            auto list       = eng->callbacks().paneCommands(pane->id, 0);
            uint64_t target = *info.selectedCommandId;
            for (const auto &rec : list) {
                if (rec.id == target) {
                    return buildCommandObject(ctx, rec, pane->id);
                }
            }
            return JS_NULL;
        }
        case 10: { // commands — gated on shell.commands.
            if (!checkPerm(ctx, Perm::ShellReadCommands)) {
                scheduleTermination(ctx);
                return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
            }
            JSValue arr = JS_NewArray(ctx);
            if (!eng->callbacks().paneCommands) {
                return arr;
            }
            auto list = eng->callbacks().paneCommands(pane->id, 0);
            for (size_t i = 0; i < list.size(); ++i) {
                JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), buildCommandObject(ctx, list[i], pane->id));
            }
            return arr;
        }
        case 11: { // selection → { startRowId, startCol, endRowId, endCol } | null
            REQUIRE_PERM(ctx, PaneRead);
            if (!info.hasSelection) {
                return JS_NULL;
            }
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "startRowId", JS_NewInt64(ctx, static_cast<int64_t>(info.selectionStartLineId)));
            JS_SetPropertyStr(ctx, obj, "startCol", JS_NewInt32(ctx, info.selectionStartCol));
            JS_SetPropertyStr(ctx, obj, "endRowId", JS_NewInt64(ctx, static_cast<int64_t>(info.selectionEndLineId)));
            JS_SetPropertyStr(ctx, obj, "endCol", JS_NewInt32(ctx, info.selectionEndCol));
            return obj;
        }
        // case 12 (cursor) is intentionally absent: pane.cursor resolves via
        // the inherited Terminal base getter (jsTerminalGetProp magic 2),
        // which performs a live mutex-locked read against the emulator and
        // applies the same PaneRead gate when `this` is a pane. The previous
        // case-12 fallback used the paneInfo snapshot and was never wired
        // into jsPaneProto, so removing it has no observable effect.
        case 13: return JS_NewInt64(ctx, static_cast<int64_t>(info.oldestLineId));
        case 14: return JS_NewInt64(ctx, static_cast<int64_t>(info.newestLineId));
        case 15: { // mousePosition → { cellX, cellY, pixelX, pixelY } | null
            if (!info.mouseInPane) {
                return JS_NULL;
            }
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "cellX", JS_NewInt32(ctx, info.mouseCellX));
            JS_SetPropertyStr(ctx, obj, "cellY", JS_NewInt32(ctx, info.mouseCellY));
            JS_SetPropertyStr(ctx, obj, "pixelX", JS_NewInt32(ctx, info.mousePixelX));
            JS_SetPropertyStr(ctx, obj, "pixelY", JS_NewInt32(ctx, info.mousePixelY));
            return obj;
        }
        case 16: { // selectedCommandId → number | null (gated on shell.commands, matching .commands)
            if (!checkPerm(ctx, Perm::ShellReadCommands)) {
                scheduleTermination(ctx);
                return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
            }
            if (!info.selectedCommandId) {
                return JS_NULL;
            }
            return JS_NewInt64(ctx, static_cast<int64_t>(*info.selectedCommandId));
        }
        case 17: // nodeId → UUID string of this pane's Terminal node in the shared
                 // LayoutTree, or null if unattached. Ungated — UUIDs are just
                 // handles that round-trip through mb.layout.node(...), which has
                 // its own permission discipline for mutations.
            return info.nodeId.empty()
                ? JS_NULL
                : JS_NewStringLen(ctx, info.nodeId.data(), info.nodeId.size());
        case 18: return JS_NewBool(ctx, info.usingAltScreen);
        default: return JS_UNDEFINED;
    }
}

static JSValue jsPaneSelectCommand(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_UNDEFINED;
    }
    if (!checkPerm(ctx, Perm::ShellReadCommands)) {
        scheduleTermination(ctx);
        return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneSetSelectedCommand) {
        return JS_UNDEFINED;
    }
    std::optional<uint64_t> id;
    if (argc >= 1 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        int64_t v = 0;
        if (JS_ToInt64(ctx, &v, argv[0]) < 0) {
            return JS_EXCEPTION;
        }
        if (v < 0) {
            return JS_ThrowRangeError(ctx, "selectCommand: id must be non-negative");
        }
        id = static_cast<uint64_t>(v);
    }
    eng->callbacks().paneSetSelectedCommand(pane->id, id);
    return JS_UNDEFINED;
}

// Forward declarations — defined after Popup / Embedded classes
static JSValue jsPaneCreatePopup(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue jsPaneGetPopups(JSContext *, JSValueConst);
static JSValue jsPaneCreateEmbedded(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue jsPaneGetEmbeddeds(JSContext *, JSValueConst);

static const JSCFunctionListEntry jsPaneProto[] = {
    // Methods & getters listed here are PANE-SPECIFIC. `inject`, `cols`,
    // `rows`, `cursor`, `kind` are inherited from the Terminal base
    // prototype (see jsTerminalProto).
    JS_CFUNC_DEF("addEventListener", 2, jsPaneAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsPaneRemoveEventListener),
    JS_CFUNC_DEF("write", 1, jsPaneWrite),
    JS_CFUNC_DEF("paste", 1, jsPanePaste),
    JS_CFUNC_DEF("getTextFromRows", 4, jsPaneGetTextFromRows),
    JS_CFUNC_DEF("writeRangeToFile", 5, jsPaneWriteRangeToFile),
    JS_CFUNC_DEF("captureOutputToFile", 2, jsPaneCaptureOutputToFile),
    JS_CFUNC_DEF("getLinksFromRows", 2, jsPaneGetLinksFromRows),
    JS_CFUNC_DEF("linkAt", 2, jsPaneLinkAt),
    JS_CFUNC_DEF("rowIdAt", 1, jsPaneRowIdAt),
    JS_CFUNC_DEF("findText", 2, jsPaneFindText),
    JS_CFUNC_DEF("scrollToRow", 1, jsPaneScrollToRow),
    JS_CFUNC_DEF("createPopup", 1, jsPaneCreatePopup),
    JS_CFUNC_DEF("createEmbeddedTerminal", 1, jsPaneCreateEmbedded),
    // pane.nodeId is the LayoutTree node UUID (magic 17 below). A prior
    // duplicate entry at magic 0 returned the engine PaneId string —
    // unreachable due to QuickJS last-write-wins on duplicate prop
    // names, and semantically wrong (PaneId ≠ tree node UUID). Removed.
    JS_CGETSET_MAGIC_DEF("title", jsPaneGetProp, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("cwd", jsPaneGetProp, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("hasPty", jsPaneGetProp, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("focused", jsPaneGetProp, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("focusedPopupId", jsPaneGetProp, nullptr, 7),
    JS_CGETSET_MAGIC_DEF("foregroundProcess", jsPaneGetProp, nullptr, 8),
    JS_CGETSET_DEF("popups", jsPaneGetPopups, nullptr),
    JS_CGETSET_DEF("embeddeds", jsPaneGetEmbeddeds, nullptr),
    JS_CGETSET_MAGIC_DEF("selectedCommand", jsPaneGetProp, nullptr, 9),
    JS_CGETSET_MAGIC_DEF("commands", jsPaneGetProp, nullptr, 10),
    JS_CGETSET_MAGIC_DEF("selection", jsPaneGetProp, nullptr, 11),
    JS_CGETSET_MAGIC_DEF("oldestRowId", jsPaneGetProp, nullptr, 13),
    JS_CGETSET_MAGIC_DEF("newestRowId", jsPaneGetProp, nullptr, 14),
    JS_CGETSET_MAGIC_DEF("mousePosition", jsPaneGetProp, nullptr, 15),
    JS_CGETSET_MAGIC_DEF("selectedCommandId", jsPaneGetProp, nullptr, 16),
    JS_CGETSET_MAGIC_DEF("nodeId", jsPaneGetProp, nullptr, 17),
    JS_CGETSET_MAGIC_DEF("usingAltScreen", jsPaneGetProp, nullptr, 18),
    JS_CFUNC_DEF("selectCommand", 1, jsPaneSelectCommand),
};

// ============================================================================
// Popup JS class — wraps (paneId, popupId string)
// ============================================================================

static JSClassID jsPopupClassId;

struct JsPopupData
{
    PaneId paneId;
    std::string popupId;
    bool alive;
};

static void jsPopupFinalize(JSRuntime *, JSValue val)
{
    delete static_cast<JsPopupData *>(JS_GetOpaque(val, jsPopupClassId));
}

static JSClassDef jsPopupClassDef = { "Popup", jsPopupFinalize };

static JSValue jsPopupNew(JSContext *ctx, PaneId paneId, const std::string &popupId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsPopupClassId);
    JS_SetOpaque(obj, new JsPopupData { paneId, popupId, true });
    return obj;
}

static JsPopupData *jsPopupGet(JSContext *ctx, JSValueConst val)
{
    return static_cast<JsPopupData *>(JS_GetOpaque(val, jsPopupClassId));
}

// popup.close()
static JSValue jsPopupClose(JSContext *ctx, JSValueConst this_val,
                            int, JSValueConst *)
{
    REQUIRE_PERM(ctx, UiPopupDestroy);
    auto *popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) {
        return JS_ThrowTypeError(ctx, "popup is destroyed");
    }
    engineFromCtx(ctx)->callbacks().destroyPopup(popup->paneId, popup->popupId);
    popup->alive = false;

    // Clear popup registry so a future popup with the same id on the same pane
    // registers fresh and input is delivered to the new popup's listeners.
    std::string regKey = popup->paneId.toString() + ":" + popup->popupId;
    JSValue global     = JS_GetGlobalObject(ctx);
    JSValue registry   = JS_GetPropertyStr(ctx, global, "__popup_registry");
    if (!JS_IsUndefined(registry)) {
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_UNDEFINED);
    }
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

// popup.addEventListener(event, fn) — stores on the popup object
static JSValue jsPopupAddEventListener(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
    }
    auto *popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) {
        return JS_ThrowTypeError(ctx, "popup is destroyed");
    }

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }

    std::string prop;
    if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput");
        }
        prop = "__input_filters";
    } else if (strcmp(event, "mouse") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: ui");
        }
        prop = "__mouse_listeners";
    } else if (strcmp(event, "mousemove") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: ui");
        }
        prop = "__evt_mousemove";
    } else {
        prop = std::string("__evt_") + event;
    }
    JS_FreeCString(ctx, event);

    // Register in popup registry for input/mouse delivery
    Engine *eng        = engineFromCtx(ctx);
    std::string regKey = popup->paneId.toString() + ":" + popup->popupId;
    // Use a string-keyed property on a global popup registry
    JSValue global     = JS_GetGlobalObject(ctx);
    JSValue registry   = JS_GetPropertyStr(ctx, global, "__popup_registry");
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__popup_registry", JS_DupValue(ctx, registry));
    }
    JSValue existing = JS_GetPropertyStr(ctx, registry, regKey.c_str());
    if (JS_IsUndefined(existing)) {
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_DupValue(ctx, this_val));
    }
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);

    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    if (JS_IsUndefined(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, prop.c_str(), JS_DupValue(ctx, arr));
    }
    JSValue pushFn = JS_GetPropertyStr(ctx, arr, "push");
    JS_Call(ctx, pushFn, arr, 1, &argv[1]);
    JS_FreeValue(ctx, pushFn);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsPopupRemoveEventListener(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
    }
    auto *popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) {
        return JS_ThrowTypeError(ctx, "popup is destroyed");
    }
    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }
    std::string prop;
    if (strcmp(event, "input") == 0) {
        prop = "__input_filters";
    } else if (strcmp(event, "mouse") == 0) {
        prop = "__mouse_listeners";
    } else {
        prop = std::string("__evt_") + event;
    }
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    removeFromJSArray(ctx, arr, argv[1]);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

// popup property getters
static JSValue jsPopupGetProp(JSContext *ctx, JSValueConst this_val, int magic)
{
    auto *popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) {
        return JS_UNDEFINED;
    }
    Engine *eng = engineFromCtx(ctx);
    switch (magic) {
        case 0: {
            std::string s = popup->paneId.toString();
            return JS_NewStringLen(ctx, s.data(), s.size());
        }
        case 1: return JS_NewString(ctx, popup->popupId.c_str());
        case 2: {
            auto info = eng->callbacks().paneInfo(popup->paneId);
            return JS_NewBool(ctx, info.focusedPopupId == popup->popupId);
        }
        case 5:
        case 6: {
            // x, y — look up from panePopups (cols/rows come from Terminal base).
            auto popups = eng->callbacks().panePopups(popup->paneId);
            for (const auto &p : popups) {
                if (p.id == popup->popupId) {
                    if (magic == 5) {
                        return JS_NewInt32(ctx, p.x);
                    }
                    if (magic == 6) {
                        return JS_NewInt32(ctx, p.y);
                    }
                }
            }
            return JS_UNDEFINED;
        }
        default: return JS_UNDEFINED;
    }
}

// popup.resize({x, y, w, h})
static JSValue jsPopupResize(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "resize requires ({x, y, w, h})");
    }
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto *popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) {
        return JS_ThrowTypeError(ctx, "popup is destroyed");
    }

    int32_t x, y, w, h;
    JSValue v;
    // Default to current values if not specified — need to query
    // For simplicity, all four are required
    v = JS_GetPropertyStr(ctx, argv[0], "x");
    JS_ToInt32(ctx, &x, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "y");
    JS_ToInt32(ctx, &y, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "w");
    JS_ToInt32(ctx, &w, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "h");
    JS_ToInt32(ctx, &h, v);
    JS_FreeValue(ctx, v);

    Engine *eng = engineFromCtx(ctx);
    eng->callbacks().resizePopup(popup->paneId, popup->popupId, x, y, w, h);
    return JS_UNDEFINED;
}

// popup.focus() — make this popup the focused popup on its pane (and clear
// the pane's focused embedded). Gated on UiFocus so OSC-loaded scripts
// without explicit focus permission cannot hijack the keyboard. Returns
// true on success, false if the popup is already gone.
static JSValue jsPopupFocus(JSContext *ctx, JSValueConst this_val,
                            int, JSValueConst *)
{
    REQUIRE_PERM(ctx, UiFocus);
    auto *popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) {
        return JS_NewBool(ctx, false);
    }
    Engine *eng = engineFromCtx(ctx);
    bool ok     = eng->callbacks().setFocusedPopup && eng->callbacks().setFocusedPopup(popup->paneId, popup->popupId);
    return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry jsPopupProto[] = {
    // `inject`, `cols`, `rows`, `cursor`, `kind` inherited from Terminal base.
    JS_CFUNC_DEF("addEventListener", 2, jsPopupAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsPopupRemoveEventListener),
    JS_CFUNC_DEF("resize", 1, jsPopupResize),
    JS_CFUNC_DEF("focus", 0, jsPopupFocus),
    JS_CFUNC_DEF("close", 0, jsPopupClose),
    JS_CGETSET_MAGIC_DEF("paneId", jsPopupGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("id", jsPopupGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("focused", jsPopupGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("x", jsPopupGetProp, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("y", jsPopupGetProp, nullptr, 6),
};

// pane.createPopup({id, x, y, w, h}) -> Popup
static JSValue jsPaneCreatePopup(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "createPopup requires ({id, x, y, w, h})");
    }
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);

    const char *id = nullptr;
    JSValue idVal  = JS_GetPropertyStr(ctx, argv[0], "id");
    if (JS_IsString(idVal)) {
        id = JS_ToCString(ctx, idVal);
    }
    JS_FreeValue(ctx, idVal);
    if (!id) {
        return JS_ThrowTypeError(ctx, "createPopup: missing 'id' property");
    }

    int32_t x = 0, y = 0, w = 20, h = 5;
    JSValue v;
    v = JS_GetPropertyStr(ctx, argv[0], "x");
    JS_ToInt32(ctx, &x, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "y");
    JS_ToInt32(ctx, &y, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "w");
    JS_ToInt32(ctx, &w, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "h");
    JS_ToInt32(ctx, &h, v);
    JS_FreeValue(ctx, v);

    std::string popupId(id);
    JS_FreeCString(ctx, id);

    Uuid paneId = pane->id;
    bool ok     = eng->callbacks().createPopup(paneId, popupId, x, y, w, h, [eng, paneId, popupId](const char *data, size_t len)
                                           {
                                               // Deliver input to popup listeners
                                               std::string regKey = paneId.toString() + ":" + popupId;
                                               eng->deliverPopupInput(regKey, data, len);
                                           });

    if (!ok) {
        return JS_ThrowTypeError(ctx, "createPopup failed (duplicate id?)");
    }

    // Track ownership for cleanup on unload
    auto *inst = instanceFromCtx(ctx);
    if (inst) {
        inst->ownedPopups.push_back({ paneId, popupId });
    }

    return jsPopupNew(ctx, paneId, popupId);
}

// pane.popups — returns array of Popup objects for this pane
static JSValue jsPaneGetPopups(JSContext *ctx, JSValueConst this_val)
{
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_NewArray(ctx);
    }
    Engine *eng = engineFromCtx(ctx);
    auto popups = eng->callbacks().panePopups(pane->id);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < popups.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, jsPopupNew(ctx, pane->id, popups[i].id));
    }
    return arr;
}

// ============================================================================
// EmbeddedTerminal JS class — wraps (paneId, lineId)
// ============================================================================

static JSClassID jsEmbeddedClassId;

struct JsEmbeddedData
{
    PaneId paneId;
    uint64_t lineId;
    bool alive;
};

static void jsEmbeddedFinalize(JSRuntime *, JSValue val)
{
    delete static_cast<JsEmbeddedData *>(JS_GetOpaque(val, jsEmbeddedClassId));
}

static JSClassDef jsEmbeddedClassDef = { "EmbeddedTerminal", jsEmbeddedFinalize };

static JSValue jsEmbeddedNew(JSContext *ctx, PaneId paneId, uint64_t lineId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsEmbeddedClassId);
    JS_SetOpaque(obj, new JsEmbeddedData { paneId, lineId, true });
    return obj;
}

static JsEmbeddedData *jsEmbeddedGet(JSContext *, JSValueConst val)
{
    return static_cast<JsEmbeddedData *>(JS_GetOpaque(val, jsEmbeddedClassId));
}

// embedded.resize(rows)
static JSValue jsEmbeddedResize(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "resize requires (rows)");
    }
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto *em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) {
        return JS_ThrowTypeError(ctx, "embedded is destroyed");
    }
    int32_t rows = 0;
    if (JS_ToInt32(ctx, &rows, argv[0]) < 0) {
        return JS_EXCEPTION;
    }
    bool ok = engineFromCtx(ctx)->callbacks().resizeEmbedded(em->paneId, em->lineId, rows);
    return JS_NewBool(ctx, ok);
}

// embedded.close()
static JSValue jsEmbeddedClose(JSContext *ctx, JSValueConst this_val,
                               int, JSValueConst *)
{
    REQUIRE_PERM(ctx, UiPopupDestroy);
    auto *em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) {
        return JS_ThrowTypeError(ctx, "embedded is destroyed");
    }
    Engine *eng        = engineFromCtx(ctx);
    // Capture regKey before destroy invalidates the embedded.
    std::string regKey = em->paneId.toString() + ":" + std::to_string(em->lineId);
    eng->callbacks().destroyEmbedded(em->paneId, em->lineId);
    em->alive = false;
    // Fire destroyed event to listeners, then clear registry entry so a
    // future embedded at the same lineId (unlikely — ids are monotonic)
    // registers fresh.
    eng->deliverEmbeddedDestroyed(regKey);
    JSValue global   = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, "__embedded_registry");
    if (!JS_IsUndefined(registry)) {
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_UNDEFINED);
    }
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

// embedded.addEventListener(event, fn)
// Supported events: "input" (keystrokes when focused), "destroyed" (eviction or close).
static JSValue jsEmbeddedAddEventListener(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
    }
    auto *em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) {
        return JS_ThrowTypeError(ctx, "embedded is destroyed");
    }

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }

    std::string prop;
    if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput");
        }
        prop = "__input_filters";
    } else if (strcmp(event, "mouse") == 0 || strcmp(event, "mousemove") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "permission denied: ui");
        }
        // Mirror pane: "mouse" → dedicated list (press/release), "mousemove" → __evt_ bucket.
        prop = (strcmp(event, "mouse") == 0) ? "__mouse_listeners" : "__evt_mousemove";
    } else {
        prop = std::string("__evt_") + event;
    }
    JS_FreeCString(ctx, event);

    std::string regKey = em->paneId.toString() + ":" + std::to_string(em->lineId);
    JSValue global     = JS_GetGlobalObject(ctx);
    JSValue registry   = JS_GetPropertyStr(ctx, global, "__embedded_registry");
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__embedded_registry", JS_DupValue(ctx, registry));
    }
    JSValue existing = JS_GetPropertyStr(ctx, registry, regKey.c_str());
    if (JS_IsUndefined(existing)) {
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_DupValue(ctx, this_val));
    }
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);

    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    if (JS_IsUndefined(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, prop.c_str(), JS_DupValue(ctx, arr));
    }
    JSValue pushFn = JS_GetPropertyStr(ctx, arr, "push");
    JS_Call(ctx, pushFn, arr, 1, &argv[1]);
    JS_FreeValue(ctx, pushFn);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsEmbeddedRemoveEventListener(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
    }
    auto *em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) {
        return JS_ThrowTypeError(ctx, "embedded is destroyed");
    }
    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }
    std::string prop;
    if (strcmp(event, "input") == 0) {
        prop = "__input_filters";
    } else if (strcmp(event, "mouse") == 0) {
        prop = "__mouse_listeners";
    } else {
        prop = std::string("__evt_") + event;
    }
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    removeFromJSArray(ctx, arr, argv[1]);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

// embedded.<property> getter
static JSValue jsEmbeddedGetProp(JSContext *ctx, JSValueConst this_val, int magic)
{
    auto *em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) {
        return JS_UNDEFINED;
    }
    Engine *eng = engineFromCtx(ctx);
    switch (magic) {
        case 0: {
            std::string s = em->paneId.toString();
            return JS_NewStringLen(ctx, s.data(), s.size());
        }
        case 1: return JS_NewInt64(ctx, static_cast<int64_t>(em->lineId));
        case 4: {
            // focused — cols/rows now come from the Terminal base class.
            auto embeds = eng->callbacks().paneEmbeddeds(em->paneId);
            for (const auto &e : embeds) {
                if (e.lineId == em->lineId) {
                    return JS_NewBool(ctx, e.focused);
                }
            }
            return JS_UNDEFINED;
        }
    }
    return JS_UNDEFINED;
}

// embedded.focus() — make this embedded the focused embedded on its pane
// (and clear the pane's focused popup). Gated on UiFocus, mirroring
// popup.focus(). Returns true on success, false if already gone.
static JSValue jsEmbeddedFocus(JSContext *ctx, JSValueConst this_val,
                               int, JSValueConst *)
{
    REQUIRE_PERM(ctx, UiFocus);
    auto *em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) {
        return JS_NewBool(ctx, false);
    }
    Engine *eng = engineFromCtx(ctx);
    bool ok     = eng->callbacks().setFocusedEmbedded && eng->callbacks().setFocusedEmbedded(em->paneId, em->lineId);
    return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry jsEmbeddedProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsEmbeddedAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsEmbeddedRemoveEventListener),
    JS_CFUNC_DEF("resize", 1, jsEmbeddedResize),
    JS_CFUNC_DEF("focus", 0, jsEmbeddedFocus),
    JS_CFUNC_DEF("close", 0, jsEmbeddedClose),
    JS_CGETSET_MAGIC_DEF("paneId", jsEmbeddedGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("id", jsEmbeddedGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("focused", jsEmbeddedGetProp, nullptr, 4),
};

// ============================================================================
// MbOutputCapture JS class — opaque handle returned by pane.captureOutputToFile
// ============================================================================
//
// Shape:
//   handle.path        : string  — sandbox-validated absolute path
//   handle.format      : "raw" | "asciicast"
//   handle.active      : boolean — false after stop() or auto-stop
//   handle.stop()      : explicit stop; idempotent. Fires "stopped"
//                        listeners with reason="explicit".
//   handle.addEventListener("stopped", fn) / removeEventListener
//
// "stopped" event payload: { reason: "explicit" | "io-error",
//                            error: string }   — error="" for explicit.
//
// Listeners live as a JS array on the handle object itself (same
// pattern as pane / popup listeners), keyed by `__evt_stopped`.

static JSClassID jsOutputCaptureClassId;

struct JsOutputCaptureData
{
    PaneId paneId;
    std::string path;   // sandbox-validated absolute path
    std::string format; // "raw" | "asciicast"
    bool active;        // false after stop() or auto-stop
    InstanceId instId;  // owning script instance, for cleanup-on-unload
};

static void jsOutputCaptureFinalize(JSRuntime *, JSValue val)
{
    delete static_cast<JsOutputCaptureData *>(
        JS_GetOpaque(val, jsOutputCaptureClassId));
}

static JSClassDef jsOutputCaptureClassDef = {
    "OutputCapture",
    jsOutputCaptureFinalize
};

static JSValue jsOutputCaptureNew(JSContext *ctx,
                                  PaneId paneId,
                                  const std::string &path,
                                  const std::string &format,
                                  InstanceId instId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsOutputCaptureClassId);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new JsOutputCaptureData { paneId, path, format, /*active*/ true, instId });
    return obj;
}

static JsOutputCaptureData *jsOutputCaptureGet(JSContext *ctx, JSValueConst val)
{
    return static_cast<JsOutputCaptureData *>(
        JS_GetOpaque(val, jsOutputCaptureClassId));
}

// handle.stop() — idempotent. Mirrors removeOutputCapture; the engine
// stop also fires the "stopped" event via the platform-side callback
// path, so we don't synthesise one here. Returns true the first time
// it actually stopped a live capture, false if already stopped.
static JSValue jsOutputCaptureStop(JSContext *ctx, JSValueConst this_val,
                                   int, JSValueConst *)
{
    auto *d = jsOutputCaptureGet(ctx, this_val);
    if (!d) {
        return JS_NewBool(ctx, false);
    }
    if (!d->active) {
        return JS_NewBool(ctx, false);
    }

    Engine *eng = engineFromCtx(ctx);
    Terminal *t = eng ? eng->terminal(d->paneId) : nullptr;
    if (!t) {
        // Pane already gone (terminal exited / tree pruned). Mark
        // inactive so subsequent calls no-op; the underlying file
        // was closed by Terminal's dtor.
        d->active = false;
        return JS_NewBool(ctx, false);
    }
    bool removed = t->removeOutputCapture(d->path);
    d->active    = false; // even if removed==false (race), the handle is dead
    return JS_NewBool(ctx, removed);
}

static JSValue jsOutputCaptureAddEventListener(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx,
                                 "addEventListener requires (string, function)");
    }
    auto *d = jsOutputCaptureGet(ctx, this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "capture is destroyed");
    }

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }
    // Only "stopped" is defined for now. Other event names install
    // listeners that will simply never fire — same forgiving pattern
    // pane uses for unknown events.
    std::string prop = std::string("__evt_") + event;
    JS_FreeCString(ctx, event);

    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    if (JS_IsUndefined(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, prop.c_str(), JS_DupValue(ctx, arr));
    }
    JSValue pushFn = JS_GetPropertyStr(ctx, arr, "push");
    JS_Call(ctx, pushFn, arr, 1, &argv[1]);
    JS_FreeValue(ctx, pushFn);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsOutputCaptureRemoveEventListener(JSContext *ctx,
                                                  JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx,
                                 "removeEventListener requires (string, function)");
    }
    auto *d = jsOutputCaptureGet(ctx, this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "capture is destroyed");
    }
    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }
    std::string prop = std::string("__evt_") + event;
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    removeFromJSArray(ctx, arr, argv[1]);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsOutputCaptureGetProp(JSContext *ctx, JSValueConst this_val,
                                      int magic)
{
    auto *d = jsOutputCaptureGet(ctx, this_val);
    if (!d) {
        return JS_UNDEFINED;
    }
    switch (magic) {
        case 0: return JS_NewStringLen(ctx, d->path.data(), d->path.size());
        case 1: return JS_NewStringLen(ctx, d->format.data(), d->format.size());
        case 2: return JS_NewBool(ctx, d->active);
        case 3: { // paneId — useful when the script has lost the pane handle
            std::string s = d->paneId.toString();
            return JS_NewStringLen(ctx, s.data(), s.size());
        }
        default: return JS_UNDEFINED;
    }
}

static const JSCFunctionListEntry jsOutputCaptureProto[] = {
    JS_CFUNC_DEF("stop", 0, jsOutputCaptureStop),
    JS_CFUNC_DEF("addEventListener", 2, jsOutputCaptureAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsOutputCaptureRemoveEventListener),
    JS_CGETSET_MAGIC_DEF("path", jsOutputCaptureGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("format", jsOutputCaptureGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("active", jsOutputCaptureGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("paneId", jsOutputCaptureGetProp, nullptr, 3),
};

// ============================================================================
// Terminal JS base class — shared prototype for Pane / Popup / EmbeddedTerminal
// ============================================================================
//
// Every concrete class backed by a live `TerminalEmulator` inherits from this.
// The base carries methods that delegate purely to `TerminalEmulator` state
// (inject, width/height, cursor). Kind-specific lifecycle + identity stay on
// subclasses: Pane has cwd/title/commands/selection, Popup has resize({x,y,w,h})
// and close(), Embedded has resize(rows) and close(), etc.
//
// The base class is abstract — `new Terminal()` from JS would create an object
// with no opaque, and every base method resolves the backing emulator via
// resolveEmulatorFromVal, which returns nullptr for the base itself.

static JSClassID jsTerminalClassId;
static JSClassDef jsTerminalClassDef = { "Terminal", nullptr };

// Branch on the concrete JS class. JS_GetOpaque returns non-null only when
// `val` is exactly that class id, so the chain finds the right opaque in O(1).
static TerminalEmulator *resolveEmulatorFromVal(JSContext *ctx, JSValueConst val)
{
    Engine *eng = engineFromCtx(ctx);
    if (!eng) {
        return nullptr;
    }
    if (auto *p = static_cast<JsPaneData *>(JS_GetOpaque(val, jsPaneClassId))) {
        if (!p->alive) {
            return nullptr;
        }
        return eng->terminal(p->id);
    }
    if (auto *d = static_cast<JsPopupData *>(JS_GetOpaque(val, jsPopupClassId))) {
        if (!d->alive) {
            return nullptr;
        }
        Terminal *parent = eng->terminal(d->paneId);
        return parent ? parent->findPopup(d->popupId) : nullptr;
    }
    if (auto *d = static_cast<JsEmbeddedData *>(JS_GetOpaque(val, jsEmbeddedClassId))) {
        if (!d->alive) {
            return nullptr;
        }
        Terminal *parent = eng->terminal(d->paneId);
        return parent ? parent->findEmbedded(d->lineId) : nullptr;
    }
    return nullptr;
}

// terminal.inject(data)
static JSValue jsTerminalInject(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "inject requires (string)");
    }
    REQUIRE_PERM(ctx, IoInject);
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "terminal is destroyed");
    }
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) {
        return JS_EXCEPTION;
    }
    emu->injectData(str, len);
    JS_FreeCString(ctx, str);
    if (auto &cb = engineFromCtx(ctx)->callbacks().requestRedraw) {
        cb();
    }
    return JS_UNDEFINED;
}

// Shared getters: 0 = cols, 1 = rows, 2 = cursor, 3 = kind, 4 = cellWidth, 5 = cellHeight.
static JSValue jsTerminalGetProp(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (magic == 3) {
        // `kind` — discriminator for subclass. Doesn't require a live emulator.
        JSClassID cid = JS_GetClassID(this_val);
        if (cid == jsPaneClassId) {
            return JS_NewString(ctx, "pane");
        }
        if (cid == jsPopupClassId) {
            return JS_NewString(ctx, "popup");
        }
        if (cid == jsEmbeddedClassId) {
            return JS_NewString(ctx, "embedded");
        }
        return JS_NewString(ctx, "terminal");
    }
    if (magic == 4 || magic == 5) {
        // cellWidth / cellHeight in pixels — window-global font metrics.
        auto &cb = engineFromCtx(ctx)->callbacks().fontCellSize;
        if (!cb) {
            return JS_NewFloat64(ctx, 0.0);
        }
        auto [cw, ch] = cb();
        return JS_NewFloat64(ctx, magic == 4 ? cw : ch);
    }
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) {
        return JS_UNDEFINED;
    }
    switch (magic) {
        case 0: return JS_NewInt32(ctx, emu->width());
        case 1: return JS_NewInt32(ctx, emu->height());
        case 2: {
            // cursor → { rowId, col, visible }.  PaneRead gate applies
            // only when the caller is a shell pane — applets querying their
            // own popup/embedded don't need the extra grant (the cursor they
            // see is their own drawing).
            if (JS_GetClassID(this_val) == jsPaneClassId) {
                REQUIRE_PERM(ctx, PaneRead);
            }
            std::lock_guard<std::recursive_mutex> _lk(emu->mutex());
            int absRow     = emu->document().historySize() + emu->cursorY();
            uint64_t rowId = emu->document().lineIdForAbs(absRow);
            JSValue obj    = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "rowId", JS_NewInt64(ctx, static_cast<int64_t>(rowId)));
            JS_SetPropertyStr(ctx, obj, "col", JS_NewInt32(ctx, emu->cursorX()));
            JS_SetPropertyStr(ctx, obj, "visible", JS_NewBool(ctx, emu->cursorVisible()));
            return obj;
        }
    }
    return JS_UNDEFINED;
}

// Forward decls for handle construction — full definitions follow the
// existing Decoration{...} entry points.
static JSValue jsDecorationHandleNew(JSContext *ctx, JSValueConst owner, uint64_t decorationId);

// Parse a JS spec object into a Decoration. On error, throws via
// JS_ThrowTypeError and returns false. Caller-context label (`who`) is
// included in error messages so applet authors can tell `addDecoration`
// failures from `batch.addDecoration` failures. The returned decoration's
// `id` is left at 0 — assigned by TerminalEmulator on insert.
static bool parseDecorationSpec(JSContext *ctx, JSValueConst specVal,
                                Decoration &d, const char *who)
{
    if (!JS_IsObject(specVal)) {
        JS_ThrowTypeError(ctx, "%s requires (spec)", who);
        return false;
    }
    d      = Decoration {};
    d.kind = DecorationKind::User;

    auto readInt64 = [&](const char *name, int64_t &out) -> bool
    {
        JSValue v = JS_GetPropertyStr(ctx, specVal, name);
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            return false;
        }
        int64_t n = 0;
        int rc    = JS_ToInt64(ctx, &n, v);
        JS_FreeValue(ctx, v);
        if (rc < 0) {
            return false;
        }
        out = n;
        return true;
    };
    auto readInt32 = [&](const char *name, int32_t &out) -> bool
    {
        JSValue v = JS_GetPropertyStr(ctx, specVal, name);
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            return false;
        }
        int32_t n = 0;
        int rc    = JS_ToInt32(ctx, &n, v);
        JS_FreeValue(ctx, v);
        if (rc < 0) {
            return false;
        }
        out = n;
        return true;
    };

    int64_t s = 0, e = 0;
    int32_t sc = 0, ec = 0;
    if (!readInt64("startRowId", s) || !readInt64("endRowId", e) ||
        !readInt32("startCol", sc) || !readInt32("endCol", ec)) {
        JS_ThrowTypeError(ctx, "%s: startRowId/endRowId/startCol/endCol required", who);
        return false;
    }
    d.startLineId     = static_cast<uint64_t>(s);
    d.endLineId       = static_cast<uint64_t>(e);
    d.startCellOffset = sc;
    // JS-side `endCol` is EXCLUSIVE (one past the last covered cell),
    // matching `pane.selection.endCol`, `getTextFromRows`,
    // `MbCommand.outputEnd.col`, `MbLinkInfo.endCol`, and `MbMatch.endCol`.
    // The internal `Decoration::endCellOffset` is INCLUSIVE because the
    // renderer's per-row paint loop and resolveDecoration's clamp math both
    // treat it that way. Convert here at the boundary.
    //
    // Validation: a zero-width range on a single line (sc >= ec when
    // startLineId == endLineId) is rejected — it paints nothing and a
    // silently-stored phantom decoration the renderer can never display
    // would just leak into the snapshot.
    if (s == e && ec <= sc) {
        JS_ThrowTypeError(ctx, "%s: endCol must be > startCol on a single-line range", who);
        return false;
    }
    // For multi-line ranges, ec == 0 means "stop at col 0 of endRowId",
    // i.e. the final row contributes nothing. The inclusive primitive
    // can't express "no cells on the final row" cleanly; clamp to col 0
    // (paints one cell). Acceptable degradation; multi-line decorations
    // with ec == 0 are rare (selection pinned at row boundary).
    d.endCellOffset = (ec > 0) ? (ec - 1) : 0;

    int32_t z = 0;
    if (readInt32("zPriority", z)) {
        d.zPriority = z;
    }

    JSValue shapeV = JS_GetPropertyStr(ctx, specVal, "shape");
    if (JS_IsString(shapeV)) {
        const char *sv = JS_ToCString(ctx, shapeV);
        if (sv) {
            if (std::string(sv) == "rectangle") {
                d.shape = DecorationShape::Rectangle;
            }
            JS_FreeCString(ctx, sv);
        }
    }
    JS_FreeValue(ctx, shapeV);

    JSValue tagV = JS_GetPropertyStr(ctx, specVal, "tag");
    if (JS_IsString(tagV)) {
        const char *tv = JS_ToCString(ctx, tagV);
        if (tv) {
            std::string t(tv);
            JS_FreeCString(ctx, tv);
            // Reserved tags are owned by the system kinds. User code can
            // create entries with these tags but they remain User-kind, so
            // they don't impersonate selection / hyperlink / command-region
            // composition. Still surface a clear error to keep applet code
            // honest.
            if (t == "selection" || t == "command-region" || t == "hyperlink") {
                JS_FreeValue(ctx, tagV);
                JS_ThrowTypeError(ctx, "%s: tag is reserved", who);
                return false;
            }
            d.tag = std::move(t);
        }
    }
    JS_FreeValue(ctx, tagV);

    JSValue styleV = JS_GetPropertyStr(ctx, specVal, "style");
    if (JS_IsObject(styleV)) {
        auto readU32 = [&](const char *name, std::optional<uint32_t> &out)
        {
            JSValue v = JS_GetPropertyStr(ctx, styleV, name);
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                uint32_t n = 0;
                if (JS_ToUint32(ctx, &n, v) >= 0) {
                    out = n;
                }
            }
            JS_FreeValue(ctx, v);
        };
        readU32("fg", d.style.fg);
        readU32("bg", d.style.bg);

        JSValue ulV = JS_GetPropertyStr(ctx, styleV, "underline");
        if (JS_IsObject(ulV)) {
            UnderlineSpec us;
            JSValue stV = JS_GetPropertyStr(ctx, ulV, "style");
            if (!JS_IsUndefined(stV)) {
                uint32_t n = 0;
                if (JS_ToUint32(ctx, &n, stV) >= 0) {
                    us.style = static_cast<uint8_t>(n & 0x3u);
                }
            }
            JS_FreeValue(ctx, stV);
            JSValue cV = JS_GetPropertyStr(ctx, ulV, "color");
            if (!JS_IsUndefined(cV)) {
                uint32_t n = 0;
                if (JS_ToUint32(ctx, &n, cV) >= 0) {
                    us.color = n;
                }
            }
            JS_FreeValue(ctx, cV);
            d.style.underline = us;
        }
        JS_FreeValue(ctx, ulV);

        JSValue stV = JS_GetPropertyStr(ctx, styleV, "strikethrough");
        if (JS_IsBool(stV)) {
            d.style.strikethrough = JS_ToBool(ctx, stV) ? true : false;
        }
        JS_FreeValue(ctx, stV);

        auto readI32 = [&](const char *name, int32_t &out)
        {
            JSValue v = JS_GetPropertyStr(ctx, styleV, name);
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                int32_t n = 0;
                if (JS_ToInt32(ctx, &n, v) >= 0) {
                    out = n;
                }
            }
            JS_FreeValue(ctx, v);
        };
        readI32("bgInflateX", d.style.bgInflateX);
        readI32("bgInflateY", d.style.bgInflateY);
    }
    JS_FreeValue(ctx, styleV);
    return true;
}

// terminal.addDecoration(spec) → id (number)
//
// spec: {
//   startRowId: number, startCol: number,
//   endRowId:   number, endCol:   number,
//   style?: { fg?, bg?, underline?: { style, color? }, strikethrough?: bool },
//   tag?: string,         // for grouping; reserved tags rejected
//   zPriority?: number,   // composition order tie-breaker among User entries
//   shape?: "range" | "rectangle",
// }
//
// `style.outline` and `style.dimOthers` are intentionally unsupported on
// User decorations: outline + multiplicative dim are uniform-driven render
// primitives currently dedicated to the OSC 133 command region.
//
// For bursts of mutations (e.g. clearing and re-adding hundreds of
// highlights every keystroke), prefer `createDecorationBatch()` — every
// addDecoration / clearDecorations call publishes a fresh snapshot, which
// the render thread can sample mid-burst.
static JSValue jsTerminalAddDecoration(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, PaneRead);
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "addDecoration requires (spec)");
    }
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "terminal is destroyed");
    }
    Decoration d;
    if (!parseDecorationSpec(ctx, argv[0], d, "addDecoration")) {
        return JS_EXCEPTION;
    }
    uint64_t id = emu->addDecoration(std::move(d));
    if (auto &cb = engineFromCtx(ctx)->callbacks().requestRedraw) {
        cb();
    }
    return jsDecorationHandleNew(ctx, this_val, id);
}

// terminal.removeDecoration(id) → boolean
static JSValue jsTerminalRemoveDecoration(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, PaneRead);
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "removeDecoration requires (id)");
    }
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "terminal is destroyed");
    }
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0) {
        return JS_EXCEPTION;
    }
    std::vector<uint64_t> cancelled;
    bool removed = emu->removeDecoration(static_cast<uint64_t>(id), &cancelled);
    if (Engine *eng = engineFromCtx(ctx)) {
        uint64_t nowMs = TerminalEmulator::mono();
        for (uint64_t hid : cancelled) {
            eng->settleDecorationAnimation(hid, "cancelled", /*snapToEnd=*/false, nowMs);
        }
        if (removed) {
            if (auto &cb = eng->callbacks().requestRedraw) {
                cb();
            }
        }
    }
    return JS_NewBool(ctx, removed);
}

// terminal.clearDecorations(tag?) → number (count cleared)
static JSValue jsTerminalClearDecorations(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, PaneRead);
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "terminal is destroyed");
    }
    std::string_view tag;
    std::string holder;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *tv = JS_ToCString(ctx, argv[0]);
        if (tv) {
            holder = tv;
            tag    = holder;
            JS_FreeCString(ctx, tv);
        }
    }
    std::vector<uint64_t> cancelled;
    size_t n = emu->clearUserDecorations(tag, &cancelled);
    if (Engine *eng = engineFromCtx(ctx)) {
        uint64_t nowMs = TerminalEmulator::mono();
        for (uint64_t hid : cancelled) {
            eng->settleDecorationAnimation(hid, "cancelled", /*snapToEnd=*/false, nowMs);
        }
        if (n > 0) {
            if (auto &cb = eng->callbacks().requestRedraw) {
                cb();
            }
        }
    }
    return JS_NewInt32(ctx, static_cast<int32_t>(n));
}

// ============================================================================
// DecorationBatch JS class — accumulates Add / Clear ops, applies them
// atomically on submit() via TerminalEmulator::applyDecorationBatch (one
// snapshot publish for the whole batch).
// ============================================================================

static JSClassID jsDecorationBatchClassId;

struct JsDecorationBatchData
{
    // Duped reference to the terminal-shaped JS object (Pane / Popup /
    // EmbeddedTerminal) that created this batch. Resolved at submit() time
    // so a destroyed terminal surfaces as a clean throw, not a UAF.
    JSValue ownerRef;
    std::vector<DecorationBatchOp> ops;
    bool submitted { false };
};

static void jsDecorationBatchFinalize(JSRuntime *rt, JSValue val)
{
    auto *d = static_cast<JsDecorationBatchData *>(JS_GetOpaque(val, jsDecorationBatchClassId));
    if (d) {
        JS_FreeValueRT(rt, d->ownerRef);
        delete d;
    }
}

static JSClassDef jsDecorationBatchClassDef = { "DecorationBatch", jsDecorationBatchFinalize };

static JsDecorationBatchData *jsDecorationBatchGet(JSValueConst val)
{
    return static_cast<JsDecorationBatchData *>(JS_GetOpaque(val, jsDecorationBatchClassId));
}

// batch.addDecoration(spec) — queues an Add op. Returns the batch (chainable).
static JSValue jsDecorationBatchAdd(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    auto *d = jsDecorationBatchGet(this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "not a DecorationBatch");
    }
    if (d->submitted) {
        return JS_ThrowTypeError(ctx, "DecorationBatch already submitted");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "addDecoration requires (spec)");
    }
    DecorationBatchOp op;
    op.kind = DecorationBatchOp::Kind::Add;
    if (!parseDecorationSpec(ctx, argv[0], op.spec, "batch.addDecoration")) {
        return JS_EXCEPTION;
    }
    d->ops.push_back(std::move(op));
    return JS_DupValue(ctx, this_val);
}

// batch.clearDecorations(tag?) — queues a Clear op. Returns the batch.
static JSValue jsDecorationBatchClear(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    auto *d = jsDecorationBatchGet(this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "not a DecorationBatch");
    }
    if (d->submitted) {
        return JS_ThrowTypeError(ctx, "DecorationBatch already submitted");
    }
    DecorationBatchOp op;
    op.kind = DecorationBatchOp::Kind::Clear;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *tv = JS_ToCString(ctx, argv[0]);
        if (tv) {
            op.clearTag = tv;
            JS_FreeCString(ctx, tv);
        }
    }
    d->ops.push_back(std::move(op));
    return JS_DupValue(ctx, this_val);
}

// batch.submit() — applies queued ops atomically. Returns an array of
// ids assigned to Add ops (in queue order). Throws if already submitted
// or the owning terminal is gone.
static JSValue jsDecorationBatchSubmit(JSContext *ctx, JSValueConst this_val,
                                       int, JSValueConst *)
{
    REQUIRE_PERM(ctx, PaneRead);
    auto *d = jsDecorationBatchGet(this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "not a DecorationBatch");
    }
    if (d->submitted) {
        return JS_ThrowTypeError(ctx, "DecorationBatch already submitted");
    }
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, d->ownerRef);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "terminal is destroyed");
    }
    d->submitted = true;
    std::vector<uint64_t> cancelled;
    std::vector<uint64_t> ids = emu->applyDecorationBatch(std::move(d->ops), &cancelled);
    Engine *eng               = engineFromCtx(ctx);
    if (eng) {
        uint64_t nowMs = TerminalEmulator::mono();
        for (uint64_t hid : cancelled) {
            eng->settleDecorationAnimation(hid, "cancelled", /*snapToEnd=*/false, nowMs);
        }
        if (auto &cb = eng->callbacks().requestRedraw) {
            cb();
        }
    }
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < ids.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), jsDecorationHandleNew(ctx, d->ownerRef, ids[i]));
    }
    return arr;
}

static const JSCFunctionListEntry jsDecorationBatchProto[] = {
    JS_CFUNC_DEF("addDecoration", 1, jsDecorationBatchAdd),
    JS_CFUNC_DEF("clearDecorations", 0, jsDecorationBatchClear),
    JS_CFUNC_DEF("submit", 0, jsDecorationBatchSubmit),
};

// terminal.createDecorationBatch() → DecorationBatch
static JSValue jsTerminalCreateDecorationBatch(JSContext *ctx, JSValueConst this_val,
                                               int, JSValueConst *)
{
    REQUIRE_PERM(ctx, PaneRead);
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "terminal is destroyed");
    }
    JSValue obj = JS_NewObjectClass(ctx, jsDecorationBatchClassId);
    auto *d     = new JsDecorationBatchData;
    d->ownerRef = JS_DupValue(ctx, this_val);
    JS_SetOpaque(obj, d);
    return obj;
}

// ============================================================================
// DecorationHandle JS class — opaque wrapper returned by addDecoration() and
// batch.submit(). Carries enough state to re-resolve the owning terminal at
// method-call time and route mutations (id, remove, animate).
// ============================================================================

static JSClassID jsDecorationHandleClassId;

struct JsDecorationHandleData
{
    JSValue ownerRef; // duped Pane/Popup/Embedded JS value
    uint64_t decorationId;
};

static void jsDecorationHandleFinalize(JSRuntime *rt, JSValue val)
{
    auto *d = static_cast<JsDecorationHandleData *>(JS_GetOpaque(val, jsDecorationHandleClassId));
    if (d) {
        JS_FreeValueRT(rt, d->ownerRef);
        delete d;
    }
}

static JSClassDef jsDecorationHandleClassDef = { "DecorationHandle", jsDecorationHandleFinalize };

static JSValue jsDecorationHandleNew(JSContext *ctx, JSValueConst owner, uint64_t decorationId)
{
    JSValue obj     = JS_NewObjectClass(ctx, jsDecorationHandleClassId);
    auto *d         = new JsDecorationHandleData;
    d->ownerRef     = JS_DupValue(ctx, owner);
    d->decorationId = decorationId;
    JS_SetOpaque(obj, d);
    return obj;
}

static JsDecorationHandleData *jsDecorationHandleGet(JSValueConst val)
{
    return static_cast<JsDecorationHandleData *>(JS_GetOpaque(val, jsDecorationHandleClassId));
}

// ============================================================================
// AnimationHandle JS class — returned by mb.startAnimation /
// DecorationHandle.animate. Methods: cancel(mode?), onEnd().
// ============================================================================

static JSClassID jsAnimationHandleClassId;

struct JsAnimationHandleData
{
    JSContext *ctx { nullptr };
    // ownerRef survives even if the handle's lifecycle entry has been
    // dropped — kept so settleDecorationAnimation can still find the
    // owning emulator to write the final value, and so the JS-facing
    // identity is stable.
    JSValue ownerRef { JS_UNDEFINED };
    uint64_t handleId { 0 };
    // Settled state — set by Engine::settleDecorationAnimation. After
    // settle, the engine drops the lifecycle entry; subsequent .onEnd()
    // calls return a fresh resolved promise based on `outcome`.
    bool settled { false };
    std::string outcome; // "completed" | "cancelled"
    // Cached promise + resolver for .onEnd(). Created lazily on first
    // call. Resolver is consumed and cleared on settle.
    JSValue promise { JS_UNDEFINED };
    JSValue resolveFn { JS_UNDEFINED };
};

static void jsAnimationHandleFinalize(JSRuntime *rt, JSValue val)
{
    auto *d = static_cast<JsAnimationHandleData *>(JS_GetOpaque(val, jsAnimationHandleClassId));
    if (!d) {
        return;
    }
    // Detach lifecycle's back-pointer so a later settle won't write into
    // freed memory. The animation continues — the timer is still armed and
    // will finalize the emulator-side descriptor on schedule.
    Engine *eng = static_cast<Engine *>(JS_GetRuntimeOpaque(rt));
    if (eng) {
        auto it = eng->animLifecycles().find(d->handleId);
        if (it != eng->animLifecycles().end()) {
            it->second.handleData = nullptr;
        }
    }
    JS_FreeValueRT(rt, d->ownerRef);
    if (!JS_IsUndefined(d->resolveFn)) {
        JS_FreeValueRT(rt, d->resolveFn);
    }
    if (!JS_IsUndefined(d->promise)) {
        JS_FreeValueRT(rt, d->promise);
    }
    delete d;
}

static JSClassDef jsAnimationHandleClassDef = { "AnimationHandle", jsAnimationHandleFinalize };

static JsAnimationHandleData *jsAnimationHandleGet(JSValueConst val)
{
    return static_cast<JsAnimationHandleData *>(JS_GetOpaque(val, jsAnimationHandleClassId));
}

// === Animation parameter parsing ===

static bool parseAnimPropString(std::string_view s, AnimProp &out)
{
    if (s == "bg") {
        out = AnimProp::Bg;
        return true;
    }
    if (s == "fg") {
        out = AnimProp::Fg;
        return true;
    }
    if (s == "underlineColor") {
        out = AnimProp::UnderlineColor;
        return true;
    }
    if (s == "bgInflateX") {
        out = AnimProp::BgInflateX;
        return true;
    }
    if (s == "bgInflateY") {
        out = AnimProp::BgInflateY;
        return true;
    }
    return false;
}

static bool parseEaseString(std::string_view s, Ease &out)
{
    if (s == "linear") {
        out = Ease::Linear;
        return true;
    }
    if (s == "easeIn") {
        out = Ease::EaseIn;
        return true;
    }
    if (s == "easeOut") {
        out = Ease::EaseOut;
        return true;
    }
    if (s == "easeInOut") {
        out = Ease::EaseInOut;
        return true;
    }
    if (s == "easeInOutCubic") {
        out = Ease::EaseInOutCubic;
        return true;
    }
    return false;
}

// Sample the current value of a property on a (possibly running)
// decoration. Used as the default startValue when a new animation is
// installed and the params object didn't provide one explicitly. For
// color props returns 0 if no static value and no active animation.
static int64_t currentDecorationValue(TerminalEmulator *emu, uint64_t decorationId,
                                      AnimProp prop, uint64_t nowMs)
{
    if (!emu) {
        return 0;
    }
    // Check active animation first; if so, sample at nowMs.
    for (const auto &[_, a] : emu->animations()) {
        if (a.kind != AnimTargetKind::Decoration || a.targetId != decorationId || a.prop != prop) {
            continue;
        }
        if (isColorAnimProp(prop)) {
            return static_cast<int64_t>(sampleAnimColor(a.desc, nowMs));
        }
        return static_cast<int64_t>(sampleAnimScalar(a.desc, nowMs));
    }
    // Fall back to the decoration's static style.
    if (const Decoration *dec = emu->decorationById(decorationId)) {
        switch (prop) {
            case AnimProp::Bg:
                return static_cast<int64_t>(dec->style.bg.value_or(0));
            case AnimProp::Fg:
                return static_cast<int64_t>(dec->style.fg.value_or(0));
            case AnimProp::UnderlineColor:
                return dec->style.underline ? static_cast<int64_t>(dec->style.underline->color) : 0;
            case AnimProp::BgInflateX:
                return static_cast<int64_t>(dec->style.bgInflateX);
            case AnimProp::BgInflateY:
                return static_cast<int64_t>(dec->style.bgInflateY);
            case AnimProp::Count:
                return 0;
        }
    }
    return 0;
}

// Build a JS AnimationHandle and register the lifecycle. Caller must have
// already validated `prop`, `params`, and target. Returns a fresh JS object
// or JS_EXCEPTION on failure.
static JSValue startDecorationAnimationImpl(JSContext *ctx, JSValueConst owner,
                                            TerminalEmulator *emu, uint64_t decorationId,
                                            AnimProp prop, JSValueConst paramsObj)
{
    Engine *eng = engineFromCtx(ctx);
    if (!eng || !eng->loop()) {
        return JS_ThrowInternalError(ctx, "engine not initialized");
    }

    // type: default "ease". Anything else throws (v1 only supports the
    // ease type; future "js"/"spring"/etc. extend here).
    JSValue typeV = JS_GetPropertyStr(ctx, paramsObj, "type");
    if (!JS_IsUndefined(typeV) && !JS_IsNull(typeV)) {
        const char *t = JS_ToCString(ctx, typeV);
        if (t) {
            std::string ts(t);
            JS_FreeCString(ctx, t);
            if (ts != "ease") {
                JS_FreeValue(ctx, typeV);
                return JS_ThrowTypeError(ctx, "startAnimation: unsupported type '%s' (v1: only 'ease')", ts.c_str());
            }
        }
    }
    JS_FreeValue(ctx, typeV);

    // durationMs (required, > 0).
    int64_t durationMs = 0;
    {
        JSValue v = JS_GetPropertyStr(ctx, paramsObj, "durationMs");
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx, "startAnimation: durationMs is required");
        }
        if (JS_ToInt64(ctx, &durationMs, v) < 0) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (durationMs <= 0) {
            return JS_ThrowTypeError(ctx, "startAnimation: durationMs must be > 0");
        }
    }

    // endValue (required). For color props, parsed as uint32 RGBA8;
    // for scalar props, parsed as int.
    int64_t endValue = 0;
    {
        JSValue v = JS_GetPropertyStr(ctx, paramsObj, "endValue");
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx, "startAnimation: endValue is required");
        }
        if (isColorAnimProp(prop)) {
            uint32_t u = 0;
            if (JS_ToUint32(ctx, &u, v) < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            endValue = static_cast<int64_t>(u);
        } else {
            if (JS_ToInt64(ctx, &endValue, v) < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }

    // startValue (optional; default = current sampled value).
    uint64_t nowMs = TerminalEmulator::mono();
    int64_t startValue;
    {
        JSValue v = JS_GetPropertyStr(ctx, paramsObj, "startValue");
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            startValue = currentDecorationValue(emu, decorationId, prop, nowMs);
        } else if (isColorAnimProp(prop)) {
            uint32_t u = 0;
            if (JS_ToUint32(ctx, &u, v) < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            startValue = static_cast<int64_t>(u);
        } else {
            if (JS_ToInt64(ctx, &startValue, v) < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }

    // ease (optional; default linear). String-only in v1.
    Ease ease = Ease::Linear;
    {
        JSValue v = JS_GetPropertyStr(ctx, paramsObj, "ease");
        if (JS_IsString(v)) {
            const char *s = JS_ToCString(ctx, v);
            if (s) {
                if (!parseEaseString(s, ease)) {
                    std::string bad = s;
                    JS_FreeCString(ctx, s);
                    JS_FreeValue(ctx, v);
                    return JS_ThrowTypeError(ctx, "startAnimation: unknown ease '%s'", bad.c_str());
                }
                JS_FreeCString(ctx, s);
            }
        } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx, "startAnimation: ease must be a string");
        }
        JS_FreeValue(ctx, v);
    }

    // Allocate handleId, build the Animation entry, install it.
    uint64_t handleId = eng->nextAnimHandleId();
    Animation a;
    a.handleId        = handleId;
    a.kind            = AnimTargetKind::Decoration;
    a.targetId        = decorationId;
    a.prop            = prop;
    a.desc.startMs    = nowMs;
    a.desc.durationMs = static_cast<uint64_t>(durationMs);
    a.desc.startValue = startValue;
    a.desc.endValue   = endValue;
    a.desc.ease       = ease;

    uint64_t priorHandleId = emu->startAnimation(a);

    // Create the JS handle wrapper and register the lifecycle.
    JSValue handleObj = JS_NewObjectClass(ctx, jsAnimationHandleClassId);
    auto *hd          = new JsAnimationHandleData;
    hd->ctx           = ctx;
    hd->ownerRef      = JS_DupValue(ctx, owner);
    hd->handleId      = handleId;
    JS_SetOpaque(handleObj, hd);

    Engine::DecorationAnimLifecycle lc;
    lc.handleId   = handleId;
    lc.handleData = hd;
    lc.handleCtx  = ctx;
    lc.handleRef  = JS_DupValue(ctx, handleObj);
    auto inserted = eng->animLifecycles().emplace(handleId, std::move(lc));

    // Arm the completion timer. On fire: settle as "completed".
    EventLoop *loop                = eng->loop();
    EventLoop::TimerId tid         = loop->addTimer(static_cast<uint64_t>(durationMs), false, [eng, handleId]()
                                            {
                                                eng->settleDecorationAnimation(handleId, "completed",
                                                                               /*snapToEnd=*/true,
                                                                               TerminalEmulator::mono());
                                            });
    inserted.first->second.timerId = tid;

    // If startAnimation replaced a prior animation on the same target/prop
    // slot, settle the prior handle's lifecycle as "cancelled" — the
    // prior emulator entry is already gone (it was erased in the same
    // call), so settle just resolves the prior promise + drops the
    // lifecycle.
    if (priorHandleId != 0 && priorHandleId != handleId) {
        eng->settleDecorationAnimation(priorHandleId, "cancelled",
                                       /*snapToEnd=*/false,
                                       nowMs);
    }

    if (auto &cb = eng->callbacks().requestRedraw) {
        cb();
    }

    return handleObj;
}

// AnimationHandle.cancel(mode?) — mode is "stop" (default) or "snap-to-end".
static JSValue jsAnimationHandleCancel(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    auto *hd = jsAnimationHandleGet(this_val);
    if (!hd) {
        return JS_ThrowTypeError(ctx, "not an AnimationHandle");
    }
    if (hd->settled) {
        return JS_UNDEFINED;
    }
    bool snapToEnd = false;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) {
            std::string mode = s;
            JS_FreeCString(ctx, s);
            if (mode == "snap-to-end") {
                snapToEnd = true;
            } else if (mode != "stop") {
                return JS_ThrowTypeError(ctx, "cancel: mode must be 'stop' or 'snap-to-end'");
            }
        }
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng) {
        return JS_ThrowInternalError(ctx, "engine gone");
    }
    const char *outcome = snapToEnd ? "completed" : "cancelled";
    eng->settleDecorationAnimation(hd->handleId, outcome, snapToEnd, TerminalEmulator::mono());
    return JS_UNDEFINED;
}

// AnimationHandle.onEnd() — returns a Promise that resolves with
// "completed" or "cancelled". Called more than once returns the same
// (cached) promise.
static JSValue jsAnimationHandleOnEnd(JSContext *ctx, JSValueConst this_val,
                                      int /*argc*/, JSValueConst * /*argv*/)
{
    auto *hd = jsAnimationHandleGet(this_val);
    if (!hd) {
        return JS_ThrowTypeError(ctx, "not an AnimationHandle");
    }
    if (!JS_IsUndefined(hd->promise)) {
        return JS_DupValue(ctx, hd->promise);
    }
    if (hd->settled) {
        // Already settled — return a fresh resolved promise.
        JSValue resolving[2];
        JSValue p   = JS_NewPromiseCapability(ctx, resolving);
        JSValue arg = JS_NewString(ctx, hd->outcome.c_str());
        JSValue r   = JS_Call(ctx, resolving[0], JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        hd->promise = JS_DupValue(ctx, p);
        return p;
    }
    // Pending — capture resolve so settle can fire.
    JSValue resolving[2];
    JSValue p     = JS_NewPromiseCapability(ctx, resolving);
    hd->resolveFn = resolving[0];
    JS_FreeValue(ctx, resolving[1]); // reject — unused
    hd->promise = JS_DupValue(ctx, p);
    return p;
}

static const JSCFunctionListEntry jsAnimationHandleProto[] = {
    JS_CFUNC_DEF("cancel", 1, jsAnimationHandleCancel),
    JS_CFUNC_DEF("onEnd", 0, jsAnimationHandleOnEnd),
};

// DecorationHandle.id getter
static JSValue jsDecorationHandleGetId(JSContext *ctx, JSValueConst this_val)
{
    auto *d = jsDecorationHandleGet(this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "not a DecorationHandle");
    }
    return JS_NewInt64(ctx, static_cast<int64_t>(d->decorationId));
}

// DecorationHandle.remove() — drops the underlying decoration and cancels
// any animations on it.
static JSValue jsDecorationHandleRemove(JSContext *ctx, JSValueConst this_val,
                                        int /*argc*/, JSValueConst * /*argv*/)
{
    REQUIRE_PERM(ctx, PaneRead);
    auto *d = jsDecorationHandleGet(this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "not a DecorationHandle");
    }
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, d->ownerRef);
    if (!emu) {
        return JS_NewBool(ctx, false);
    }
    Engine *eng = engineFromCtx(ctx);
    std::vector<uint64_t> cancelled;
    bool ok = emu->removeDecoration(d->decorationId, &cancelled);
    if (eng) {
        uint64_t nowMs = TerminalEmulator::mono();
        for (uint64_t hid : cancelled) {
            eng->settleDecorationAnimation(hid, "cancelled", /*snapToEnd=*/false, nowMs);
        }
        if (ok) {
            if (auto &cb = eng->callbacks().requestRedraw) {
                cb();
            }
        }
    }
    return JS_NewBool(ctx, ok);
}

// DecorationHandle.animate(propString, params) — sugar for
// mb.startAnimation(this, propString, params).
static JSValue jsDecorationHandleAnimate(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, PaneRead);
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "animate requires (propString, params)");
    }
    auto *d = jsDecorationHandleGet(this_val);
    if (!d) {
        return JS_ThrowTypeError(ctx, "not a DecorationHandle");
    }
    if (!JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "animate: propString must be a string");
    }
    if (!JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "animate: params must be an object");
    }
    AnimProp prop;
    {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (!s) {
            return JS_EXCEPTION;
        }
        std::string ps(s);
        JS_FreeCString(ctx, s);
        if (!parseAnimPropString(ps, prop)) {
            return JS_ThrowTypeError(ctx, "animate: unknown property '%s'", ps.c_str());
        }
    }
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, d->ownerRef);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "decoration's owner is destroyed");
    }
    return startDecorationAnimationImpl(ctx, d->ownerRef, emu, d->decorationId, prop, argv[1]);
}

static const JSCFunctionListEntry jsDecorationHandleProto[] = {
    JS_CGETSET_DEF("id", jsDecorationHandleGetId, nullptr),
    JS_CFUNC_DEF("remove", 0, jsDecorationHandleRemove),
    JS_CFUNC_DEF("animate", 2, jsDecorationHandleAnimate),
};

// mb._setMonoForTest(ms) — internal. Overrides TerminalEmulator::mono()
// process-wide so animation render tests can pin the clock at specific
// points in an animation's timeline. Pass 0 to restore wall-clock.
// Underscore-prefixed because it is test-only and not part of the
// public applet API.
static JSValue jsMbSetMonoForTest(JSContext *ctx, JSValueConst /*this_val*/,
                                  int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "_setMonoForTest requires (ms)");
    }
    int64_t ms = 0;
    if (JS_ToInt64(ctx, &ms, argv[0]) < 0) {
        return JS_EXCEPTION;
    }
    TerminalEmulator::setMonoForTest(static_cast<uint64_t>(ms));
    if (auto &cb = engineFromCtx(ctx)->callbacks().requestRedraw) {
        cb();
    }
    return JS_UNDEFINED;
}

// mb.startAnimation(handle, propString, params) — equivalent to
// handle.animate(propString, params). v1 only accepts DecorationHandle.
static JSValue jsMbStartAnimation(JSContext *ctx, JSValueConst /*this_val*/,
                                  int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, PaneRead);
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "startAnimation requires (target, propString, params)");
    }
    auto *d = static_cast<JsDecorationHandleData *>(JS_GetOpaque(argv[0], jsDecorationHandleClassId));
    if (!d) {
        return JS_ThrowTypeError(ctx, "startAnimation: target must be a DecorationHandle (v1)");
    }
    if (!JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx, "startAnimation: propString must be a string");
    }
    if (!JS_IsObject(argv[2])) {
        return JS_ThrowTypeError(ctx, "startAnimation: params must be an object");
    }
    AnimProp prop;
    {
        const char *s = JS_ToCString(ctx, argv[1]);
        if (!s) {
            return JS_EXCEPTION;
        }
        std::string ps(s);
        JS_FreeCString(ctx, s);
        if (!parseAnimPropString(ps, prop)) {
            return JS_ThrowTypeError(ctx, "startAnimation: unknown property '%s'", ps.c_str());
        }
    }
    TerminalEmulator *emu = resolveEmulatorFromVal(ctx, d->ownerRef);
    if (!emu) {
        return JS_ThrowTypeError(ctx, "decoration's owner is destroyed");
    }
    return startDecorationAnimationImpl(ctx, d->ownerRef, emu, d->decorationId, prop, argv[2]);
}

static const JSCFunctionListEntry jsTerminalProto[] = {
    JS_CFUNC_DEF("inject", 1, jsTerminalInject),
    JS_CFUNC_DEF("addDecoration", 1, jsTerminalAddDecoration),
    JS_CFUNC_DEF("removeDecoration", 1, jsTerminalRemoveDecoration),
    JS_CFUNC_DEF("clearDecorations", 0, jsTerminalClearDecorations),
    JS_CFUNC_DEF("createDecorationBatch", 0, jsTerminalCreateDecorationBatch),
    JS_CGETSET_MAGIC_DEF("cols", jsTerminalGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("rows", jsTerminalGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("cursor", jsTerminalGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("kind", jsTerminalGetProp, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("cellWidth", jsTerminalGetProp, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("cellHeight", jsTerminalGetProp, nullptr, 5),
};

// pane.createEmbeddedTerminal({rows}) -> EmbeddedTerminal | null
static JSValue jsPaneCreateEmbedded(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "createEmbeddedTerminal requires ({rows})");
    }
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_ThrowTypeError(ctx, "pane is destroyed");
    }
    Engine *eng = engineFromCtx(ctx);

    int32_t rows = 0;
    JSValue v    = JS_GetPropertyStr(ctx, argv[0], "rows");
    JS_ToInt32(ctx, &rows, v);
    JS_FreeValue(ctx, v);
    if (rows <= 0) {
        return JS_ThrowTypeError(ctx, "createEmbeddedTerminal: 'rows' must be > 0");
    }

    Uuid paneId     = pane->id;
    uint64_t lineId = eng->callbacks().createEmbedded(paneId, rows);

    if (lineId == 0) {
        return JS_NULL;
    }

    auto *inst = instanceFromCtx(ctx);
    if (inst) {
        inst->ownedEmbeddeds.push_back({ paneId, lineId });
    }

    return jsEmbeddedNew(ctx, paneId, lineId);
}

// pane.embeddeds — array of EmbeddedTerminal objects for this pane
static JSValue jsPaneGetEmbeddeds(JSContext *ctx, JSValueConst this_val)
{
    auto *pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) {
        return JS_NewArray(ctx);
    }
    Engine *eng = engineFromCtx(ctx);
    auto embeds = eng->callbacks().paneEmbeddeds(pane->id);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < embeds.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, jsEmbeddedNew(ctx, pane->id, embeds[i].lineId));
    }
    return arr;
}

// JS `Tab` class is gone. Tab identity is the subtreeRoot Uuid string —
// scripts handle tabs as bare UUIDs, query their structure through
// `mb.layout.queryNodes("Stack", root)` / `mb.layout.node(uuid)`, and
// build Pane objects via `mb.pane(uuid)` from `queryNodes("Terminal", tab)`.

// ============================================================================
// mb global — controller API
// ============================================================================

static JSValue jsMbInvokeAction(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "invokeAction requires (name, ...args)");
    }
    REQUIRE_PERM(ctx, ActionsInvoke);
    Engine *eng      = engineFromCtx(ctx);
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }

    uint32_t extraPerm = actionPermission(std::string(name));
    if (extraPerm && !checkPerm(ctx, extraPerm)) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "permission denied: action requires additional permissions");
    }

    if (eng->callbacks().hasActiveTab && !eng->callbacks().hasActiveTab()) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "no active tab");
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const char *arg = JS_ToCString(ctx, argv[i]);
        if (arg) {
            args.emplace_back(arg);
            JS_FreeCString(ctx, arg);
        }
    }
    bool ok = eng->callbacks().invokeAction(std::string(name), args);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

// mb.addEventListener("action", "ActionName", fn) — 3 args
// mb.addEventListener("tabCreated", fn) — 2 args
static JSValue jsMbAddEventListener(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, ...)");
    }

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }

    std::string prop;
    JSValueConst callback;

    if (strcmp(event, "action") == 0) {
        // Three-arg form: ("action", "ActionName", fn)
        if (argc < 3 || !JS_IsString(argv[1]) || !JS_IsFunction(ctx, argv[2])) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "addEventListener('action', ...) requires (string, string, function)");
        }
        const char *actionName = JS_ToCString(ctx, argv[1]);
        if (!actionName) {
            JS_FreeCString(ctx, event);
            return JS_EXCEPTION;
        }
        prop = std::string("__evt_action_") + actionName;
        JS_FreeCString(ctx, actionName);
        callback = argv[2];
    } else {
        // Two-arg form: ("tabCreated", fn)
        if (!JS_IsFunction(ctx, argv[1])) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
        }
        prop     = std::string("__evt_") + event;
        callback = argv[1];
    }
    JS_FreeCString(ctx, event);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb     = JS_GetPropertyStr(ctx, global, "mb");
    JSValue arr    = JS_GetPropertyStr(ctx, mb, prop.c_str());
    if (JS_IsUndefined(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, mb, prop.c_str(), JS_DupValue(ctx, arr));
    }
    JSValue pushFn = JS_GetPropertyStr(ctx, arr, "push");
    JS_Call(ctx, pushFn, arr, 1, &callback);
    JS_FreeValue(ctx, pushFn);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, mb);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

static JSValue jsMbRemoveEventListener(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, ...)");
    }

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        return JS_EXCEPTION;
    }

    std::string prop;
    JSValueConst callback;
    if (strcmp(event, "action") == 0) {
        if (argc < 3 || !JS_IsString(argv[1]) || !JS_IsFunction(ctx, argv[2])) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "removeEventListener('action', ...) requires (string, string, function)");
        }
        const char *actionName = JS_ToCString(ctx, argv[1]);
        if (!actionName) {
            JS_FreeCString(ctx, event);
            return JS_EXCEPTION;
        }
        prop = std::string("__evt_action_") + actionName;
        JS_FreeCString(ctx, actionName);
        callback = argv[2];
    } else {
        if (!JS_IsFunction(ctx, argv[1])) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
        }
        prop     = std::string("__evt_") + event;
        callback = argv[1];
    }
    JS_FreeCString(ctx, event);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb     = JS_GetPropertyStr(ctx, global, "mb");
    JSValue arr    = JS_GetPropertyStr(ctx, mb, prop.c_str());
    removeFromJSArray(ctx, arr, callback);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, mb);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

// Walk every own property of `obj` and set those whose name starts with
// `prefix` to undefined. Used by removeAllListeners to nuke entire
// `__evt_*` families in one pass — notify* paths treat undefined as
// "no listeners" so this is functionally equivalent to deleting the
// property but cheaper and survives subsequent addEventListener calls
// (which lazily recreate the array on next push).
static void clearOwnPropsWithPrefix(JSContext *ctx, JSValueConst obj,
                                    const char *prefix)
{
    JSPropertyEnum *tab = nullptr;
    uint32_t len        = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        return;
    }
    const size_t prefixLen = std::strlen(prefix);
    for (uint32_t i = 0; i < len; ++i) {
        const char *name = JS_AtomToCString(ctx, tab[i].atom);
        if (name && std::strncmp(name, prefix, prefixLen) == 0) {
            JS_SetProperty(ctx, obj, tab[i].atom, JS_UNDEFINED);
        }
        if (name) {
            JS_FreeCString(ctx, name);
        }
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
}

// mb.removeAllListeners()                      -> clear every __evt_* family
// mb.removeAllListeners(eventName)             -> clear __evt_<eventName>
// mb.removeAllListeners("action")              -> clear every __evt_action_*
// mb.removeAllListeners("action", actionName)  -> clear __evt_action_<actionName>
static JSValue jsMbRemoveAllListeners(JSContext *ctx, JSValueConst,
                                      int argc, JSValueConst *argv)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb     = JS_GetPropertyStr(ctx, global, "mb");

    if (argc == 0) {
        clearOwnPropsWithPrefix(ctx, mb, "__evt_");
        JS_FreeValue(ctx, mb);
        JS_FreeValue(ctx, global);
        return JS_UNDEFINED;
    }

    if (!JS_IsString(argv[0])) {
        JS_FreeValue(ctx, mb);
        JS_FreeValue(ctx, global);
        return JS_ThrowTypeError(ctx,
                                 "removeAllListeners requires no args, an "
                                 "event string, or ('action', actionName)");
    }

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) {
        JS_FreeValue(ctx, mb);
        JS_FreeValue(ctx, global);
        return JS_EXCEPTION;
    }

    if (std::strcmp(event, "action") == 0) {
        if (argc >= 2 && JS_IsString(argv[1])) {
            const char *actionName = JS_ToCString(ctx, argv[1]);
            if (actionName) {
                std::string prop = std::string("__evt_action_") + actionName;
                JS_SetPropertyStr(ctx, mb, prop.c_str(), JS_UNDEFINED);
                JS_FreeCString(ctx, actionName);
            }
        } else {
            clearOwnPropsWithPrefix(ctx, mb, "__evt_action_");
        }
    } else {
        std::string prop = std::string("__evt_") + event;
        JS_SetPropertyStr(ctx, mb, prop.c_str(), JS_UNDEFINED);
    }

    JS_FreeCString(ctx, event);
    JS_FreeValue(ctx, mb);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

static JSValue jsMbGetActivePane(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    // Engine holds the global focused-terminal Uuid — no need to walk through
    // the tabs() callback (which is itself derived from the layout tree).
    Engine *eng  = engineFromCtx(ctx);
    Uuid focused = eng->focusedTerminalNodeId();
    if (focused.isNil() || !eng->terminal(focused)) {
        return JS_UNDEFINED;
    }
    return jsPaneNew(ctx, focused);
}

// PascalCase → snake_case: "FocusPane" → "focus_pane"
static std::string toSnakeCase(std::string_view name)
{
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 0) {
                result += '_';
            }
            result += static_cast<char>(c + 32);
        } else {
            result += c;
        }
    }
    return result;
}

// PascalCase → spaced label: "FocusPane" → "Focus Pane"
static std::string toLabel(std::string_view name)
{
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z' && i > 0) {
            result += ' ';
        }
        result += c;
    }
    return result;
}

static JSValue jsMbActionsRegister(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue jsMbActionsUnregister(JSContext *, JSValueConst, int, JSValueConst *);

// mb.actions -> array of {name, label, builtin, args?} objects plus
// register/unregister methods for handler ownership of JS-owned actions.
static JSValue jsMbGetActions(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    Engine *eng = engineFromCtx(ctx);

    // Directional arg expansions for actions that take Direction
    struct ArgVariant
    {
        const char *arg;
        const char *labelSuffix;
    };

    static const std::unordered_map<std::string_view, std::vector<ArgVariant>> argVariants = {
        { "SplitPane", { { "right", "Right" }, { "down", "Down" }, { "left", "Left" }, { "up", "Up" } } },
        { "FocusPane", { { "next", "Next" }, { "prev", "Previous" }, { "left", "Left" }, { "right", "Right" }, { "up", "Up" }, { "down", "Down" } } },
        { "AdjustPaneSize", { { "left", "Left" }, { "right", "Right" }, { "up", "Up" }, { "down", "Down" } } },
        { "ScrollToPrompt", { { "-1", "Previous" }, { "1", "Next" } } },
        { "ActivateTabRelative", { { "-1", "Left" }, { "1", "Right" } } },
        { "MoveTab", { { "left", "Left" }, { "right", "Right" } } },
        { "SwapPane", { { "left", "Left" }, { "right", "Right" }, { "up", "Up" }, { "down", "Down" }, { "next", "Next" }, { "prev", "Previous" } } },
        { "RotatePanes", { { "cw", "Clockwise" }, { "ccw", "Counterclockwise" } } },
        { "Clear", { { "scrollback", "Scrollback" }, { "all", "All" } } },
    };

    JSValue arr  = JS_NewArray(ctx);
    uint32_t idx = 0;

    for (Action::TypeIndex i = 0; i < Action::count; ++i) {
        auto pascalName = Action::nameTable[i];
        // Skip ScriptAction — script actions come from the registered set below
        if (pascalName == "ScriptAction") {
            continue;
        }

        std::string snakeName = toSnakeCase(pascalName);
        std::string baseLabel = toLabel(pascalName);

        auto vit = argVariants.find(pascalName);
        if (vit != argVariants.end()) {
            // Expand into one entry per variant
            for (const auto &v : vit->second) {
                JSValue obj = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, snakeName.c_str()));
                std::string label = baseLabel + " " + v.labelSuffix;
                JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, label.c_str()));
                JS_SetPropertyStr(ctx, obj, "builtin", JS_TRUE);
                JSValue args = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, args, 0, JS_NewString(ctx, v.arg));
                JS_SetPropertyStr(ctx, obj, "args", args);
                JS_SetPropertyUint32(ctx, arr, idx++, obj);
            }
        } else {
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, snakeName.c_str()));
            JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, baseLabel.c_str()));
            JS_SetPropertyStr(ctx, obj, "builtin", JS_TRUE);
            JS_SetPropertyUint32(ctx, arr, idx++, obj);
        }
    }

    // Script actions
    for (const auto &fullName : eng->registeredActions()) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, fullName.c_str()));
        JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, fullName.c_str()));
        JS_SetPropertyStr(ctx, obj, "builtin", JS_FALSE);
        JS_SetPropertyUint32(ctx, arr, idx++, obj);
    }

    // Attach handler-registry methods to the returned array. The array is
    // regenerated on every `mb.actions` access, but consumers that capture it
    // into a local (`const a = mb.actions; a.register(...)`) still have the
    // methods bound.
    JS_SetPropertyStr(ctx, arr, "register", JS_NewCFunction(ctx, jsMbActionsRegister, "register", 2));
    JS_SetPropertyStr(ctx, arr, "unregister", JS_NewCFunction(ctx, jsMbActionsUnregister, "unregister", 1));

    return arr;
}

// mb.createUuid() -> 36-char UUID v4 string ("xxxxxxxx-xxxx-4xxx-Nxxx-xxxxxxxxxxxx").
// Ungated — providing randomness confers no capability. String form is the only
// JS-safe representation; 128-bit integers don't round-trip through JS Number.
static JSValue jsMbCreateUuid(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    std::string s = Uuid::generate().toString();
    return JS_NewStringLen(ctx, s.data(), s.size());
}

// mb.createSecureToken(length = 32) -> hex string
// Generates `length` cryptographically-secure random bytes and returns them as a 2*length
// hex string. Ungated — providing randomness confers no capability.
static JSValue jsMbCreateSecureToken(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    int length = 32;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t n;
        if (JS_ToInt32(ctx, &n, argv[0]) == 0 && n > 0 && n <= 4096) {
            length = n;
        }
    }

    std::vector<unsigned char> buf(static_cast<size_t>(length));
    fillSecureRandom(buf.data(), buf.size());

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<size_t>(length) * 2);
    for (size_t i = 0; i < buf.size(); ++i) {
        result += hex[buf[i] >> 4];
        result += hex[buf[i] & 0x0F];
    }
    return JS_NewStringLen(ctx, result.data(), result.size());
}

// Convert a LoadResult to a JS { status, id?, error? } object.
static JSValue loadResultToJs(JSContext *ctx, const Engine::LoadResult &res)
{
    JSValue obj           = JS_NewObject(ctx);
    const char *statusStr = "error";
    switch (res.status) {
        case Engine::LoadResult::Status::Loaded: statusStr = "loaded"; break;
        case Engine::LoadResult::Status::Pending: statusStr = "pending"; break;
        case Engine::LoadResult::Status::Denied: statusStr = "denied"; break;
        case Engine::LoadResult::Status::Error: statusStr = "error"; break;
    }
    JS_SetPropertyStr(ctx, obj, "status", JS_NewString(ctx, statusStr));
    if (res.status == Engine::LoadResult::Status::Loaded) {
        JS_SetPropertyStr(ctx, obj, "id", JS_NewInt64(ctx, static_cast<int64_t>(res.id)));
    }
    if (res.status == Engine::LoadResult::Status::Error && !res.error.empty()) {
        JS_SetPropertyStr(ctx, obj, "error", JS_NewStringLen(ctx, res.error.data(), res.error.size()));
    }
    return obj;
}

// mb.loadScript(path, permissionsStr) -> { status, id?, error? }
//
// Privilege-elevation gate: if `permissionsStr` includes "builtin"
// (which parses to Perm::All | Perm::BuiltIn), we require the calling
// instance to itself be built-in. A user script that names "builtin"
// is treated as a permission violation — same kill-the-context
// semantics as accessing shell.commands without the grant. The
// rationale: built-ins can already get the same effect by importing
// arbitrary files from their directory (which run with the importer's
// permissions in the same context), so explicitly allowing them to
// spawn separate built-in instances doesn't grant new authority — it
// just gives them lifecycle control. User scripts must NEVER be able
// to escape their sandbox, so the attempt is a hard error.
static JSValue jsMbLoadScript(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        Engine::LoadResult res { Engine::LoadResult::Status::Error, 0, "path argument required" };
        return loadResultToJs(ctx, res);
    }
    REQUIRE_PERM(ctx, ScriptsLoad);
    Engine *eng = engineFromCtx(ctx);

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_EXCEPTION;
    }

    std::string permsStr;
    if (argc >= 2) {
        const char *p = JS_ToCString(ctx, argv[1]);
        if (p) {
            permsStr = p;
            JS_FreeCString(ctx, p);
        }
    }

    uint32_t perms = parsePermissions(permsStr);

    // Built-in elevation gate. The Perm::BuiltIn bit only enters the
    // bitmask via the "builtin" token (and possibly via a hand-edited
    // allowlist file, but the load path here doesn't consult that
    // before this check). Callers that request elevation but aren't
    // themselves built-in get the same treatment as any other
    // permission violation: the calling instance is scheduled for
    // termination and we throw rather than returning a misleading
    // success / pending result.
    if (perms & Perm::BuiltIn) {
        auto *inst = instanceFromCtx(ctx);
        if (!inst || !inst->builtIn) {
            JS_FreeCString(ctx, path);
            sLog().error("ScriptEngine: user script attempted to load '{}' as built-in",
                         inst ? inst->path : std::string("(unknown)"));
            scheduleTermination(ctx);
            return JS_ThrowTypeError(ctx,
                                     "loadScript: 'builtin' permission may only be requested by built-in scripts");
        }
    }

    Engine::LoadResult res = eng->loadScript(std::string(path), perms);
    JS_FreeCString(ctx, path);
    return loadResultToJs(ctx, res);
}

// mb.approveScript(path, response) — response is "y", "n", "a", "d".
// Returns the final LoadResult as { status, id?, error? }.
static JSValue jsMbApproveScript(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 2) {
        Engine::LoadResult res { Engine::LoadResult::Status::Error, 0, "approveScript requires path and response" };
        return loadResultToJs(ctx, res);
    }
    // Only built-in scripts can approve
    auto *inst = instanceFromCtx(ctx);
    if (!inst || !inst->builtIn) {
        return JS_ThrowTypeError(ctx, "approveScript is only available to built-in scripts");
    }

    Engine *eng      = engineFromCtx(ctx);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_EXCEPTION;
    }
    const char *resp = JS_ToCString(ctx, argv[1]);
    if (!resp) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }

    char response = resp[0];
    JS_FreeCString(ctx, resp);

    Engine::LoadResult res = eng->approveScript(std::string(path), response);
    JS_FreeCString(ctx, path);
    return loadResultToJs(ctx, res);
}

// mb.unloadScript(id)
static JSValue jsMbUnloadScript(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "unloadScript requires (id)");
    }
    REQUIRE_PERM(ctx, ScriptsUnload);
    Engine *eng = engineFromCtx(ctx);
    int64_t id;
    JS_ToInt64(ctx, &id, argv[0]);
    if (id > 0) {
        eng->unload(static_cast<uint64_t>(id));
    }
    return JS_UNDEFINED;
}

// mb.exit() — unload the calling script instance via a zero-delay timer
static JSValue jsMbExit(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    Engine *eng = engineFromCtx(ctx);
    auto *inst  = instanceFromCtx(ctx);
    if (!inst) {
        return JS_UNDEFINED;
    }
    if (inst->builtIn) {
        return JS_ThrowTypeError(ctx, "built-in scripts cannot call exit()");
    }

    InstanceId id = inst->id;
    if (eng->loop()) {
        eng->loop()->addTimer(0, false, [eng, id]()
                              {
                                  eng->unload(id);
                              });
    } else {
        eng->unload(id);
    }

    return JS_UNDEFINED;
}

// mb.quit() — quit the application. Distinct from mb.exit() (which only
// unloads the calling script instance). Used by the default UI controller
// when the last terminal has exited.
static JSValue jsMbQuit(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    Engine *eng = engineFromCtx(ctx);
    if (eng && eng->callbacks().quit) {
        eng->callbacks().quit();
    }
    return JS_UNDEFINED;
}

// mb.hasPermission(name) → bool. Non-destructive query (no termination on
// false, unlike REQUIRE_PERM). Accepts individual permission names
// ("ui.focus", "config.modify") and group names ("ui", "io"). Group names
// return true only when ALL bits in the group are granted, matching the
// "this script has the full group" semantic. Unknown names return false.
static JSValue jsMbHasPermission(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_NewBool(ctx, false);
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_NewBool(ctx, false);
    }
    uint32_t bits = Script::parsePermissions(name);
    JS_FreeCString(ctx, name);
    if (bits == 0) {
        return JS_NewBool(ctx, false); // unknown name
    }
    auto *inst = instanceFromCtx(ctx);
    if (!inst) {
        return JS_NewBool(ctx, false);
    }
    return JS_NewBool(ctx, (inst->permissions & bits) == bits);
}

// mb.process.spawn(path, args?, opts?) -> number (pid)
//
// Detached, fire-and-forget process launch. Wraps platformSpawnDetached
// via the AppCallbacks::spawnProcess shim. Use cases: open the user's
// $EDITOR on a file, fire xdg-open on a URL, run an out-of-band linter,
// poke a desktop notification daemon — anything that doesn't belong in
// a terminal pane.
//
// Args:
//   path  : string. PATH-resolved via execvp(3); may be a basename or
//           absolute path.
//   args  : string[] (default []). argv[1..N] as the child sees them.
//           argv[0] is set to `path` automatically — callers don't need
//           to repeat it. Pass any positional / option flags here.
//   opts  : { cwd?: string; env?: { [k: string]: string } } (default {}).
//           cwd: chdir target before exec; empty / omitted → inherit
//                mb's cwd. Failure to chdir aborts the spawn (logged
//                from the parent via the errno-pipe path; from the
//                script's perspective the spawn just silently fails).
//           env: per-key merge over the inherited environment. Missing
//                key in `env` → inherited value preserved. Present key
//                with empty string → key is set to empty (NOT removed).
//                v1 has no key-removal syntax.
//
// Permissions: process.spawn (group: process). Built-ins get it via
// Perm::All. No second permission required — the model is "either you
// can spawn external processes or you can't"; we don't try to scope by
// path or argv pattern (impossible to enforce safely against a
// determined script anyway).
//
// Returns:
//   pid : intermediate-child pid on successful fork. Note this is NOT
//         the grandchild's pid (which is what actually runs the binary)
//         — there's no portable way to surface that without breaking
//         the detached double-fork pattern. v1 has no API for waiting
//         on / killing the spawned process; if/when a tracked variant
//         is added, it'll return a handle object, not just a pid.
//   0   : pre-fork failure (e.g. ENOMEM). Logged on the C++ side; the
//         JS caller sees a 0 return and should fall back gracefully.
//
// Throws on permission denial, missing/invalid args, or an internal
// platform-callback being unwired (shouldn't happen in production —
// this is a "did you forget to call setCallbacks?" guard).
//
// Does NOT throw on grandchild exec failure — that's lossy by design
// (see the platform-side comment on platformSpawnDetached). Scripts
// that need exec-success confirmation must check via some external
// signal (e.g. the spawned process's own side effects).
static JSValue jsMbProcessSpawn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "process.spawn requires (path: string, args?: string[], opts?: object)");
    }
    REQUIRE_PERM(ctx, ProcessSpawn);

    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().spawnProcess) {
        return JS_ThrowTypeError(ctx, "process.spawn: not wired (internal: AppCallbacks.spawnProcess unset)");
    }

    AppCallbacks::ProcessSpawnReq req;

    // path (mandatory)
    {
        const char *p = JS_ToCString(ctx, argv[0]);
        if (!p) {
            return JS_EXCEPTION;
        }
        req.path = p;
        JS_FreeCString(ctx, p);
        if (req.path.empty()) {
            return JS_ThrowTypeError(ctx, "process.spawn: path must be non-empty");
        }
    }

    // args (optional). Implicit argv[0] = path is added by
    // platformSpawnDetached when the caller's argv is empty, so we
    // mirror that here: if the caller supplied an args array, prepend
    // path so the layout the platform sees is [path, ...args]. If the
    // caller supplied nothing (or undefined / null), pass empty and
    // let the platform default kick in.
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (!JS_IsArray(argv[1])) {
            return JS_ThrowTypeError(ctx, "process.spawn: args must be an array of strings");
        }
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        uint32_t len   = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        // Reserve path + len. argv[0] is the program name as the child
        // sees it — defaults to `path`. Scripts that want a different
        // argv[0] (rare; e.g. busybox-style multicall binaries) can
        // pass it as args[0] and we'll pass it through verbatim.
        req.argv.reserve(len + 1);
        req.argv.push_back(req.path);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue elt = JS_GetPropertyUint32(ctx, argv[1], i);
            if (!JS_IsString(elt)) {
                JS_FreeValue(ctx, elt);
                return JS_ThrowTypeError(ctx, "process.spawn: args[%u] is not a string", i);
            }
            const char *s = JS_ToCString(ctx, elt);
            if (!s) {
                JS_FreeValue(ctx, elt);
                return JS_EXCEPTION;
            }
            req.argv.emplace_back(s);
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, elt);
        }
    }

    // opts (optional)
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (!JS_IsObject(argv[2])) {
            return JS_ThrowTypeError(ctx, "process.spawn: opts must be an object");
        }

        // cwd
        JSValue cwdVal = JS_GetPropertyStr(ctx, argv[2], "cwd");
        if (!JS_IsUndefined(cwdVal) && !JS_IsNull(cwdVal)) {
            if (!JS_IsString(cwdVal)) {
                JS_FreeValue(ctx, cwdVal);
                return JS_ThrowTypeError(ctx, "process.spawn: opts.cwd must be a string");
            }
            const char *s = JS_ToCString(ctx, cwdVal);
            if (s) {
                req.cwd = s;
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, cwdVal);

        // env: { K: V, ... }. Iterating own enumerable string-keyed
        // props matches Node child_process semantics. We don't try to
        // detect Symbol-keyed entries — strings only.
        JSValue envVal = JS_GetPropertyStr(ctx, argv[2], "env");
        if (!JS_IsUndefined(envVal) && !JS_IsNull(envVal)) {
            if (!JS_IsObject(envVal)) {
                JS_FreeValue(ctx, envVal);
                return JS_ThrowTypeError(ctx, "process.spawn: opts.env must be an object");
            }
            JSPropertyEnum *props = nullptr;
            uint32_t plen         = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &plen, envVal, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < plen; ++i) {
                    const char *keyC = JS_AtomToCString(ctx, props[i].atom);
                    JSValue v        = JS_GetProperty(ctx, envVal, props[i].atom);
                    if (keyC && JS_IsString(v)) {
                        const char *valC = JS_ToCString(ctx, v);
                        if (valC) {
                            req.env.emplace_back(keyC, valC);
                            JS_FreeCString(ctx, valC);
                        }
                    }
                    if (keyC) {
                        JS_FreeCString(ctx, keyC);
                    }
                    JS_FreeValue(ctx, v);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, envVal);
    }

    pid_t pid = eng->callbacks().spawnProcess(req);
    return JS_NewInt32(ctx, static_cast<int32_t>(pid));
}

// mb.setNamespace(name) — claim a namespace for this script instance
static JSValue jsMbSetNamespace(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "setNamespace requires a non-empty string argument");
    }

    const char *ns = JS_ToCString(ctx, argv[0]);
    if (!ns) {
        return JS_EXCEPTION;
    }
    std::string nsStr(ns);
    JS_FreeCString(ctx, ns);

    if (nsStr.empty()) {
        return JS_ThrowTypeError(ctx, "namespace must not be empty");
    }

    Engine *eng = engineFromCtx(ctx);
    auto *inst  = instanceFromCtx(ctx);
    if (!inst) {
        return JS_ThrowTypeError(ctx, "no script instance");
    }

    if (!eng->setNamespace(inst->id, nsStr)) {
        return JS_ThrowTypeError(ctx, "namespace '%s' is already taken or already set", nsStr.c_str());
    }

    return JS_UNDEFINED;
}

// mb.registerAction(name) — register a script action as "namespace.name"
static JSValue jsMbRegisterAction(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ActionsInvoke);
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "registerAction requires a string argument");
    }

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    std::string nameStr(name);
    JS_FreeCString(ctx, name);

    Engine *eng = engineFromCtx(ctx);
    auto *inst  = instanceFromCtx(ctx);
    if (!inst) {
        return JS_ThrowTypeError(ctx, "no script instance");
    }

    if (!eng->registerAction(inst->id, nameStr)) {
        return JS_ThrowTypeError(ctx, "registerAction failed: namespace not set, "
                                      "or action already registered by a different instance");
    }

    return JS_UNDEFINED;
}

// ============================================================================
// Microtask event dispatch
// ============================================================================

static JSValue eventJobFunc(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_UNDEFINED;
    }
    JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, argc - 1, argv + 1);
    if (JS_IsException(ret)) {
        JSValue exc   = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        sLog().error("ScriptEngine: event handler error: {}", s ? s : "(null)");
        if (s) {
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);
    return JS_UNDEFINED;
}

static void enqueueListeners(JSContext *ctx, JSValue arr, int extraArgc, JSValue *extraArgv)
{
    if (JS_IsUndefined(arr)) {
        return;
    }
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len    = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

    for (int32_t i = 0; i < len; ++i) {
        JSValue fn = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsFunction(ctx, fn)) {
            std::vector<JSValue> jobArgs;
            jobArgs.push_back(fn);
            for (int j = 0; j < extraArgc; ++j) {
                jobArgs.push_back(extraArgv[j]);
            }
            JS_EnqueueJob(ctx, eventJobFunc, static_cast<int>(jobArgs.size()), jobArgs.data());
        }
        JS_FreeValue(ctx, fn);
    }
}

// ============================================================================
// ============================================================================
// setTimeout / setInterval / clearTimeout / clearInterval
// ============================================================================

// Fire a JS timer by jsId. For one-shot timers, removes the entry after firing.
static void fireJsTimer(Engine *eng, uint32_t jsId)
{
    auto &timers = eng->jsTimers();
    auto it      = timers.find(jsId);
    if (it == timers.end()) {
        return;
    }

    JSContext *ctx = it->second.ctx;
    JSValue cb     = it->second.callback;
    bool interval  = it->second.interval;

    JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(ret)) {
        JSValue exc   = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        sLog().error("ScriptEngine: timer error: {}", s ? s : "(null)");
        if (s) {
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);

    if (!interval) {
        JS_FreeValue(ctx, cb);
        timers.erase(jsId);
    }
}

// setTimeout(fn, ms) -> timerId
static JSValue jsSetTimeout(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_UNDEFINED;
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->loop()) {
        return JS_UNDEFINED;
    }

    int64_t ms = 0;
    if (argc >= 2) {
        JS_ToInt64(ctx, &ms, argv[1]);
    }
    if (ms < 0) {
        ms = 0;
    }

    uint32_t jsId             = eng->nextTimer();
    EventLoop::TimerId loopId = eng->loop()->addTimer(
        static_cast<uint64_t>(ms),
        false,
        [eng, jsId]()
        {
            fireJsTimer(eng, jsId);
        });

    auto &t    = eng->jsTimers()[jsId];
    t.ctx      = ctx;
    t.callback = JS_DupValue(ctx, argv[0]);
    t.loopId   = loopId;
    t.interval = false;
    t.ms       = static_cast<uint64_t>(ms);
    return JS_NewUint32(ctx, jsId);
}

// setInterval(fn, ms) -> timerId
static JSValue jsSetInterval(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[0])) {
        return JS_UNDEFINED;
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->loop()) {
        return JS_UNDEFINED;
    }

    int64_t ms = 0;
    JS_ToInt64(ctx, &ms, argv[1]);
    if (ms < 1) {
        ms = 1;
    }

    uint32_t jsId             = eng->nextTimer();
    EventLoop::TimerId loopId = eng->loop()->addTimer(
        static_cast<uint64_t>(ms),
        true,
        [eng, jsId]()
        {
            fireJsTimer(eng, jsId);
        });

    auto &t    = eng->jsTimers()[jsId];
    t.ctx      = ctx;
    t.callback = JS_DupValue(ctx, argv[0]);
    t.loopId   = loopId;
    t.interval = true;
    t.ms       = static_cast<uint64_t>(ms);
    return JS_NewUint32(ctx, jsId);
}

// clearTimeout(id) / clearInterval(id) — same implementation
static JSValue jsClearTimer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_UNDEFINED;
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->loop()) {
        return JS_UNDEFINED;
    }

    uint32_t jsId;
    JS_ToUint32(ctx, &jsId, argv[0]);

    auto &timers = eng->jsTimers();
    auto it      = timers.find(jsId);
    if (it != timers.end()) {
        eng->loop()->removeTimer(it->second.loopId);
        JS_FreeValue(it->second.ctx, it->second.callback);
        timers.erase(it);
    }

    return JS_UNDEFINED;
}

// ============================================================================
// console.*
// ============================================================================

static std::shared_ptr<spdlog::logger> jsLogger()
{
    auto l = spdlog::get("js");
    return l ? l : spdlog::default_logger();
}

static int sJsIndent = 0;

static std::string jsConsoleMsg(JSContext *ctx, int argc, JSValueConst *argv)
{
    std::string msg;
    if (sJsIndent > 0) {
        msg.append(static_cast<size_t>(sJsIndent * 2), ' ');
    }
    for (int i = 0; i < argc; ++i) {
        if (i > 0) {
            msg += ' ';
        }
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            msg += s;
            JS_FreeCString(ctx, s);
        }
    }
    return msg;
}

static JSValue jsConsoleGroup(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc > 0) {
        jsLogger()->info("{}", jsConsoleMsg(ctx, argc, argv));
    }
    ++sJsIndent;
    return JS_UNDEFINED;
}

static JSValue jsConsoleGroupEnd(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    if (sJsIndent > 0) {
        --sJsIndent;
    }
    return JS_UNDEFINED;
}

static JSValue jsConsoleTrace(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    jsLogger()->trace("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleDebug(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    jsLogger()->debug("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleInfo(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    jsLogger()->info("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleLog(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    jsLogger()->info("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleWarn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    jsLogger()->warn("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleError(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    jsLogger()->error("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

// ============================================================================
// Engine implementation
// ============================================================================

Engine::Engine()
    : layoutTree_(std::make_unique<LayoutTree>())
{
    // One Stack holds each tab's Layout subtree as a direct child; the
    // Stack's activeChild tracks the currently active tab. Created up front
    // and set as the tree's root so `mb.layout.getRoot()` returns something
    // meaningful from the first JS call onward.
    layoutRootStack_ = layoutTree_->createStack();
    layoutTree_->setRoot(layoutRootStack_);

    // Subscribe to node-destroyed events so the per-subtree maps
    // (lastFocusedInTab_, tabIcons_) self-prune. destroyNode fires the
    // callback bottom-up — descendants before their ancestor — so any
    // value-side Uuid (focused-pane leaves) is reported before the
    // wrapper Stack-child whose entry stored it. Both the key (Stack
    // child id) and the value (Terminal id) can match; erasing on key is
    // the primary case but a focused-pane-side match (terminal evicted
    // out from under a still-live subtree) gets cleaned up by the
    // matching-value sweep below.
    layoutTree_->setOnNodeDestroyed([this](Uuid id)
                                    {
                                        lastFocusedInTab_.erase(id);
                                        tabIcons_.erase(id);
                                        for (auto it = lastFocusedInTab_.begin();
                                             it != lastFocusedInTab_.end();) {
                                            if (it->second == id) {
                                                it = lastFocusedInTab_.erase(it);
                                            } else {
                                                ++it;
                                            }
                                        }
                                    });

    rt_ = JS_NewRuntime();
    JS_SetRuntimeOpaque(rt_, this);

    JS_SetModuleLoaderFunc(rt_, moduleNormalize, moduleLoader, this);

    JS_NewClassID(rt_, &jsTerminalClassId);
    JS_NewClass(rt_, jsTerminalClassId, &jsTerminalClassDef);

    JS_NewClassID(rt_, &jsPaneClassId);
    JS_NewClass(rt_, jsPaneClassId, &jsPaneClassDef);

    JS_NewClassID(rt_, &jsPopupClassId);
    JS_NewClass(rt_, jsPopupClassId, &jsPopupClassDef);

    JS_NewClassID(rt_, &jsEmbeddedClassId);
    JS_NewClass(rt_, jsEmbeddedClassId, &jsEmbeddedClassDef);

    JS_NewClassID(rt_, &jsCommandClassId);
    JS_NewClass(rt_, jsCommandClassId, &jsCommandClassDef);

    JS_NewClassID(rt_, &jsOutputCaptureClassId);
    JS_NewClass(rt_, jsOutputCaptureClassId, &jsOutputCaptureClassDef);

    JS_NewClassID(rt_, &jsDecorationBatchClassId);
    JS_NewClass(rt_, jsDecorationBatchClassId, &jsDecorationBatchClassDef);

    JS_NewClassID(rt_, &jsDecorationHandleClassId);
    JS_NewClass(rt_, jsDecorationHandleClassId, &jsDecorationHandleClassDef);

    JS_NewClassID(rt_, &jsAnimationHandleClassId);
    JS_NewClass(rt_, jsAnimationHandleClassId, &jsAnimationHandleClassDef);
}

Engine::~Engine()
{
    // Close any live WS servers + their lws context before runtime teardown.
    wsDestroyEngine(this);
    // Unload each instance so its owned JSValues (timer callbacks, action
    // handlers, etc.) are freed before JS_FreeRuntime's GC-empty assert.
    // Collect ids first; unload() mutates instances_.
    std::vector<InstanceId> ids;
    ids.reserve(instances_.size());
    for (const auto &inst : instances_) {
        ids.push_back(inst.id);
    }
    for (InstanceId id : ids) {
        unload(id);
    }
    // Any remaining entries (dead contexts flagged during iteration) are
    // already freed; just drop the vector slots. Timer callbacks not
    // attached to an instance, plus global action-handler/timer state, get
    // swept here as a belt-and-braces step.
    for (auto &[_, t] : jsTimers_) {
        if (loop_) {
            loop_->removeTimer(t.loopId);
        }
        JS_FreeValue(t.ctx, t.callback);
    }
    jsTimers_.clear();
    // Drop any in-flight decoration animation completion timers — their
    // callbacks capture `this`, so leaving them armed past Engine
    // destruction would fire on freed memory. Release the strong refs
    // we hold to each JS handle so the runtime's final GC sees correct
    // refcounts.
    for (auto &[_, lc] : animLifecycles_) {
        if (lc.timerId != 0 && loop_) {
            loop_->removeTimer(lc.timerId);
        }
        if (!JS_IsUndefined(lc.handleRef) && lc.handleCtx) {
            JS_FreeValue(lc.handleCtx, lc.handleRef);
            lc.handleRef = JS_UNDEFINED;
        }
    }
    animLifecycles_.clear();
    for (auto &[_, h] : actionHandlers_) {
        JS_FreeValue(h.ctx, h.fn);
    }
    actionHandlers_.clear();
    for (auto &inst : instances_) {
        if (inst.ctx) {
            JS_FreeContext(inst.ctx);
        }
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
    }
}

// Engine::terminal / insertTerminal / extractTerminal are defined inline in
// the header so Layout.cpp (which tests link directly without pulling in
// ScriptEngine.cpp's heavy deps on QuickJS / libwebsockets) can resolve the
// symbols. Destruction of terminals_ is still anchored here via the
// out-of-line ~Engine() since ScriptEngine.cpp includes Terminal.h, so
// unique_ptr<Terminal>::~unique_ptr instantiates against the complete type.

void Engine::settleDecorationAnimation(uint64_t handleId, const char *outcome,
                                       bool snapToEnd, uint64_t nowMs)
{
    auto it = animLifecycles_.find(handleId);
    if (it == animLifecycles_.end()) {
        return;
    }
    DecorationAnimLifecycle lc = std::move(it->second);
    animLifecycles_.erase(it);

    if (lc.timerId != 0 && loop_) {
        loop_->removeTimer(lc.timerId);
    }

    // Find the owning emulator. We try the JS handle's ownerRef first; if
    // the JS handle was GCed (handleData == nullptr) we walk every known
    // terminal looking for the animation. With ~few terminals this is
    // cheap.
    JsAnimationHandleData *hd = lc.handleData;
    JSContext *ctx            = hd ? hd->ctx : nullptr;
    TerminalEmulator *emu     = nullptr;
    if (hd) {
        emu = resolveEmulatorFromVal(ctx, hd->ownerRef);
    }
    if (!emu) {
        for (auto &[_, term] : terminals_) {
            if (!term) {
                continue;
            }
            TerminalEmulator *cand = term.get();
            if (cand->animations().count(handleId) != 0) {
                emu = cand;
                break;
            }
            for (auto &p : term->popups()) {
                TerminalEmulator *pe = p.get();
                if (pe && pe->animations().count(handleId) != 0) {
                    emu = pe;
                    break;
                }
            }
            if (emu) {
                break;
            }
        }
    }

    if (emu) {
        if (snapToEnd) {
            emu->finishAnimation(handleId);
        } else {
            emu->cancelAnimation(handleId, /*snapToEnd=*/false, nowMs);
        }
    }

    // Resolve the JS promise if the handle is still alive and observers
    // requested one via .onEnd(). Done while `hd` is still valid — i.e.
    // BEFORE we release the lifecycle's strong ref. JS_FreeValue on the
    // handle below may drop the refcount to zero and run the finalizer
    // synchronously, freeing `hd`; doing the resolve after that would be
    // a use-after-free.
    if (hd) {
        hd->settled = true;
        hd->outcome = outcome;
        if (!JS_IsUndefined(hd->resolveFn)) {
            JSValue arg = JS_NewString(ctx, outcome);
            JSValue r   = JS_Call(ctx, hd->resolveFn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, arg);
            JS_FreeValue(ctx, hd->resolveFn);
            hd->resolveFn = JS_UNDEFINED;
        }
    }

    if (emu) {
        if (auto &cb = callbacks_.requestRedraw) {
            cb();
        }
    }

    // Release the strong ref last — once dropped, the JS handle's refcount
    // may hit zero and the finalizer can run synchronously, freeing `hd`.
    // No code below uses `hd`.
    if (!JS_IsUndefined(lc.handleRef) && lc.handleCtx) {
        JS_FreeValue(lc.handleCtx, lc.handleRef);
        lc.handleRef = JS_UNDEFINED;
    }
}

void Engine::setCallbacks(AppCallbacks cbs)
{
    callbacks_ = std::move(cbs);
}

JSContext *Engine::createContext()
{
    JSContext *ctx = JS_NewContext(rt_);

    // Terminal base prototype — shared across Pane / Popup / EmbeddedTerminal.
    // Methods on the base (`inject`, `cols`, `rows`, `cursor`, `kind`) resolve
    // the backing TerminalEmulator via a class-id-branching resolver, so
    // applets don't need to know or care which concrete kind they're holding.
    JSValue terminalProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, terminalProto, jsTerminalProto, sizeof(jsTerminalProto) / sizeof(jsTerminalProto[0]));
    JS_SetClassProto(ctx, jsTerminalClassId, terminalProto);

    // Expose `Terminal` on the global so `x instanceof Terminal` works.
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "Terminal", JS_DupValue(ctx, terminalProto));
        JS_FreeValue(ctx, global);
    }

    auto makeSubProto = [&](const JSCFunctionListEntry *entries, size_t count)
    {
        JSValue p = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, p, entries, static_cast<int>(count));
        // Chain to Terminal.prototype so instances inherit base methods.
        JS_SetPrototype(ctx, p, terminalProto);
        return p;
    };

    JSValue paneProto = makeSubProto(jsPaneProto, sizeof(jsPaneProto) / sizeof(jsPaneProto[0]));
    JS_SetClassProto(ctx, jsPaneClassId, paneProto);

    JSValue popupProto = makeSubProto(jsPopupProto, sizeof(jsPopupProto) / sizeof(jsPopupProto[0]));
    JS_SetClassProto(ctx, jsPopupClassId, popupProto);

    JSValue embeddedProto = makeSubProto(jsEmbeddedProto, sizeof(jsEmbeddedProto) / sizeof(jsEmbeddedProto[0]));
    JS_SetClassProto(ctx, jsEmbeddedClassId, embeddedProto);

    JSValue commandProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, commandProto, jsCommandProto, jsCommandProtoCount);
    JS_SetClassProto(ctx, jsCommandClassId, commandProto);

    // OutputCapture is a flat handle (no Terminal-base inheritance —
    // it doesn't represent a TerminalEmulator), so we install its
    // proto directly without makeSubProto.
    JSValue captureProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, captureProto, jsOutputCaptureProto, sizeof(jsOutputCaptureProto) / sizeof(jsOutputCaptureProto[0]));
    JS_SetClassProto(ctx, jsOutputCaptureClassId, captureProto);

    // DecorationBatch is also a flat handle.
    JSValue batchProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, batchProto, jsDecorationBatchProto, sizeof(jsDecorationBatchProto) / sizeof(jsDecorationBatchProto[0]));
    JS_SetClassProto(ctx, jsDecorationBatchClassId, batchProto);

    // DecorationHandle / AnimationHandle prototypes.
    JSValue decorationHandleProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, decorationHandleProto, jsDecorationHandleProto, sizeof(jsDecorationHandleProto) / sizeof(jsDecorationHandleProto[0]));
    JS_SetClassProto(ctx, jsDecorationHandleClassId, decorationHandleProto);

    JSValue animationHandleProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, animationHandleProto, jsAnimationHandleProto, sizeof(jsAnimationHandleProto) / sizeof(jsAnimationHandleProto[0]));
    JS_SetClassProto(ctx, jsAnimationHandleClassId, animationHandleProto);

    // Timer globals
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunction(ctx, jsSetTimeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval", JS_NewCFunction(ctx, jsSetInterval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, jsClearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunction(ctx, jsClearTimer, "clearInterval", 1));

    // console object
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "group", JS_NewCFunction(ctx, jsConsoleGroup, "group", 0));
    JS_SetPropertyStr(ctx, console, "groupEnd", JS_NewCFunction(ctx, jsConsoleGroupEnd, "groupEnd", 0));
    JS_SetPropertyStr(ctx, console, "groupCollapsed", JS_NewCFunction(ctx, jsConsoleGroup, "groupCollapsed", 0)); // same as group
    JS_SetPropertyStr(ctx, console, "trace", JS_NewCFunction(ctx, jsConsoleTrace, "trace", 0));
    JS_SetPropertyStr(ctx, console, "debug", JS_NewCFunction(ctx, jsConsoleDebug, "debug", 0));
    JS_SetPropertyStr(ctx, console, "info", JS_NewCFunction(ctx, jsConsoleInfo, "info", 0));
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, jsConsoleLog, "log", 0));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, jsConsoleWarn, "warn", 0));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, jsConsoleError, "error", 0));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);

    return ctx;
}

static JSValue jsMbRegisterTcap(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue jsMbUnregisterTcap(JSContext *, JSValueConst, int, JSValueConst *);

// Internal: parse the live Config snapshot into a fresh JS object. Used by
// the `mb.config` getter and by the patch/add/remove helpers (which read the
// current snapshot, mutate it, and submit the result).
static JSValue jsConfigParseSnapshot(JSContext *ctx)
{
    auto &cb = engineFromCtx(ctx)->callbacks().configJson;
    if (!cb) {
        return JS_NULL;
    }
    std::string json = cb();
    if (json.empty()) {
        return JS_NULL;
    }
    return JS_ParseJSON(ctx, json.data(), json.size(), "<mb.config>");
}

// Internal: serialize a JS Config object via JSON.stringify and submit it
// through the applyConfigJson callback. On parse/validation failure throws
// a JS Error with the diagnostic from glaze (returns JS_EXCEPTION).
static JSValue jsConfigApplyObject(JSContext *ctx, JSValueConst configObj)
{
    auto &cb = engineFromCtx(ctx)->callbacks().applyConfigJson;
    if (!cb) {
        return JS_ThrowTypeError(ctx, "mb.config: not wired");
    }

    JSValue jsonVal = JS_JSONStringify(ctx, configObj, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(jsonVal)) {
        return JS_EXCEPTION;
    }
    size_t len    = 0;
    const char *s = JS_ToCStringLen(ctx, &len, jsonVal);
    JS_FreeValue(ctx, jsonVal);
    if (!s) {
        return JS_EXCEPTION;
    }
    std::string json(s, len);
    JS_FreeCString(ctx, s);

    std::string err = cb(json);
    if (!err.empty()) {
        return JS_ThrowTypeError(ctx, "%s", err.c_str());
    }
    return JS_UNDEFINED;
}

// Recursively deep-merge `src` into `dst`, mutating `dst` in place.
// Object-typed fields recurse; arrays and primitives replace wholesale.
// `null` in `src` clears the field on `dst`. Returns false on JS exception.
static bool jsDeepMergeInto(JSContext *ctx, JSValue dst, JSValueConst src)
{
    JSPropertyEnum *tab = nullptr;
    uint32_t len        = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, src, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; i < len && ok; ++i) {
        JSValue srcVal = JS_GetProperty(ctx, src, tab[i].atom);
        if (JS_IsException(srcVal)) {
            ok = false;
            break;
        }

        // Recurse for plain objects (not arrays, not null).
        if (JS_IsObject(srcVal) && !JS_IsArray(srcVal) && !JS_IsNull(srcVal)) {
            JSValue dstVal = JS_GetProperty(ctx, dst, tab[i].atom);
            if (JS_IsObject(dstVal) && !JS_IsArray(dstVal) && !JS_IsNull(dstVal)) {
                ok = jsDeepMergeInto(ctx, dstVal, srcVal);
                JS_FreeValue(ctx, dstVal);
                JS_FreeValue(ctx, srcVal);
                continue;
            }
            JS_FreeValue(ctx, dstVal);
        }
        // Replace wholesale (arrays, primitives, type mismatch).
        if (JS_SetProperty(ctx, dst, tab[i].atom, srcVal) < 0) {
            ok = false; // srcVal consumed by JS_SetProperty even on failure
        }
    }
    for (uint32_t i = 0; i < len; ++i) {
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    return ok;
}

// mb.config.patch(partial) — deep-merges `partial` into a copy of the live
// Config and applies the result. Object fields recurse; arrays (keybinding,
// mousebinding) and primitives are replaced wholesale. Throws on validation
// failure.
static JSValue jsMbConfigPatch(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ConfigModify);
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsArray(argv[0])) {
        return JS_ThrowTypeError(ctx, "mb.config.patch(partial): expected object");
    }

    JSValue snap = jsConfigParseSnapshot(ctx);
    if (JS_IsNull(snap) || JS_IsException(snap)) {
        return JS_ThrowTypeError(ctx, "mb.config.patch: cannot read current config");
    }

    if (!jsDeepMergeInto(ctx, snap, argv[0])) {
        JS_FreeValue(ctx, snap);
        return JS_EXCEPTION;
    }
    JSValue ret = jsConfigApplyObject(ctx, snap);
    JS_FreeValue(ctx, snap);
    return ret;
}

// Internal: snapshot helper for the array mutators below. Reads the current
// snapshot, returns the array property by name (stealing a fresh reference),
// or creates an empty array if the property is missing/wrong type.
// `outSnap` is set to the snapshot object (caller frees on success).
// Returns JS_EXCEPTION (with thrown error) on read failure.
static JSValue jsConfigGetArrayField(JSContext *ctx, const char *fieldName,
                                     JSValue *outSnap)
{
    JSValue snap = jsConfigParseSnapshot(ctx);
    if (JS_IsNull(snap) || JS_IsException(snap)) {
        return JS_ThrowTypeError(ctx, "mb.config: cannot read current config");
    }
    JSValue arr = JS_GetPropertyStr(ctx, snap, fieldName);
    if (!JS_IsArray(arr)) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        // The mutator path will assign this back via JS_SetPropertyStr,
        // so the snapshot picks it up.
    }
    *outSnap = snap;
    return arr;
}

// Validate that argv[0] is a plain object with a string `action` and (for
// keybindings) a string-array `keys` / (for mousebindings) string `button`,
// `event`. We delegate full schema validation to glaze on the apply path —
// here we just reject obvious shape errors so callers get a clearer message.
static bool jsValidatePlainObject(JSContext *ctx, JSValueConst v, const char *sig)
{
    if (!JS_IsObject(v) || JS_IsArray(v) || JS_IsNull(v)) {
        JS_ThrowTypeError(ctx, "%s: expected object", sig);
        return false;
    }
    return true;
}

// mb.config.addKeybinding({keys: string[], action: string, args?: string[]})
// Appends the entry to config.keybinding[] and applies. Convenience over
// `mb.config.patch({keybinding: [...mb.config.keybinding, b]})`. Does not
// remove duplicates; later entries win in the merge inside applyConfig.
static JSValue jsMbConfigAddKeybinding(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ConfigModify);
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "addKeybinding({keys, action, args?})");
    }
    if (!jsValidatePlainObject(ctx, argv[0], "addKeybinding")) {
        return JS_EXCEPTION;
    }

    JSValue snap;
    JSValue arr = jsConfigGetArrayField(ctx, "keybinding", &snap);
    if (JS_IsException(arr)) {
        return JS_EXCEPTION;
    }

    uint32_t len = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &len, lenv) != 0) {
        JS_FreeValue(ctx, lenv);
        JS_FreeValue(ctx, arr);
        JS_FreeValue(ctx, snap);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);

    // argv[0] is borrowed — dup before stuffing into the array (the array
    // takes ownership of its elements; on snap free they all release).
    JS_SetPropertyUint32(ctx, arr, len, JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, snap, "keybinding", arr); // arr ownership transferred

    JSValue ret = jsConfigApplyObject(ctx, snap);
    JS_FreeValue(ctx, snap);
    return ret;
}

// mb.config.removeKeybinding({keys: string[]}) — removes all entries with a
// matching `keys` array (exact element-wise match, order-sensitive). Returns
// the count of removed entries (0 if none matched).
static JSValue jsMbConfigRemoveKeybinding(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ConfigModify);
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "removeKeybinding({keys})");
    }
    if (!jsValidatePlainObject(ctx, argv[0], "removeKeybinding")) {
        return JS_EXCEPTION;
    }

    JSValue matchKeys = JS_GetPropertyStr(ctx, argv[0], "keys");
    if (!JS_IsArray(matchKeys)) {
        JS_FreeValue(ctx, matchKeys);
        return JS_ThrowTypeError(ctx, "removeKeybinding: `keys` must be a string array");
    }

    JSValue snap;
    JSValue arr = jsConfigGetArrayField(ctx, "keybinding", &snap);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, matchKeys);
        return JS_EXCEPTION;
    }

    auto strArrayEqual = [&](JSValue a, JSValue b) -> bool
    {
        if (!JS_IsArray(a) || !JS_IsArray(b)) {
            return false;
        }
        uint32_t la = 0, lb = 0;
        JSValue lav = JS_GetPropertyStr(ctx, a, "length");
        JSValue lbv = JS_GetPropertyStr(ctx, b, "length");
        JS_ToUint32(ctx, &la, lav);
        JS_ToUint32(ctx, &lb, lbv);
        JS_FreeValue(ctx, lav);
        JS_FreeValue(ctx, lbv);
        if (la != lb) {
            return false;
        }
        for (uint32_t i = 0; i < la; ++i) {
            JSValue av     = JS_GetPropertyUint32(ctx, a, i);
            JSValue bv     = JS_GetPropertyUint32(ctx, b, i);
            const char *as = JS_ToCString(ctx, av);
            const char *bs = JS_ToCString(ctx, bv);
            bool eq        = (as && bs && std::strcmp(as, bs) == 0);
            if (as) {
                JS_FreeCString(ctx, as);
            }
            if (bs) {
                JS_FreeCString(ctx, bs);
            }
            JS_FreeValue(ctx, av);
            JS_FreeValue(ctx, bv);
            if (!eq) {
                return false;
            }
        }
        return true;
    };

    uint32_t arrLen = 0;
    JSValue lenv    = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &arrLen, lenv);
    JS_FreeValue(ctx, lenv);

    JSValue filtered = JS_NewArray(ctx);
    uint32_t outIdx  = 0;
    int removed      = 0;
    for (uint32_t i = 0; i < arrLen; ++i) {
        JSValue entry     = JS_GetPropertyUint32(ctx, arr, i);
        JSValue entryKeys = JS_GetPropertyStr(ctx, entry, "keys");
        bool match        = strArrayEqual(entryKeys, matchKeys);
        JS_FreeValue(ctx, entryKeys);
        if (match) {
            JS_FreeValue(ctx, entry);
            ++removed;
            continue;
        }
        JS_SetPropertyUint32(ctx, filtered, outIdx++, entry); // ownership transferred
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, matchKeys);
    JS_SetPropertyStr(ctx, snap, "keybinding", filtered);

    JSValue applyRet = jsConfigApplyObject(ctx, snap);
    JS_FreeValue(ctx, snap);
    if (JS_IsException(applyRet)) {
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, applyRet);
    return JS_NewInt32(ctx, removed);
}

// mb.config.addMousebinding({button, event, mode?, region?, action, args?})
// Appends to config.mousebinding[] and applies.
static JSValue jsMbConfigAddMousebinding(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ConfigModify);
    if (argc < 1) {
        return JS_ThrowTypeError(ctx,
                                 "addMousebinding({button, event, mode?, region?, action, args?})");
    }
    if (!jsValidatePlainObject(ctx, argv[0], "addMousebinding")) {
        return JS_EXCEPTION;
    }

    JSValue snap;
    JSValue arr = jsConfigGetArrayField(ctx, "mousebinding", &snap);
    if (JS_IsException(arr)) {
        return JS_EXCEPTION;
    }

    uint32_t len = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &len, lenv);
    JS_FreeValue(ctx, lenv);

    JS_SetPropertyUint32(ctx, arr, len, JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, snap, "mousebinding", arr);

    JSValue ret = jsConfigApplyObject(ctx, snap);
    JS_FreeValue(ctx, snap);
    return ret;
}

// mb.config.removeMousebinding({button, event, mode?, region?})
// Removes all entries whose specified fields match (omitted fields are
// wildcards). Returns the count removed.
static JSValue jsMbConfigRemoveMousebinding(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ConfigModify);
    if (argc < 1) {
        return JS_ThrowTypeError(ctx,
                                 "removeMousebinding({button, event, mode?, region?})");
    }
    if (!jsValidatePlainObject(ctx, argv[0], "removeMousebinding")) {
        return JS_EXCEPTION;
    }

    auto getStr = [&](JSValueConst obj, const char *key) -> std::string
    {
        JSValue v = JS_GetPropertyStr(ctx, obj, key);
        std::string out;
        if (JS_IsString(v)) {
            const char *s = JS_ToCString(ctx, v);
            if (s) {
                out = s;
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, v);
        return out;
    };
    std::string mButton = getStr(argv[0], "button");
    std::string mEvent  = getStr(argv[0], "event");
    std::string mMode   = getStr(argv[0], "mode");
    std::string mRegion = getStr(argv[0], "region");

    JSValue snap;
    JSValue arr = jsConfigGetArrayField(ctx, "mousebinding", &snap);
    if (JS_IsException(arr)) {
        return JS_EXCEPTION;
    }

    uint32_t arrLen = 0;
    JSValue lenv    = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &arrLen, lenv);
    JS_FreeValue(ctx, lenv);

    JSValue filtered = JS_NewArray(ctx);
    uint32_t outIdx  = 0;
    int removed      = 0;
    for (uint32_t i = 0; i < arrLen; ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, arr, i);
        bool match    = true;
        if (!mButton.empty() && getStr(entry, "button") != mButton) {
            match = false;
        }
        if (match && !mEvent.empty() && getStr(entry, "event") != mEvent) {
            match = false;
        }
        if (match && !mMode.empty() && getStr(entry, "mode") != mMode) {
            match = false;
        }
        if (match && !mRegion.empty() && getStr(entry, "region") != mRegion) {
            match = false;
        }
        if (match) {
            JS_FreeValue(ctx, entry);
            ++removed;
            continue;
        }
        JS_SetPropertyUint32(ctx, filtered, outIdx++, entry);
    }
    JS_FreeValue(ctx, arr);
    JS_SetPropertyStr(ctx, snap, "mousebinding", filtered);

    JSValue applyRet = jsConfigApplyObject(ctx, snap);
    JS_FreeValue(ctx, snap);
    if (JS_IsException(applyRet)) {
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, applyRet);
    return JS_NewInt32(ctx, removed);
}

// mb.config — JS snapshot of the loaded TOML Config (the same struct
// PlatformDawn applies on hot-reload). Re-fetch after the `configChanged`
// event to pick up live updates. Implemented as a JSON round-trip through
// glz::write_json + JS_ParseJSON so the JS surface tracks the C++ struct
// shape automatically.
//
// Mutation methods attached to the returned object (gated on `config.modify`):
//   `patch(partial)` — deep-merge a partial Config in
//   `addKeybinding({keys, action, args?})` / `removeKeybinding({keys})`
//   `addMousebinding({...})` / `removeMousebinding({...})`
//
// All mutations go through the same applyConfig path as TOML hot-reload;
// changes are ephemeral (not persisted to disk).
static JSValue jsMbConfig(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    JSValue obj = jsConfigParseSnapshot(ctx);
    if (JS_IsNull(obj) || JS_IsException(obj)) {
        return obj;
    }

    JS_SetPropertyStr(ctx, obj, "patch", JS_NewCFunction(ctx, jsMbConfigPatch, "patch", 1));
    JS_SetPropertyStr(ctx, obj, "addKeybinding", JS_NewCFunction(ctx, jsMbConfigAddKeybinding, "addKeybinding", 1));
    JS_SetPropertyStr(ctx, obj, "removeKeybinding", JS_NewCFunction(ctx, jsMbConfigRemoveKeybinding, "removeKeybinding", 1));
    JS_SetPropertyStr(ctx, obj, "addMousebinding", JS_NewCFunction(ctx, jsMbConfigAddMousebinding, "addMousebinding", 1));
    JS_SetPropertyStr(ctx, obj, "removeMousebinding", JS_NewCFunction(ctx, jsMbConfigRemoveMousebinding, "removeMousebinding", 1));
    return obj;
}

// mb.pane(nodeId) -> Pane | null. Construct a Pane object wrapping the
// terminal at `nodeId`. Returns null when the UUID is malformed or doesn't
// refer to a live Terminal in the engine's terminal map.
static JSValue jsMbPane(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_NULL;
    }
    size_t len    = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) {
        return JS_NULL;
    }
    Uuid u = Uuid::fromString(std::string_view(s, len));
    JS_FreeCString(ctx, s);
    if (u.isNil()) {
        return JS_NULL;
    }
    Engine *eng = engineFromCtx(ctx);
    if (!eng->terminal(u)) {
        return JS_NULL;
    }
    return jsPaneNew(ctx, u);
}

// mb.getClipboard(source?) -> Promise<string>.
// source = "clipboard" | "primary" (default "clipboard"). Async because the
// X11 backend has to round-trip a SELECTION_NOTIFY through the event loop;
// resolving directly was a busy-poll source on the main thread that raced
// with middle-click pastes (see the OSC 52 / requestSelection discussion).
static JSValue jsMbGetClipboard(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ClipboardRead);
    Engine *eng = engineFromCtx(ctx);

    JSValue resolveFn = JS_UNDEFINED, rejectFn = JS_UNDEFINED;
    JSValue resolvers[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolvers);
    if (JS_IsException(promise)) {
        return JS_EXCEPTION;
    }
    resolveFn = resolvers[0];
    rejectFn  = resolvers[1];

    if (!eng->callbacks().getClipboard) {
        JSValue empty = JS_NewString(ctx, "");
        JS_Call(ctx, resolveFn, JS_UNDEFINED, 1, &empty);
        JS_FreeValue(ctx, empty);
        JS_FreeValue(ctx, resolveFn);
        JS_FreeValue(ctx, rejectFn);
        return promise;
    }

    std::string source = "clipboard";
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) {
            source = s;
            JS_FreeCString(ctx, s);
        }
    }

    // Hold a reference to the resolve fn for the callback. Reject is unused
    // here (we never reject — empty string represents "no clipboard").
    JS_FreeValue(ctx, rejectFn);
    auto resolveHolder    = std::make_shared<JSValue>(resolveFn);
    JSContext *ctxCapture = ctx;

    eng->callbacks().getClipboard(source,
                                  [ctxCapture, resolveHolder](std::string text)
                                  {
                                      JSValue str = JS_NewStringLen(ctxCapture, text.data(), text.size());
                                      JS_Call(ctxCapture, *resolveHolder, JS_UNDEFINED, 1, &str);
                                      JS_FreeValue(ctxCapture, str);
                                      JS_FreeValue(ctxCapture, *resolveHolder);
                                  });
    return promise;
}

// mb.setClipboard(text, source?).  source = "clipboard" | "primary" (default "clipboard")
static JSValue jsMbSetClipboard(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "setClipboard requires (text, source?)");
    }
    REQUIRE_PERM(ctx, ClipboardWrite);
    Engine *eng = engineFromCtx(ctx);
    if (!eng->callbacks().setClipboard) {
        return JS_UNDEFINED;
    }
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) {
        return JS_EXCEPTION;
    }
    std::string source = "clipboard";
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char *s = JS_ToCString(ctx, argv[1]);
        if (s) {
            source = s;
            JS_FreeCString(ctx, s);
        }
    }
    eng->callbacks().setClipboard(source, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

void Engine::setupGlobals(JSContext *ctx, InstanceId id)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb     = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mb, "invokeAction", JS_NewCFunction(ctx, jsMbInvokeAction, "invokeAction", 1));
    JS_SetPropertyStr(ctx, mb, "addEventListener", JS_NewCFunction(ctx, jsMbAddEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, mb, "removeEventListener", JS_NewCFunction(ctx, jsMbRemoveEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, mb, "removeAllListeners", JS_NewCFunction(ctx, jsMbRemoveAllListeners, "removeAllListeners", 0));
    // Getter properties on mb object
    auto defineGetter = [&](const char *name, JSCFunction *getter)
    {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, mb, atom, JS_NewCFunction(ctx, getter, name, 0), JS_UNDEFINED, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
    };
    defineGetter("activePane", jsMbGetActivePane);
    defineGetter("actions", jsMbGetActions);
    defineGetter("config", jsMbConfig);
    JS_SetPropertyStr(ctx, mb, "pane", JS_NewCFunction(ctx, jsMbPane, "pane", 1));
    JS_SetPropertyStr(ctx, mb, "unloadScript", JS_NewCFunction(ctx, jsMbUnloadScript, "unloadScript", 1));
    JS_SetPropertyStr(ctx, mb, "loadScript", JS_NewCFunction(ctx, jsMbLoadScript, "loadScript", 2));
    JS_SetPropertyStr(ctx, mb, "createSecureToken", JS_NewCFunction(ctx, jsMbCreateSecureToken, "createSecureToken", 1));
    JS_SetPropertyStr(ctx, mb, "createUuid", JS_NewCFunction(ctx, jsMbCreateUuid, "createUuid", 0));
    JS_SetPropertyStr(ctx, mb, "approveScript", JS_NewCFunction(ctx, jsMbApproveScript, "approveScript", 2));
    JS_SetPropertyStr(ctx, mb, "setNamespace", JS_NewCFunction(ctx, jsMbSetNamespace, "setNamespace", 1));
    JS_SetPropertyStr(ctx, mb, "registerAction", JS_NewCFunction(ctx, jsMbRegisterAction, "registerAction", 1));
    JS_SetPropertyStr(ctx, mb, "exit", JS_NewCFunction(ctx, jsMbExit, "exit", 0));
    JS_SetPropertyStr(ctx, mb, "quit", JS_NewCFunction(ctx, jsMbQuit, "quit", 0));
    JS_SetPropertyStr(ctx, mb, "hasPermission", JS_NewCFunction(ctx, jsMbHasPermission, "hasPermission", 1));
    JS_SetPropertyStr(ctx, mb, "registerTcap", JS_NewCFunction(ctx, jsMbRegisterTcap, "registerTcap", 2));
    JS_SetPropertyStr(ctx, mb, "unregisterTcap", JS_NewCFunction(ctx, jsMbUnregisterTcap, "unregisterTcap", 1));
    JS_SetPropertyStr(ctx, mb, "getClipboard", JS_NewCFunction(ctx, jsMbGetClipboard, "getClipboard", 0));
    JS_SetPropertyStr(ctx, mb, "setClipboard", JS_NewCFunction(ctx, jsMbSetClipboard, "setClipboard", 1));
    JS_SetPropertyStr(ctx, mb, "startAnimation", JS_NewCFunction(ctx, jsMbStartAnimation, "startAnimation", 3));
    JS_SetPropertyStr(ctx, mb, "_setMonoForTest", JS_NewCFunction(ctx, jsMbSetMonoForTest, "_setMonoForTest", 1));

    // mb.process — namespace for external process management. v1 has
    // exactly one method (spawn); the namespace exists to give us
    // breathing room for tracked spawn / kill / list / SIGCHLD-watch
    // follow-ups without polluting the top-level mb surface. Created
    // unconditionally; permission gating happens at method-call time
    // (REQUIRE_PERM(ctx, ProcessSpawn) inside jsMbProcessSpawn).
    {
        JSValue process = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, process, "spawn", JS_NewCFunction(ctx, jsMbProcessSpawn, "spawn", 3));
        JS_SetPropertyStr(ctx, mb, "process", process);
    }

    // mb.os — Node-style platform/arch identity for scripts that want to
    // gate behavior on host OS without resorting to env / file probes.
    // Both fields resolve at compile time so scripts get the same values
    // regardless of the runtime environment. Strings match Node's
    // `process.platform` / `os.arch()` conventions (the surface most
    // script authors already know) so cross-engine snippets port without
    // a translation table.
    {
        JSValue os = JS_NewObject(ctx);
        constexpr const char *kPlatform =
#if defined(__APPLE__)
            "darwin";
#elif defined(__linux__)
            "linux";
#elif defined(_WIN32)
            "win32";
#elif defined(__FreeBSD__)
            "freebsd";
#elif defined(__OpenBSD__)
            "openbsd";
#else
            "unknown";
#endif
        constexpr const char *kArch =
#if defined(__aarch64__) || defined(_M_ARM64)
            "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
            "x64";
#elif defined(__i386__) || defined(_M_IX86)
            "ia32";
#elif defined(__arm__) || defined(_M_ARM)
            "arm";
#else
            "unknown";
#endif
        JS_SetPropertyStr(ctx, os, "platform", JS_NewString(ctx, kPlatform));
        JS_SetPropertyStr(ctx, os, "arch", JS_NewString(ctx, kArch));

        char hostbuf[256] = { 0 };
        if (gethostname(hostbuf, sizeof(hostbuf) - 1) == 0) {
            JS_SetPropertyStr(ctx, os, "hostname", JS_NewString(ctx, hostbuf));
        } else {
            JS_SetPropertyStr(ctx, os, "hostname", JS_NewString(ctx, ""));
        }

        JS_SetPropertyStr(ctx, mb, "os", os);
    }

    installLayoutBindings(*this, ctx, mb);

    JS_SetPropertyStr(ctx, global, "mb", mb);
    JS_FreeValue(ctx, global);
}

InstanceId Engine::loadController(const std::string &path)
{
    mb::tsx::TransformError tsErr;
    std::string src = mb::tsx::loadAsJs(path, &tsErr);
    if (src.empty()) {
        if (!tsErr.message.empty()) {
            sLog().error("ScriptEngine: '{}' TypeScript transform failed at byte {}: {}",
                         path,
                         tsErr.offset,
                         tsErr.message);
        } else {
            sLog().error("ScriptEngine: failed to read '{}'", path);
        }
        return 0;
    }
    JSContext *ctx = createContext();
    InstanceId id  = nextId_++;
    setupGlobals(ctx, id);

    instances_.push_back({ id, ctx, path, /*contentHash=*/"", Perm::All, /*builtIn=*/true });
    JS_SetContextOpaque(ctx, reinterpret_cast<void *>(static_cast<uintptr_t>(id)));

    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), path.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc     = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        sLog().error("ScriptEngine: '{}' error: {}", path, str ? str : "(null)");
        if (str) {
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        instances_.pop_back();
        return 0;
    }
    // Module eval may return a promise (top-level await); resolved by executePendingJobs()
    JS_FreeValue(ctx, result);
    sLog().info("ScriptEngine: loaded built-in '{}' (id={})", path, id);
    return id;
}

bool Engine::reevalInstance(InstanceId id, const std::string &path)
{
    Instance *inst = findInstance(id);
    if (!inst || !inst->ctx) {
        sLog().error("ScriptEngine: reevalInstance: id {} not found", id);
        return false;
    }
    mb::tsx::TransformError tsErr;
    std::string src = mb::tsx::loadAsJs(path, &tsErr);
    if (src.empty()) {
        if (!tsErr.message.empty()) {
            sLog().error("ScriptEngine: reevalInstance '{}' TypeScript transform failed at byte {}: {}",
                         path,
                         tsErr.offset,
                         tsErr.message);
        } else {
            sLog().error("ScriptEngine: reevalInstance: failed to read '{}'", path);
        }
        return false;
    }
    JSValue result = JS_Eval(inst->ctx, src.c_str(), src.size(), path.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc     = JS_GetException(inst->ctx);
        const char *str = JS_ToCString(inst->ctx, exc);
        sLog().error("ScriptEngine: reevalInstance '{}' error: {}",
                     path,
                     str ? str : "(null)");
        if (str) {
            JS_FreeCString(inst->ctx, str);
        }
        JS_FreeValue(inst->ctx, exc);
        JS_FreeValue(inst->ctx, result);
        return false;
    }
    JS_FreeValue(inst->ctx, result);
    sLog().info("ScriptEngine: re-eval'd '{}' (id={})", path, id);
    return true;
}

void Engine::unload(InstanceId id)
{
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
        if (it->id != id) {
            continue;
        }

        JSContext *ctx = it->ctx;
        sLog().info("ScriptEngine: unloading '{}' (id={})", it->path, id);

        // 1. Kill all timers belonging to this context
        {
            std::vector<uint32_t> toRemove;
            for (auto &[jsId, t] : jsTimers_) {
                if (t.ctx == ctx) {
                    toRemove.push_back(jsId);
                }
            }
            for (uint32_t jsId : toRemove) {
                auto it = jsTimers_.find(jsId);
                if (it == jsTimers_.end()) {
                    continue;
                }
                if (loop_) {
                    loop_->removeTimer(it->second.loopId);
                }
                JS_FreeValue(it->second.ctx, it->second.callback);
                jsTimers_.erase(it);
            }
        }

        // 2. Destroy owned popups
        for (auto &ref : it->ownedPopups) {
            callbacks_.destroyPopup(ref.pane, ref.popupId);
        }
        // 2b. Destroy owned embedded terminals
        for (auto &ref : it->ownedEmbeddeds) {
            if (callbacks_.destroyEmbedded) {
                callbacks_.destroyEmbedded(ref.pane, ref.lineId);
            }
        }
        // 2c. Stop owned output captures. Direct Terminal access (no
        //     AppCallbacks indirection) — Engine already owns the
        //     terminal map. Terminal::removeOutputCapture closes the
        //     file and fires the onCaptureStopped callback, which the
        //     platform routes to notifyCaptureStopped → JS "stopped"
        //     event. Even though the instance is being torn down, the
        //     event still fires for any other instance subscribed to
        //     the same handle (rare: scripts pass capture handles
        //     between modules; harmless either way).
        for (auto &ref : it->ownedOutputCaptures) {
            if (Terminal *t = terminal(ref.pane)) {
                t->removeOutputCapture(ref.path);
            }
        }

        // 3. Decrement filter counts for this instance's registrations.
        //    Mirror the count change into the per-pane atomic flag so
        //    the parse-worker fast-path observes the state change
        //    without locking.
        for (auto pane : it->paneOutputFilters) {
            auto fc = paneOutputFilterCount_.find(pane);
            if (fc != paneOutputFilterCount_.end() && --fc->second <= 0) {
                paneOutputFilterCount_.erase(fc);
                if (auto fl = paneOutputFilterFlag_.find(pane);
                    fl != paneOutputFilterFlag_.end()) {
                    fl->second->store(false, std::memory_order_release);
                }
            }
        }
        for (auto pane : it->paneInputFilters) {
            auto fc = paneInputFilterCount_.find(pane);
            if (fc != paneInputFilterCount_.end() && --fc->second <= 0) {
                paneInputFilterCount_.erase(fc);
                if (auto fl = paneInputFilterFlag_.find(pane);
                    fl != paneInputFilterFlag_.end()) {
                    fl->second->store(false, std::memory_order_release);
                }
            }
        }
        for (auto pane : it->paneMouseMoveListeners) {
            auto fc = paneMouseMoveCount_.find(pane);
            if (fc != paneMouseMoveCount_.end() && --fc->second <= 0) {
                paneMouseMoveCount_.erase(fc);
            }
        }
        for (auto pane : it->paneRowEvictedListeners) {
            auto fc = paneRowEvictedCount_.find(pane);
            if (fc != paneRowEvictedCount_.end() && --fc->second <= 0) {
                paneRowEvictedCount_.erase(fc);
                if (auto fl = paneRowEvictedFlag_.find(pane);
                    fl != paneRowEvictedFlag_.end()) {
                    fl->second->store(false, std::memory_order_release);
                }
            }
        }

        // 5. Remove registered actions owned by this instance. Owner is
        // tracked in the map value; previously this matched by namespace
        // prefix, but since two instances could share a namespace we now
        // erase only what's actually ours.
        for (auto ait = registeredActions_.begin(); ait != registeredActions_.end();) {
            if (ait->second == id) {
                ait = registeredActions_.erase(ait);
            } else {
                ++ait;
            }
        }

        // 6. Drop any action handlers registered by this instance.
        for (auto ahit = actionHandlers_.begin(); ahit != actionHandlers_.end();) {
            if (ahit->second.id == id) {
                JS_FreeValue(ahit->second.ctx, ahit->second.fn);
                ahit = actionHandlers_.erase(ahit);
            } else {
                ++ahit;
            }
        }

        // 7. Close any WS servers + connections owned by this instance.
        wsUnloadInstance(this, id);

        // 8. Free context and remove (or defer removal if iterating)
        JS_FreeContext(ctx);
        if (iterating()) {
            it->ctx = nullptr; // mark dead; IterGuard sweeps on unwind
        } else {
            instances_.erase(it);
        }
        return;
    }
}

void Engine::setConfigDir(const std::string &dir)
{
    configDir_ = dir;
    allowlist_.load(dir);
}

// Collect {path, sha256} for every .js file in the directory tree of scriptPath,
// sorted for deterministic comparison.
static std::vector<std::pair<std::string, std::string>>
collectDirModules(const std::string &scriptPath)
{
    std::vector<std::pair<std::string, std::string>> result;
    fs::path dir = fs::path(scriptPath).parent_path();
    std::error_code ec;
    for (auto &entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            continue;
        }
        // Both .js and .ts contribute to the allowlist hash bundle: .ts files
        // execute (via whiteout) the same way .js does, so a TS edit must
        // invalidate the cached grant just like a JS edit would. Hash the raw
        // on-disk bytes — what the user audited at approval time — rather
        // than the post-transform output.
        auto ext = entry.path().extension();
        if (ext != ".js" && ext != ".ts") {
            continue;
        }
        std::string p       = entry.path().string();
        std::string content = io::readFile(p);
        if (!content.empty()) {
            result.emplace_back(std::move(p), crypto::sha256Hex(content));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Returns true if the modules stored in the allowlist entry match the current
// state of the script's directory (same set of files, same hashes).
static bool verifyModuleHashes(const Allowlist::AllowEntry &entry)
{
    if (entry.modules.empty()) {
        return true; // no modules recorded — allow (old entry)
    }
    auto current = collectDirModules(entry.path);
    return current == entry.modules;
}

Engine::LoadResult Engine::loadScript(const std::string &path,
                                      uint32_t requestedPerms)
{
    std::string content = io::readFile(path);
    if (content.empty()) {
        sLog().error("ScriptEngine: failed to read '{}'", path);
        return { LoadResult::Status::Error, 0, "failed to read '" + path + "'" };
    }

    // Built-in elevation: caller asked for the BuiltIn marker bit.
    // Skip the allowlist and SHA-256 check entirely — the trust
    // comes from the *caller* being a built-in (verified at the JS
    // layer in jsMbLoadScript before we get here; user scripts that
    // request the bit are terminated before reaching this method).
    // Force permissions to Perm::All since built-ins are unrestricted
    // by definition; the BuiltIn marker bit itself is consumed by
    // loadScriptInternal and not stored on the Instance.
    if (requestedPerms & Perm::BuiltIn) {
        std::string err;
        InstanceId id = loadScriptInternal(path, content, Perm::All | Perm::BuiltIn, &err);
        if (id == 0) {
            return { LoadResult::Status::Error, 0, err.empty() ? std::string("script evaluation failed") : std::move(err) };
        }
        return { LoadResult::Status::Loaded, id, {} };
    }

    std::string hash = crypto::sha256Hex(content);

    if (allowlist_.isDenied(path, hash)) {
        sLog().info("ScriptEngine: script '{}' is permanently denied", path);
        return { LoadResult::Status::Denied, 0, {} };
    }

    const auto *entry = allowlist_.check(path, hash);
    if (entry) {
        if ((requestedPerms & ~entry->permissions) == 0) {
            if (verifyModuleHashes(*entry)) {
                std::string err;
                InstanceId id = loadScriptInternal(path, content, requestedPerms, &err);
                if (id == 0) {
                    return { LoadResult::Status::Error, 0, err.empty() ? std::string("script evaluation failed") : std::move(err) };
                }
                return { LoadResult::Status::Loaded, id, {} };
            }
            sLog().info("ScriptEngine: module files changed for '{}', re-prompting", path);
        }
        // Requesting new perms beyond what was granted, or modules changed — re-prompt
    }

    // Store pending script and notify JS to show permission prompt
    std::string pendingKey      = path; // keyed by path
    pendingScripts_[pendingKey] = { path, content, hash, requestedPerms, "", Uuid {} };

    // Fire scriptPermissionRequired event on mb
    notifyPermissionRequired(path, permissionsToString(requestedPerms), hash);
    return { LoadResult::Status::Pending, 0, {} };
}

InstanceId Engine::loadScriptInternal(const std::string &path, const std::string &content,
                                      uint32_t permissions, std::string *errOut)
{
    std::string hash = crypto::sha256Hex(content);

    // Built-in elevation: the marker bit lives in `permissions` itself.
    // Strip it from the value we store on the Instance (it's not a
    // permission, it's a request flag) but use its presence to set
    // the builtIn flag on the instance.
    const bool asBuiltIn       = (permissions & Perm::BuiltIn) != 0;
    const uint32_t storedPerms = permissions & ~Perm::BuiltIn;

    // Idempotent reload: if an existing instance has identical path, content hash,
    // permissions, AND built-in status, return its id without unloading/reloading.
    // Avoids pointless churn (owned resources, registered handlers, WS servers,
    // etc.) when the same script is loaded repeatedly — e.g. applet-loader handling
    // repeated OSC 58237 triggers from multiple shells.
    //
    // Built-in matching: a script previously loaded as built-in and now
    // re-requested as user (or vice versa) is NOT idempotent — they're
    // different trust levels and need a full reload through the unload
    // path below.
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        if (inst.builtIn == asBuiltIn && inst.path == path && inst.contentHash == hash && inst.permissions == storedPerms) {
            sLog().info("ScriptEngine: identical reload of '{}' (id={}), no-op", path, inst.id);
            return inst.id;
        }
    }

    // Unload any existing instance with the same path (content or perms differ).
    // We unload regardless of the previous instance's built-in status — a
    // re-load with different trust intent should replace, not coexist.
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        if (inst.path == path) {
            sLog().info("ScriptEngine: replacing existing instance of '{}'", path);
            unload(inst.id);
            break;
        }
    }

    JSContext *ctx = createContext();
    InstanceId id  = nextId_++;
    setupGlobals(ctx, id);

    instances_.push_back({ id, ctx, path, hash, storedPerms, asBuiltIn });
    JS_SetContextOpaque(ctx, reinterpret_cast<void *>(static_cast<uintptr_t>(id)));

    // For .ts inputs, hand QuickJS the type-stripped bytes; identity hash and
    // allowlist comparison stay on the on-disk source so user-visible auditing
    // tracks what the user actually edited rather than a derived artifact.
    mb::tsx::TransformError tsErr;
    std::string evalSrc = mb::tsx::toJs(path, content, &tsErr);
    if (evalSrc.empty()) {
        sLog().error("ScriptEngine: '{}' TypeScript transform failed at byte {}: {}",
                     path,
                     tsErr.offset,
                     tsErr.message.empty() ? "(no detail)" : tsErr.message);
        if (errOut) {
            *errOut = !tsErr.message.empty()
                ? "TypeScript transform failed at byte " + std::to_string(tsErr.offset) + ": " + tsErr.message
                : std::string("TypeScript transform failed");
        }
        JS_FreeContext(ctx);
        instances_.pop_back();
        return 0;
    }
    JSValue result = JS_Eval(ctx, evalSrc.c_str(), evalSrc.size(), path.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc         = JS_GetException(ctx);
        const char *str     = JS_ToCString(ctx, exc);
        std::string evalMsg = str ? str : "(null)";
        sLog().error("ScriptEngine: '{}' error: {}", path, evalMsg);
        if (errOut) {
            *errOut = "script evaluation failed: " + evalMsg;
        }
        if (str) {
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        instances_.pop_back();
        return 0;
    }
    JS_FreeValue(ctx, result);
    sLog().info("ScriptEngine: loaded {} '{}' (id={}, perms={})",
                asBuiltIn ? "built-in" : "script",
                path,
                id,
                permissionsToString(storedPerms));
    return id;
}

Engine::LoadResult Engine::approveScript(const std::string &path, char response)
{
    auto it = pendingScripts_.find(path);
    if (it == pendingScripts_.end()) {
        sLog().warn("ScriptEngine: no pending script for '{}'", path);
        return { LoadResult::Status::Error, 0, "no pending script for '" + path + "'" };
    }

    PendingScript pending = std::move(it->second);
    pendingScripts_.erase(it);

    auto tryLoad = [&]() -> LoadResult
    {
        std::string err;
        InstanceId id = loadScriptInternal(pending.path, pending.content, pending.requestedPerms, &err);
        if (id == 0) {
            return { LoadResult::Status::Error, 0, err.empty() ? std::string("script evaluation failed") : std::move(err) };
        }
        return { LoadResult::Status::Loaded, id, {} };
    };

    switch (response) {
        case 'y':
        case 'Y':
            return tryLoad();
        case 'a':
        case 'A': {
            auto modules = collectDirModules(pending.path);
            allowlist_.allow(pending.path, pending.hash, pending.requestedPerms, modules);
            allowlist_.save();
            return tryLoad();
        }
        case 'd':
        case 'D':
            allowlist_.deny(pending.path, pending.hash);
            allowlist_.save();
            sLog().info("ScriptEngine: permanently denied '{}'", pending.path);
            return { LoadResult::Status::Denied, 0, {} };
        case 'n':
        case 'N':
        default:
            sLog().info("ScriptEngine: denied '{}' (one-time)", pending.path);
            return { LoadResult::Status::Denied, 0, {} };
    }
}

// ============================================================================
// Custom XTGETTCAP

std::optional<std::string> Engine::lookupCustomTcap(const std::string &name) const
{
    // Called from any thread (parser workers most often). Shared lock for reads.
    std::shared_lock<std::shared_mutex> lock(customTcapsMutex_);
    auto it = customTcaps_.find(name);
    if (it != customTcaps_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void Engine::registerTcap(const std::string &name, const std::string &value)
{
    std::unique_lock<std::shared_mutex> lock(customTcapsMutex_);
    customTcaps_[name] = value;
}

void Engine::unregisterTcap(const std::string &name)
{
    std::unique_lock<std::shared_mutex> lock(customTcapsMutex_);
    customTcaps_.erase(name);
}

static JSValue jsMbRegisterTcap(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ActionsInvoke);
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "registerTcap requires (name, value)");
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    std::string value;
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char *v = JS_ToCString(ctx, argv[1]);
        if (v) {
            value = v;
            JS_FreeCString(ctx, v);
        }
    }
    engineFromCtx(ctx)->registerTcap(name, value);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue jsMbUnregisterTcap(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, ActionsInvoke);
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "unregisterTcap requires (name)");
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    engineFromCtx(ctx)->unregisterTcap(name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue jsMbActionsRegister(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, LayoutModify);
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "mb.actions.register(name, fn)");
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    auto *inst = instanceFromCtx(ctx);
    if (!inst) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "no instance");
    }
    engineFromCtx(ctx)->registerActionHandler(inst->id, name, argv[1]);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue jsMbActionsUnregister(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    REQUIRE_PERM(ctx, LayoutModify);
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "mb.actions.unregister(name)");
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    engineFromCtx(ctx)->unregisterActionHandler(name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

// ============================================================================
// Synchronous filters
// ============================================================================

bool Engine::hasPaneOutputFilters(PaneId pane) const
{
    auto it = paneOutputFilterCount_.find(pane);
    return it != paneOutputFilterCount_.end() && it->second > 0;
}

bool Engine::hasPaneInputFilters(PaneId pane) const
{
    auto it = paneInputFilterCount_.find(pane);
    return it != paneInputFilterCount_.end() && it->second > 0;
}

bool Engine::hasPaneMouseMoveListeners(PaneId pane) const
{
    auto it = paneMouseMoveCount_.find(pane);
    return it != paneMouseMoveCount_.end() && it->second > 0;
}

bool Engine::filterPaneOutput(PaneId pane, std::string &data)
{
    return runPaneFilters(pane, "__output_filters", data);
}

bool Engine::filterPaneInput(PaneId pane, std::string &data)
{
    return runPaneFilters(pane, "__input_filters", data);
}

// Run filters on all Pane JS objects with matching id across controller contexts.
// Pane objects are found via a __pane_registry global keyed by id.
bool Engine::runPaneFilters(PaneId pane, const char *filterProp, std::string &data)
{
    IterGuard guard(this);
    bool modified = false;
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }

        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        JS_FreeValue(inst.ctx, registry);
        if (JS_IsUndefined(paneObj)) {
            continue;
        }

        JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, filterProp);
        if (!JS_IsUndefined(arr)) {
            JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
            int32_t len    = 0;
            JS_ToInt32(inst.ctx, &len, lenVal);
            JS_FreeValue(inst.ctx, lenVal);

            for (int32_t i = 0; i < len; ++i) {
                JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                if (JS_IsFunction(inst.ctx, fn)) {
                    JSValue arg = JS_NewStringLen(inst.ctx, data.c_str(), data.size());
                    JSValue ret = JS_Call(inst.ctx, fn, paneObj, 1, &arg);
                    JS_FreeValue(inst.ctx, arg);

                    if (JS_IsException(ret)) {
                        JSValue exc   = JS_GetException(inst.ctx);
                        const char *s = JS_ToCString(inst.ctx, exc);
                        sLog().error("ScriptEngine: filter error: {}", s ? s : "(null)");
                        if (s) {
                            JS_FreeCString(inst.ctx, s);
                        }
                        JS_FreeValue(inst.ctx, exc);
                    } else if (JS_IsString(ret)) {
                        size_t rlen;
                        const char *rstr = JS_ToCStringLen(inst.ctx, &rlen, ret);
                        if (rstr) {
                            data.assign(rstr, rlen);
                            modified = true;
                            JS_FreeCString(inst.ctx, rstr);
                        }
                    } else if (JS_IsNull(ret)) {
                        data.clear();
                        modified = true;
                    }
                    JS_FreeValue(inst.ctx, ret);
                }
                JS_FreeValue(inst.ctx, fn);
            }
        }
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, paneObj);
    }
    return modified;
}

// ============================================================================
// Async notifications
// ============================================================================

void Engine::notifyAction(const std::string &actionName,
                          const std::vector<std::string> &args)
{
    IterGuard guard(this);
    std::string prop = std::string("__evt_action_") + actionName;
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, prop.c_str());

        // Build per-instance string args. Each JSValue belongs to inst.ctx
        // and is freed after enqueue — enqueueListeners dups them into the
        // microtask payload so the originals are safe to release here.
        std::vector<JSValue> argv;
        argv.reserve(args.size());
        for (const auto &s : args) {
            argv.push_back(JS_NewString(inst.ctx, s.c_str()));
        }
        enqueueListeners(inst.ctx,
                         arr,
                         static_cast<int>(argv.size()),
                         argv.empty() ? nullptr : argv.data());
        for (auto v : argv) {
            JS_FreeValue(inst.ctx, v);
        }

        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyPermissionRequired(const std::string &path,
                                      const std::string &permissions,
                                      const std::string &hash)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        if (!inst.builtIn) {
            continue; // only built-in scripts see permission requests
        }

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, "__evt_scriptPermissionRequired");
        if (!JS_IsUndefined(arr)) {
            JSValue args[3] = {
                JS_NewString(inst.ctx, path.c_str()),
                JS_NewString(inst.ctx, permissions.c_str()),
                JS_NewString(inst.ctx, hash.c_str())
            };
            enqueueListeners(inst.ctx, arr, 3, args);
            JS_FreeValue(inst.ctx, args[0]);
            JS_FreeValue(inst.ctx, args[1]);
            JS_FreeValue(inst.ctx, args[2]);
        }
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyPaneCreated(TabId tab, PaneId pane)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }

        JSValue global  = JS_GetGlobalObject(inst.ctx);
        JSValue mb      = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr     = JS_GetPropertyStr(inst.ctx, mb, "__evt_paneCreated");
        JSValue paneObj = jsPaneNew(inst.ctx, pane);
        enqueueListeners(inst.ctx, arr, 1, &paneObj);
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyConfigChanged()
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, "__evt_configChanged");
        enqueueListeners(inst.ctx, arr, 0, nullptr);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyPaneDestroyed(PaneId pane, Uuid nodeId)
{
    // Fan out to JS listeners before cleanup — the handle is gone by the
    // time the listener fires, so the payload is (id, nodeId) scalars, not
    // a Pane object (mirrors the convention in TODO.md:165).
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global  = JS_GetGlobalObject(inst.ctx);
        JSValue mb      = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr     = JS_GetPropertyStr(inst.ctx, mb, "__evt_paneDestroyed");
        std::string ps  = pane.toString();
        JSValue args[2] = {
            JS_NewStringLen(inst.ctx, ps.data(), ps.size()),
            nodeId.isNil()
                ? JS_NULL
                : JS_NewStringLen(inst.ctx, nodeId.toString().c_str(), 36),
        };
        enqueueListeners(inst.ctx, arr, 2, args);
        JS_FreeValue(inst.ctx, args[0]);
        JS_FreeValue(inst.ctx, args[1]);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }

    cleanupPane(pane);
}

void Engine::notifyTabCreated(TabId tab, Uuid parentStack)
{
    IterGuard guard(this);
    std::string idStr     = tab.toString();
    std::string parentStr = parentStack.toString();
    const bool topLevel   = (parentStack == layoutRootStack_);
    const char *levelStr  = topLevel ? "top" : "sub";
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global  = JS_GetGlobalObject(inst.ctx);
        JSValue mb      = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr     = JS_GetPropertyStr(inst.ctx, mb, "__evt_tabCreated");
        JSValue payload = JS_NewObject(inst.ctx);
        JS_SetPropertyStr(inst.ctx, payload, "id", JS_NewStringLen(inst.ctx, idStr.data(), idStr.size()));
        JS_SetPropertyStr(inst.ctx, payload, "parentStackId", JS_NewStringLen(inst.ctx, parentStr.data(), parentStr.size()));
        JS_SetPropertyStr(inst.ctx, payload, "level", JS_NewString(inst.ctx, levelStr));
        enqueueListeners(inst.ctx, arr, 1, &payload);
        JS_FreeValue(inst.ctx, payload);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

bool Engine::hasQuitListeners() const
{
    // Const-iterate without IterGuard (no listener fan-out, no mutation).
    for (const auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, "__evt_quit-requested");
        bool any       = false;
        if (JS_IsArray(arr)) {
            JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
            int32_t arrLen = 0;
            JS_ToInt32(inst.ctx, &arrLen, lenVal);
            JS_FreeValue(inst.ctx, lenVal);
            any = arrLen > 0;
        }
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
        if (any) {
            return true;
        }
    }
    return false;
}

void Engine::fireQuitRequested()
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, "__evt_quit-requested");
        enqueueListeners(inst.ctx, arr, 0, nullptr);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyTabDestroyed(TabId tab, Uuid parentStack)
{
    IterGuard guard(this);
    std::string idStr     = tab.toString();
    std::string parentStr = parentStack.toString();
    const bool topLevel   = (parentStack == layoutRootStack_);
    const char *levelStr  = topLevel ? "top" : "sub";
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global  = JS_GetGlobalObject(inst.ctx);
        JSValue mb      = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr     = JS_GetPropertyStr(inst.ctx, mb, "__evt_tabDestroyed");
        JSValue payload = JS_NewObject(inst.ctx);
        JS_SetPropertyStr(inst.ctx, payload, "id", JS_NewStringLen(inst.ctx, idStr.data(), idStr.size()));
        JS_SetPropertyStr(inst.ctx, payload, "parentStackId", JS_NewStringLen(inst.ctx, parentStr.data(), parentStr.size()));
        JS_SetPropertyStr(inst.ctx, payload, "level", JS_NewString(inst.ctx, levelStr));
        enqueueListeners(inst.ctx, arr, 1, &payload);
        JS_FreeValue(inst.ctx, payload);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }

    // cleanupTab clears per-tab listener maps. Only meaningful for
    // top-level (those are the ones JS scripts care about for tab-level
    // bookkeeping). Sub-tab containers don't have analogous per-id state.
    if (topLevel) {
        cleanupTab(tab);
    }
}

void Engine::notifyTerminalExited(PaneId pane, Uuid nodeId)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global  = JS_GetGlobalObject(inst.ctx);
        JSValue mb      = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr     = JS_GetPropertyStr(inst.ctx, mb, "__evt_terminalExited");
        JSValue payload = JS_NewObject(inst.ctx);
        std::string ps  = pane.toString();
        JS_SetPropertyStr(inst.ctx, payload, "paneId", JS_NewStringLen(inst.ctx, ps.data(), ps.size()));
        JS_SetPropertyStr(inst.ctx, payload, "paneNodeId", nodeId.isNil() ? JS_NULL : JS_NewStringLen(inst.ctx, nodeId.toString().c_str(), 36));
        enqueueListeners(inst.ctx, arr, 1, &payload);
        JS_FreeValue(inst.ctx, payload);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyPaneResized(PaneId pane, int cols, int rows)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }

        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr    = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_resized");
            JSValue args[] = { JS_NewInt32(inst.ctx, cols), JS_NewInt32(inst.ctx, rows) };
            enqueueListeners(inst.ctx, arr, 2, args);
            JS_FreeValue(inst.ctx, args[0]);
            JS_FreeValue(inst.ctx, args[1]);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyOSC(PaneId pane, int oscNum, const std::string &payload)
{
    IterGuard guard(this);
    std::string prop = "__evt_osc:" + std::to_string(oscNum);

    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }

        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, prop.c_str());
            if (!JS_IsUndefined(arr)) {
                JSValue arg = JS_NewStringLen(inst.ctx, payload.c_str(), payload.size());
                enqueueListeners(inst.ctx, arr, 1, &arg);
                JS_FreeValue(inst.ctx, arg);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, registry);

        // Also check mb-level listeners — pass pane object directly
        JSValue global2 = JS_GetGlobalObject(inst.ctx);
        JSValue mb      = JS_GetPropertyStr(inst.ctx, global2, "mb");
        JSValue mbArr   = JS_GetPropertyStr(inst.ctx, mb, prop.c_str());
        if (!JS_IsUndefined(mbArr) && !JS_IsUndefined(paneObj)) {
            JSValue args[] = {
                JS_DupValue(inst.ctx, paneObj),
                JS_NewStringLen(inst.ctx, payload.c_str(), payload.size())
            };
            enqueueListeners(inst.ctx, mbArr, 2, args);
            JS_FreeValue(inst.ctx, args[0]);
            JS_FreeValue(inst.ctx, args[1]);
        }
        JS_FreeValue(inst.ctx, mbArr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global2);
        JS_FreeValue(inst.ctx, paneObj);
    }
}

void Engine::notifyForegroundProcessChanged(PaneId pane, const std::string &processName)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_foregroundProcessChanged");
            JSValue arg = JS_NewString(inst.ctx, processName.c_str());
            enqueueListeners(inst.ctx, arr, 1, &arg);
            JS_FreeValue(inst.ctx, arg);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyPaneFocusChanged(PaneId pane, bool focused)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_focusChanged");
            JSValue arg = JS_NewBool(inst.ctx, focused);
            enqueueListeners(inst.ctx, arr, 1, &arg);
            JS_FreeValue(inst.ctx, arg);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyFocusedPopupChanged(PaneId pane, const std::string &popupId)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_focusedPopupChanged");
            JSValue arg = popupId.empty() ? JS_NULL : JS_NewString(inst.ctx, popupId.c_str());
            enqueueListeners(inst.ctx, arr, 1, &arg);
            JS_FreeValue(inst.ctx, arg);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyPaneMouseMove(PaneId pane, int cellX, int cellY, int pixelX, int pixelY)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_mousemove");
            if (!JS_IsUndefined(arr)) {
                JSValue ev = JS_NewObject(inst.ctx);
                JS_SetPropertyStr(inst.ctx, ev, "cellX", JS_NewInt32(inst.ctx, cellX));
                JS_SetPropertyStr(inst.ctx, ev, "cellY", JS_NewInt32(inst.ctx, cellY));
                JS_SetPropertyStr(inst.ctx, ev, "pixelX", JS_NewInt32(inst.ctx, pixelX));
                JS_SetPropertyStr(inst.ctx, ev, "pixelY", JS_NewInt32(inst.ctx, pixelY));
                enqueueListeners(inst.ctx, arr, 1, &ev);
                JS_FreeValue(inst.ctx, ev);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

// ----- Command class getter body + proto table -----
// (class id / finalizer / class def are declared near the top of the file
// so the Engine ctor can reference them.)

static JSValue jsCmdMakePos(JSContext *ctx, uint64_t rowId, int absRow, int col)
{
    JSValue pos = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pos, "rowId", JS_NewInt64(ctx, static_cast<int64_t>(rowId)));
    JS_SetPropertyStr(ctx, pos, "absRow", JS_NewInt32(ctx, absRow));
    JS_SetPropertyStr(ctx, pos, "col", JS_NewInt32(ctx, col));
    return pos;
}

// Magic numbers map to property names in jsCommandProto below.
static JSValue jsCmdGet(JSContext *ctx, JSValueConst this_val, int magic)
{
    auto *d = static_cast<CmdObjData *>(JS_GetOpaque2(ctx, this_val, jsCommandClassId));
    if (!d) {
        return JS_EXCEPTION;
    }
    switch (magic) {
        case 0: return JS_NewInt64(ctx, static_cast<int64_t>(d->info.id));
        case 1: return JS_NewStringLen(ctx, d->info.cwd.data(), d->info.cwd.size());
        case 2: return d->info.exitCode.has_value()
            ? JS_NewInt32(ctx, *d->info.exitCode)
            : JS_NULL;
        case 3: return JS_NewInt64(ctx, static_cast<int64_t>(d->info.startMs));
        case 4: return JS_NewInt64(ctx, static_cast<int64_t>(d->info.endMs));
        case 5: { // command text — lazy
            if (JS_IsUninitialized(d->cachedCommand)) {
                Engine *eng = engineFromCtx(ctx);
                if (eng && eng->callbacks().paneGetText &&
                    d->info.commandStartCol >= 0 && d->info.outputStartLineId) {
                    std::string s = eng->callbacks().paneGetText(
                        d->paneId,
                        d->info.commandStartLineId,
                        d->info.commandStartCol,
                        d->info.outputStartLineId,
                        d->info.outputStartCol);
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) {
                        s.pop_back();
                    }
                    d->cachedCommand = JS_NewStringLen(ctx, s.data(), s.size());
                } else {
                    d->cachedCommand = JS_NewString(ctx, "");
                }
            }
            return JS_DupValue(ctx, d->cachedCommand);
        }
        case 6: { // output text — lazy
            if (JS_IsUninitialized(d->cachedOutput)) {
                Engine *eng = engineFromCtx(ctx);
                if (eng && eng->callbacks().paneGetText &&
                    d->info.outputStartCol >= 0 && d->info.outputEndLineId) {
                    std::string s = eng->callbacks().paneGetText(
                        d->paneId,
                        d->info.outputStartLineId,
                        d->info.outputStartCol,
                        d->info.outputEndLineId,
                        d->info.outputEndCol);
                    d->cachedOutput = JS_NewStringLen(ctx, s.data(), s.size());
                } else {
                    d->cachedOutput = JS_NewString(ctx, "");
                }
            }
            return JS_DupValue(ctx, d->cachedOutput);
        }
        // rowId is the stable logical-line identifier (reflow-invariant, shared
        // across all physical rows of a soft-wrapped line). absRow is the
        // resolved abs at object-build time (may shift on the next reflow).
        case 7: return jsCmdMakePos(ctx, d->info.promptStartLineId, d->info.promptStartAbsRow, d->info.promptStartCol);
        case 8: return jsCmdMakePos(ctx, d->info.commandStartLineId, d->info.commandStartAbsRow, d->info.commandStartCol);
        case 9: return jsCmdMakePos(ctx, d->info.outputStartLineId, d->info.outputStartAbsRow, d->info.outputStartCol);
        case 10: return jsCmdMakePos(ctx, d->info.outputEndLineId, d->info.outputEndAbsRow, d->info.outputEndCol);
        default: return JS_UNDEFINED;
    }
}

const JSCFunctionListEntry jsCommandProto[] = {
    JS_CGETSET_MAGIC_DEF("id", jsCmdGet, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("cwd", jsCmdGet, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("exitCode", jsCmdGet, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("startMs", jsCmdGet, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("endMs", jsCmdGet, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("command", jsCmdGet, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("output", jsCmdGet, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("promptStart", jsCmdGet, nullptr, 7),
    JS_CGETSET_MAGIC_DEF("commandStart", jsCmdGet, nullptr, 8),
    JS_CGETSET_MAGIC_DEF("outputStart", jsCmdGet, nullptr, 9),
    JS_CGETSET_MAGIC_DEF("outputEnd", jsCmdGet, nullptr, 10),
};
const size_t jsCommandProtoCount = sizeof(jsCommandProto) / sizeof(jsCommandProto[0]);

static JSValue buildCommandObject(JSContext *ctx, const Script::CommandInfo &p, PaneId paneId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsCommandClassId);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new CmdObjData { p, paneId, JS_UNINITIALIZED, JS_UNINITIALIZED });
    return obj;
}

void Engine::notifyCommandComplete(PaneId pane, const CommandInfo &rec)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_commandComplete");
            JSValue arg = buildCommandObject(inst.ctx, rec, pane);
            enqueueListeners(inst.ctx, arr, 1, &arg);
            JS_FreeValue(inst.ctx, arg);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyRowEvicted(PaneId pane, uint64_t lineId)
{
    // Posted from PlatformDawn after the parse worker yields mMutex, so
    // we're safely on the main thread when entering QuickJS.
    //
    // Permission gating happens at SUBSCRIPTION time (jsPaneAddEventListener
    // requires PaneRead for "rowEvicted"); here we just dispatch to
    // whatever listeners managed to register. A script that lost its
    // grant after subscribing would still receive events until it
    // explicitly removes the listener — same model as commandComplete /
    // selection events.
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_rowEvicted");
            // Payload: { rowId } object, matching the shape of other
            // row-id-bearing payloads (cursor, selection, links).
            JSValue ev  = JS_NewObject(inst.ctx);
            JS_SetPropertyStr(inst.ctx, ev, "rowId", JS_NewInt64(inst.ctx, static_cast<int64_t>(lineId)));
            enqueueListeners(inst.ctx, arr, 1, &ev);
            JS_FreeValue(inst.ctx, ev);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyCaptureStopped(PaneId pane,
                                  const std::string &path,
                                  const std::string &reason,
                                  const std::string &errorMessage)
{
    // Posted from PlatformDawn after Terminal::onCaptureStopped fires
    // off-thread (from the parse worker on auto-stop, or main on
    // explicit stop). PlatformDawn always posts to main, so we're
    // safely on the main thread when entering QuickJS.
    //
    // Lookup is by (paneId:path) — the same key used at registration
    // in jsPaneCaptureOutputToFile. The MbOutputCapture handle has a
    // private listener array (`__evt_stopped`); we mark the handle
    // inactive (active=false) before firing so listeners that re-
    // enter via handle.active see the right state.
    std::string regKey = pane.toString() + ":" + path;

    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__capture_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue handle = JS_GetPropertyStr(inst.ctx, registry, regKey.c_str());
        if (!JS_IsUndefined(handle)) {
            // Mark dead first so listeners that read .active inside
            // their callback see the post-stop state.
            if (auto *d = jsOutputCaptureGet(inst.ctx, handle)) {
                d->active = false;
            }
            JSValue arr = JS_GetPropertyStr(inst.ctx, handle, "__evt_stopped");
            JSValue ev  = JS_NewObject(inst.ctx);
            JS_SetPropertyStr(inst.ctx, ev, "reason", JS_NewStringLen(inst.ctx, reason.data(), reason.size()));
            JS_SetPropertyStr(inst.ctx, ev, "error", JS_NewStringLen(inst.ctx, errorMessage.data(), errorMessage.size()));
            enqueueListeners(inst.ctx, arr, 1, &ev);
            JS_FreeValue(inst.ctx, ev);
            JS_FreeValue(inst.ctx, arr);
            // Drop the registry entry so a subsequent capture with
            // the same (paneId, path) registers a fresh handle.
            JS_SetPropertyStr(inst.ctx, registry, regKey.c_str(), JS_UNDEFINED);
        }
        JS_FreeValue(inst.ctx, handle);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyCommandSelectionChanged(PaneId pane, std::optional<uint64_t> commandId)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_commandSelectionChanged");
            JSValue arg = commandId
                ? JS_NewInt64(inst.ctx, static_cast<int64_t>(*commandId))
                : JS_NULL;
            enqueueListeners(inst.ctx, arr, 1, &arg);
            JS_FreeValue(inst.ctx, arg);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyAltScreenChanged(PaneId pane, bool usingAltScreen)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_altScreenChanged");
            JSValue arg = JS_NewBool(inst.ctx, usingAltScreen ? 1 : 0);
            enqueueListeners(inst.ctx, arr, 1, &arg);
            JS_FreeValue(inst.ctx, arg);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverInput(const char *registryName, uint32_t key,
                          const char *data, size_t len)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, registryName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue obj = JS_GetPropertyUint32(inst.ctx, registry, key);
        if (JS_IsUndefined(obj)) {
            JS_FreeValue(inst.ctx, obj);
            JS_FreeValue(inst.ctx, registry);
            continue;
        }

        JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__input_filters");
        if (JS_IsUndefined(arr)) {
            JS_FreeValue(inst.ctx, arr);
            JS_FreeValue(inst.ctx, obj);
            JS_FreeValue(inst.ctx, registry);
            continue;
        }

        JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
        int32_t arrLen = 0;
        JS_ToInt32(inst.ctx, &arrLen, lenVal);
        JS_FreeValue(inst.ctx, lenVal);

        JSValue arg = JS_NewStringLen(inst.ctx, data, len);
        for (int32_t i = 0; i < arrLen; ++i) {
            JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
            if (JS_IsFunction(inst.ctx, fn)) {
                JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &arg);
                if (JS_IsException(ret)) {
                    JSValue exc   = JS_GetException(inst.ctx);
                    const char *s = JS_ToCString(inst.ctx, exc);
                    sLog().error("ScriptEngine: input listener error: {}", s ? s : "(null)");
                    if (s) {
                        JS_FreeCString(inst.ctx, s);
                    }
                    JS_FreeValue(inst.ctx, exc);
                }
                JS_FreeValue(inst.ctx, ret);
            }
            JS_FreeValue(inst.ctx, fn);
        }
        JS_FreeValue(inst.ctx, arg);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverPopupInput(const std::string &regKey, const char *data, size_t len)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__popup_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, regKey.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__input_filters");
            if (!JS_IsUndefined(arr)) {
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0;
                JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);

                JSValue arg = JS_NewStringLen(inst.ctx, data, len);
                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &arg);
                        if (JS_IsException(ret)) {
                            JSValue exc   = JS_GetException(inst.ctx);
                            const char *s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: popup input error: {}", s ? s : "(null)");
                            if (s) {
                                JS_FreeCString(inst.ctx, s);
                            }
                            JS_FreeValue(inst.ctx, exc);
                        }
                        JS_FreeValue(inst.ctx, ret);
                    }
                    JS_FreeValue(inst.ctx, fn);
                }
                JS_FreeValue(inst.ctx, arg);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverEmbeddedInput(const std::string &regKey, const char *data, size_t len)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__embedded_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, regKey.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__input_filters");
            if (!JS_IsUndefined(arr)) {
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0;
                JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);

                JSValue arg = JS_NewStringLen(inst.ctx, data, len);
                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &arg);
                        if (JS_IsException(ret)) {
                            JSValue exc   = JS_GetException(inst.ctx);
                            const char *s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: embedded input error: {}", s ? s : "(null)");
                            if (s) {
                                JS_FreeCString(inst.ctx, s);
                            }
                            JS_FreeValue(inst.ctx, exc);
                        }
                        JS_FreeValue(inst.ctx, ret);
                    }
                    JS_FreeValue(inst.ctx, fn);
                }
                JS_FreeValue(inst.ctx, arg);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverEmbeddedDestroyed(const std::string &regKey)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__embedded_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, regKey.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__evt_destroyed");
            if (!JS_IsUndefined(arr)) {
                // Fire listeners with no argument.
                enqueueListeners(inst.ctx, arr, 0, nullptr);
            }
            JS_FreeValue(inst.ctx, arr);

            // Mark JsEmbeddedData as !alive so subsequent method calls throw.
            auto *emData = static_cast<JsEmbeddedData *>(JS_GetOpaque(obj, jsEmbeddedClassId));
            if (emData) {
                emData->alive = false;
            }
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverPopupMouseEvent(PaneId pane, const std::string &popupId,
                                    const std::string &type, int cellX, int cellY,
                                    int pixelX, int pixelY, int button, int delta)
{
    IterGuard guard(this);
    std::string regKey = pane.toString() + ":" + popupId;

    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__popup_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, regKey.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__mouse_listeners");
            if (!JS_IsUndefined(arr)) {
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0;
                JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);

                // Build event object: {type, cellX, cellY, pixelX, pixelY, button, delta}
                JSValue ev = JS_NewObject(inst.ctx);
                JS_SetPropertyStr(inst.ctx, ev, "type", JS_NewString(inst.ctx, type.c_str()));
                JS_SetPropertyStr(inst.ctx, ev, "cellX", JS_NewInt32(inst.ctx, cellX));
                JS_SetPropertyStr(inst.ctx, ev, "cellY", JS_NewInt32(inst.ctx, cellY));
                JS_SetPropertyStr(inst.ctx, ev, "pixelX", JS_NewInt32(inst.ctx, pixelX));
                JS_SetPropertyStr(inst.ctx, ev, "pixelY", JS_NewInt32(inst.ctx, pixelY));
                JS_SetPropertyStr(inst.ctx, ev, "button", JS_NewInt32(inst.ctx, button));
                JS_SetPropertyStr(inst.ctx, ev, "delta", JS_NewInt32(inst.ctx, delta));

                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &ev);
                        if (JS_IsException(ret)) {
                            JSValue exc   = JS_GetException(inst.ctx);
                            const char *s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: popup mouse error: {}", s ? s : "(null)");
                            if (s) {
                                JS_FreeCString(inst.ctx, s);
                            }
                            JS_FreeValue(inst.ctx, exc);
                        }
                        JS_FreeValue(inst.ctx, ret);
                    }
                    JS_FreeValue(inst.ctx, fn);
                }
                JS_FreeValue(inst.ctx, ev);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverMouseToRegistry(const char *registryName,
                                    const std::string &key, const std::string &type,
                                    int cellX, int cellY, int pixelX, int pixelY, int button, int delta)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, registryName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, key.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__mouse_listeners");
            if (!JS_IsUndefined(arr)) {
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0;
                JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);

                JSValue ev = JS_NewObject(inst.ctx);
                JS_SetPropertyStr(inst.ctx, ev, "type", JS_NewString(inst.ctx, type.c_str()));
                JS_SetPropertyStr(inst.ctx, ev, "cellX", JS_NewInt32(inst.ctx, cellX));
                JS_SetPropertyStr(inst.ctx, ev, "cellY", JS_NewInt32(inst.ctx, cellY));
                JS_SetPropertyStr(inst.ctx, ev, "pixelX", JS_NewInt32(inst.ctx, pixelX));
                JS_SetPropertyStr(inst.ctx, ev, "pixelY", JS_NewInt32(inst.ctx, pixelY));
                JS_SetPropertyStr(inst.ctx, ev, "button", JS_NewInt32(inst.ctx, button));
                JS_SetPropertyStr(inst.ctx, ev, "delta", JS_NewInt32(inst.ctx, delta));

                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &ev);
                        if (JS_IsException(ret)) {
                            JSValue exc   = JS_GetException(inst.ctx);
                            const char *s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: mouse event error: {}", s ? s : "(null)");
                            if (s) {
                                JS_FreeCString(inst.ctx, s);
                            }
                            JS_FreeValue(inst.ctx, exc);
                        }
                        JS_FreeValue(inst.ctx, ret);
                    }
                    JS_FreeValue(inst.ctx, fn);
                }
                JS_FreeValue(inst.ctx, ev);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverEmbeddedMouseEvent(PaneId pane, uint64_t lineId,
                                       const std::string &type,
                                       int cellX, int cellY, int pixelX, int pixelY,
                                       int button, int delta)
{
    deliverMouseToRegistry("__embedded_registry",
                           pane.toString() + ":" + std::to_string(lineId),
                           type,
                           cellX,
                           cellY,
                           pixelX,
                           pixelY,
                           button,
                           delta);
}

void Engine::deliverMousemoveToRegistry(const char *regName,
                                        const std::string &key,
                                        int cellX, int cellY, int pixelX, int pixelY)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, regName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, key.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__evt_mousemove");
            if (!JS_IsUndefined(arr)) {
                JSValue ev = JS_NewObject(inst.ctx);
                JS_SetPropertyStr(inst.ctx, ev, "cellX", JS_NewInt32(inst.ctx, cellX));
                JS_SetPropertyStr(inst.ctx, ev, "cellY", JS_NewInt32(inst.ctx, cellY));
                JS_SetPropertyStr(inst.ctx, ev, "pixelX", JS_NewInt32(inst.ctx, pixelX));
                JS_SetPropertyStr(inst.ctx, ev, "pixelY", JS_NewInt32(inst.ctx, pixelY));
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0;
                JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);
                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &ev);
                        if (JS_IsException(ret)) {
                            JSValue exc   = JS_GetException(inst.ctx);
                            const char *s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: mousemove error: {}", s ? s : "(null)");
                            if (s) {
                                JS_FreeCString(inst.ctx, s);
                            }
                            JS_FreeValue(inst.ctx, exc);
                        }
                        JS_FreeValue(inst.ctx, ret);
                    }
                    JS_FreeValue(inst.ctx, fn);
                }
                JS_FreeValue(inst.ctx, ev);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverPopupMouseMove(PaneId pane, const std::string &popupId,
                                   int cellX, int cellY, int pixelX, int pixelY)
{
    deliverMousemoveToRegistry("__popup_registry",
                               pane.toString() + ":" + popupId,
                               cellX,
                               cellY,
                               pixelX,
                               pixelY);
}

void Engine::deliverEmbeddedMouseMove(PaneId pane, uint64_t lineId,
                                      int cellX, int cellY, int pixelX, int pixelY)
{
    deliverMousemoveToRegistry("__embedded_registry",
                               pane.toString() + ":" + std::to_string(lineId),
                               cellX,
                               cellY,
                               pixelX,
                               pixelY);
}

void Engine::deliverResizedToRegistry(const char *regName, const std::string &key,
                                      int cols, int rows)
{
    IterGuard guard(this);
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, regName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }
        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, key.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__evt_resized");
            if (!JS_IsUndefined(arr)) {
                JSValue argv[2] = {
                    JS_NewInt32(inst.ctx, cols),
                    JS_NewInt32(inst.ctx, rows),
                };
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0;
                JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);
                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 2, argv);
                        if (JS_IsException(ret)) {
                            JSValue exc   = JS_GetException(inst.ctx);
                            const char *s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: resized error: {}", s ? s : "(null)");
                            if (s) {
                                JS_FreeCString(inst.ctx, s);
                            }
                            JS_FreeValue(inst.ctx, exc);
                        }
                        JS_FreeValue(inst.ctx, ret);
                    }
                    JS_FreeValue(inst.ctx, fn);
                }
                JS_FreeValue(inst.ctx, argv[0]);
                JS_FreeValue(inst.ctx, argv[1]);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverPopupResized(PaneId pane, const std::string &popupId, int cols, int rows)
{
    deliverResizedToRegistry("__popup_registry",
                             pane.toString() + ":" + popupId,
                             cols,
                             rows);
}

void Engine::deliverEmbeddedResized(PaneId pane, uint64_t lineId, int cols, int rows)
{
    deliverResizedToRegistry("__embedded_registry",
                             pane.toString() + ":" + std::to_string(lineId),
                             cols,
                             rows);
}

void Engine::deliverPaneMouseEvent(PaneId pane, const std::string &type,
                                   int cellX, int cellY, int pixelX, int pixelY, int button, int delta)
{
    deliverMouseToRegistry("__pane_registry", pane.toString(), type, cellX, cellY, pixelX, pixelY, button, delta);
}

void Engine::executePendingJobs()
{
    JSContext *pctx;
    while (JS_ExecutePendingJob(rt_, &pctx) > 0) {
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void Engine::cleanupPane(PaneId pane)
{
    IterGuard guard(this);
    paneOutputFilterCount_.erase(pane);
    paneInputFilterCount_.erase(pane);
    // Drop the count *and* the flag — but flip the flag to false first
    // so any in-flight parse worker that captured a shared_ptr to the
    // atomic sees no-filters (instead of taking the slow runOnMain
    // path against a now-erased registry).
    if (auto fl = paneOutputFilterFlag_.find(pane);
        fl != paneOutputFilterFlag_.end()) {
        fl->second->store(false, std::memory_order_release);
        paneOutputFilterFlag_.erase(fl);
    }
    if (auto fl = paneInputFilterFlag_.find(pane);
        fl != paneInputFilterFlag_.end()) {
        fl->second->store(false, std::memory_order_release);
        paneInputFilterFlag_.erase(fl);
    }

    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }

        JSValue global   = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) {
            continue;
        }

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            // Fire destroyed listeners
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_destroyed");
            enqueueListeners(inst.ctx, arr, 0, nullptr);
            JS_FreeValue(inst.ctx, arr);

            // Mark dead
            auto *data = jsPaneGet(inst.ctx, paneObj);
            if (data) {
                data->alive = false;
            }

            // Remove from registry
            JS_SetPropertyStr(inst.ctx, registry, pane.toString().c_str(), JS_UNDEFINED);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);

        // Also drop the memoised JS proxy so a future pane allocated to
        // the same uuid (post-recycle) gets a fresh JS object instead of
        // a stale one with `data->alive == false`.
        JSValue global2 = JS_GetGlobalObject(inst.ctx);
        JSValue cache   = JS_GetPropertyStr(inst.ctx, global2, "__pane_proxies");
        JS_FreeValue(inst.ctx, global2);
        if (!JS_IsUndefined(cache)) {
            JS_SetPropertyStr(inst.ctx, cache, pane.toString().c_str(), JS_UNDEFINED);
        }
        JS_FreeValue(inst.ctx, cache);
    }
}

void Engine::cleanupTab(TabId)
{
    // No per-tab JS object exists anymore — tab identity is just the
    // subtreeRoot Uuid, delivered as a string in the tabDestroyed event
    // payload. Listeners that tracked tabs by UUID are responsible for their
    // own bookkeeping. Kept as a no-op so existing call sites still link.
}

// ============================================================================
// Helpers
// ============================================================================

Engine::Instance *Engine::findInstance(InstanceId id)
{
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        if (inst.id == id) {
            return &inst;
        }
    }
    return nullptr;
}

Engine::Instance *Engine::findInstanceByCtx(JSContext *ctx)
{
    for (auto &inst : instances_) {
        if (!inst.ctx) {
            continue;
        }
        if (inst.ctx == ctx) {
            return &inst;
        }
    }
    return nullptr;
}

void Engine::addPaneOutputFilter(PaneId pane, InstanceId instId)
{
    paneOutputFilterCount_[pane]++;
    // Lazily create the per-pane atomic so the parse worker can read
    // it without taking any lock. outputFilterFlag() returns a
    // shared_ptr the worker captures; subsequent flips here update
    // that same atomic.
    auto fl = outputFilterFlag(pane);
    fl->store(true, std::memory_order_release);
    if (auto *inst = findInstance(instId)) {
        inst->paneOutputFilters.push_back(pane);
    }
}

void Engine::addPaneInputFilter(PaneId pane, InstanceId instId)
{
    paneInputFilterCount_[pane]++;
    auto fl = inputFilterFlag(pane);
    fl->store(true, std::memory_order_release);
    if (auto *inst = findInstance(instId)) {
        inst->paneInputFilters.push_back(pane);
    }
}

std::shared_ptr<std::atomic<bool>> Engine::outputFilterFlag(PaneId pane)
{
    auto &slot = paneOutputFilterFlag_[pane];
    if (!slot) {
        slot = std::make_shared<std::atomic<bool>>(false);
    }
    return slot;
}

std::shared_ptr<std::atomic<bool>> Engine::inputFilterFlag(PaneId pane)
{
    auto &slot = paneInputFilterFlag_[pane];
    if (!slot) {
        slot = std::make_shared<std::atomic<bool>>(false);
    }
    return slot;
}

void Engine::addPaneMouseMoveListener(PaneId pane, InstanceId instId)
{
    paneMouseMoveCount_[pane]++;
    if (auto *inst = findInstance(instId)) {
        inst->paneMouseMoveListeners.push_back(pane);
    }
}

void Engine::addPaneRowEvictedListener(PaneId pane, InstanceId instId)
{
    paneRowEvictedCount_[pane]++;
    auto fl = rowEvictedFlag(pane);
    fl->store(true, std::memory_order_release);
    if (auto *inst = findInstance(instId)) {
        inst->paneRowEvictedListeners.push_back(pane);
    }
}

void Engine::removePaneRowEvictedListener(PaneId pane, InstanceId instId)
{
    auto fc = paneRowEvictedCount_.find(pane);
    if (fc != paneRowEvictedCount_.end() && --fc->second <= 0) {
        paneRowEvictedCount_.erase(fc);
        if (auto fl = paneRowEvictedFlag_.find(pane);
            fl != paneRowEvictedFlag_.end()) {
            fl->second->store(false, std::memory_order_release);
        }
    }
    if (auto *inst = findInstance(instId)) {
        auto &v  = inst->paneRowEvictedListeners;
        auto pos = std::find(v.begin(), v.end(), pane);
        if (pos != v.end()) {
            v.erase(pos);
        }
    }
}

std::shared_ptr<std::atomic<bool>> Engine::rowEvictedFlag(PaneId pane)
{
    auto &slot = paneRowEvictedFlag_[pane];
    if (!slot) {
        slot = std::make_shared<std::atomic<bool>>(false);
    }
    return slot;
}

bool Engine::setNamespace(InstanceId id, const std::string &ns)
{
    auto *inst = findInstance(id);
    if (!inst) {
        return false;
    }
    if (!inst->ns.empty()) {
        return false; // already set
    }

    // Check no other instance holds this namespace
    for (auto &other : instances_) {
        if (!other.ctx) {
            continue;
        }
        if (other.id != id && other.ns == ns) {
            return false;
        }
    }

    inst->ns = ns;
    sLog().info("ScriptEngine: instance {} claimed namespace '{}'", id, ns);
    return true;
}

bool Engine::registerAction(InstanceId id, const std::string &name)
{
    auto *inst = findInstance(id);
    if (!inst) {
        return false;
    }
    if (inst->ns.empty()) {
        return false; // no namespace set
    }

    std::string fullName = inst->ns + "." + name;
    auto it              = registeredActions_.find(fullName);
    if (it != registeredActions_.end()) {
        // Already registered. Idempotent for the same instance (this is
        // what enables config.js to be re-eval'd in the same context on
        // hot-reload); fail for a different instance so we don't quietly
        // hand off ownership.
        return it->second == id;
    }
    registeredActions_.emplace(fullName, id);
    sLog().info("ScriptEngine: registered action '{}'", fullName);
    return true;
}

bool Engine::isActionRegistered(const std::string &fullName) const
{
    return registeredActions_.count(fullName) > 0;
}

std::vector<std::string> Engine::registeredActions() const
{
    std::vector<std::string> names;
    names.reserve(registeredActions_.size());
    for (const auto &[name, id] : registeredActions_) {
        names.push_back(name);
    }
    return names;
}

bool Engine::registerActionHandler(InstanceId id, const std::string &name, JSValue fn)
{
    Instance *inst = findInstance(id);
    if (!inst || !inst->ctx) {
        return false;
    }
    auto it = actionHandlers_.find(name);
    if (it != actionHandlers_.end()) {
        JS_FreeValue(it->second.ctx, it->second.fn);
        it->second = ActionHandler { id, inst->ctx, JS_DupValue(inst->ctx, fn) };
    } else {
        actionHandlers_.emplace(name,
                                ActionHandler { id, inst->ctx, JS_DupValue(inst->ctx, fn) });
    }
    return true;
}

bool Engine::unregisterActionHandler(const std::string &name)
{
    auto it = actionHandlers_.find(name);
    if (it == actionHandlers_.end()) {
        return false;
    }
    JS_FreeValue(it->second.ctx, it->second.fn);
    actionHandlers_.erase(it);
    return true;
}

bool Engine::invokeActionHandler(const std::string &name,
                                 const std::function<JSValue(JSContext *)> &buildArgs)
{
    auto it = actionHandlers_.find(name);
    if (it == actionHandlers_.end()) {
        return false;
    }
    JSContext *ctx = it->second.ctx;
    JSValue fn     = JS_DupValue(ctx, it->second.fn); // protect against re-entry unregister
    JSValue args   = buildArgs ? buildArgs(ctx) : JS_UNDEFINED;
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, &args);
    if (JS_IsException(result)) {
        JSValue exc   = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        sLog().error("action handler '{}' threw: {}", name, s ? s : "(unknown)");
        if (s) {
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, args);
    JS_FreeValue(ctx, fn);
    return true;
}

} // namespace Script
