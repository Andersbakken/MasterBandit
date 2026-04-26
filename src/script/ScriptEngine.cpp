#include "ScriptEngine.h"
#include "ScriptFsModule.h"
#include "ScriptWsModule.h"
#include "ScriptLayoutBindings.h"
#include "Action.h"
#include "LayoutTree.h"
#include "Terminal.h"
#include "Utils.h"
#include "Uuid.h"

#include <quickjs.h>
#include <spdlog/spdlog.h>
#include <eventloop/EventLoop.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef __APPLE__
#include <stdlib.h>  // arc4random_buf
#else
#include <sys/random.h>  // getrandom
#endif

namespace fs = std::filesystem;

static void fillSecureRandom(unsigned char* buf, size_t len)
{
#ifdef __APPLE__
    arc4random_buf(buf, len);
#else
    size_t off = 0;
    while (off < len) {
        ssize_t r = getrandom(buf + off, len - off, 0);
        if (r > 0) off += static_cast<size_t>(r);
        else if (r < 0 && errno != EINTR) {
            // Fall back to /dev/urandom if getrandom is unavailable.
            std::ifstream f("/dev/urandom", std::ios::binary);
            f.read(reinterpret_cast<char*>(buf + off), static_cast<std::streamsize>(len - off));
            break;
        }
    }
#endif
}

namespace Script {

static spdlog::logger& sLog()
{
    static auto l = spdlog::get("script");
    return l ? *l : *spdlog::default_logger();
}

// ============================================================================
// Module loader — resolves and validates import paths
// ============================================================================

// Resolve a canonical path, rejecting symlinks that escape the sandbox.
// Returns empty string if the path is outside the allowed directory.
static std::string resolveAndValidate(const std::string& path, const std::string& allowedDir)
{
    std::error_code ec;

    // Resolve the allowed directory to a canonical path
    auto canonicalAllowed = fs::canonical(allowedDir, ec);
    if (ec) return {};

    // Check if the target exists (without following the final symlink yet)
    if (!fs::exists(path, ec) || ec) return {};

    // Resolve to canonical path (resolves all symlinks)
    auto canonicalPath = fs::canonical(path, ec);
    if (ec) return {};

    // Verify the resolved path is under the allowed directory
    auto rel = canonicalPath.lexically_relative(canonicalAllowed);
    if (rel.empty() || rel.string().starts_with(".."))
        return {};

    return canonicalPath.string();
}

// Module name normalizer: resolves import specifiers to absolute paths.
// Called by QuickJS before the module loader.
static char* moduleNormalize(JSContext* ctx, const char* base_name,
                             const char* name, void* opaque)
{
    auto* eng = static_cast<Engine*>(opaque);

    // Built-in module (mb:fs, mb:tui, etc.)
    if (strncmp(name, "mb:", 3) == 0) {
        // Native C++ modules — returned as-is, handled in moduleLoader
        static const char* kNativeModules[] = { "fs", "ws", nullptr };
        std::string moduleName = name + 3;
        for (int i = 0; kNativeModules[i]; ++i) {
            if (moduleName == kNativeModules[i])
                return js_strdup(ctx, name); // pass through unchanged
        }

        // JS file modules in builtinModulesDir
        const auto& modulesDir = eng->builtinModulesDir();
        if (modulesDir.empty()) {
            JS_ThrowReferenceError(ctx, "No built-in modules directory configured");
            return nullptr;
        }
        std::string modulePath = modulesDir + "/" + moduleName + ".js";
        auto resolved = resolveAndValidate(modulePath, modulesDir);
        if (resolved.empty()) {
            JS_ThrowReferenceError(ctx, "Built-in module '%s' not found", name);
            return nullptr;
        }
        return js_strdup(ctx, resolved.c_str());
    }

    // Relative import — resolve against the importing script's directory
    fs::path basePath(base_name);
    fs::path baseDir = basePath.parent_path();
    fs::path resolved = baseDir / name;

    // Add .js extension if not present
    if (!resolved.has_extension())
        resolved.replace_extension(".js");

    std::string resolvedStr = resolved.string();

    // Validate: must be under the script's directory or the built-in modules dir
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) {
        JS_ThrowReferenceError(ctx, "Module '%s': no script context for import", name);
        return nullptr;
    }

    fs::path scriptDir = fs::path(inst->path).parent_path();
    auto validated = resolveAndValidate(resolvedStr, scriptDir.string());
    if (validated.empty()) {
        // Try built-in modules dir as fallback
        const auto& modulesDir = eng->builtinModulesDir();
        if (!modulesDir.empty())
            validated = resolveAndValidate(resolvedStr, modulesDir);
    }
    if (validated.empty()) {
        JS_ThrowReferenceError(ctx, "Module '%s' is outside allowed directories", name);
        return nullptr;
    }

    char* result = js_strdup(ctx, validated.c_str());
    return result;
}


// Module loader: reads the source file for a resolved module path.
static JSModuleDef* moduleLoader(JSContext* ctx, const char* module_name, void* opaque)
{
    auto* eng = static_cast<Engine*>(opaque);

    // Native C++ modules
    if (strcmp(module_name, "mb:fs") == 0)
        return createFsNativeModule(ctx, eng);
    if (strcmp(module_name, "mb:ws") == 0)
        return createWsNativeModule(ctx, eng);

    std::ifstream f(module_name, std::ios::binary);
    if (!f) {
        JS_ThrowReferenceError(ctx, "Cannot open module '%s'", module_name);
        return nullptr;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    JSValue func = JS_Eval(ctx, src.c_str(), src.size(), module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) return nullptr;

    JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return m;
}


static Engine* engineFromCtx(JSContext* ctx) {
    return static_cast<Engine*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

static Engine::Instance* instanceFromCtx(JSContext* ctx) {
    auto* eng = engineFromCtx(ctx);
    uintptr_t id = reinterpret_cast<uintptr_t>(JS_GetContextOpaque(ctx));
    return eng->findInstance(static_cast<InstanceId>(id));
}

static bool checkPerm(JSContext* ctx, uint32_t required) {
    auto* inst = instanceFromCtx(ctx);
    return inst && (inst->permissions & required) != 0;
}

// Schedule termination of a script that violated permissions
static void scheduleTermination(JSContext* ctx) {
    auto* eng = engineFromCtx(ctx);
    auto* inst = instanceFromCtx(ctx);
    if (!inst || inst->builtIn) return;
    InstanceId id = inst->id;
    sLog().error("ScriptEngine: terminating '{}' (id={}) for permission violation",
                  inst->path, id);
    if (!eng->loop()) { eng->unload(id); return; }
    eng->loop()->addTimer(0, false, [eng, id]() { eng->unload(id); });
}

#define REQUIRE_PERM(ctx, perm) \
    do { if (!checkPerm(ctx, Script::Perm::perm)) { \
        scheduleTermination(ctx); \
        return JS_ThrowTypeError(ctx, "permission denied: " #perm " not granted"); \
    } } while(0)

// ============================================================================
// Pane JS class
// ============================================================================

static JSClassID jsPaneClassId;

struct JsPaneData {
    PaneId id;
    bool alive;
};

static void jsPaneFinalize(JSRuntime*, JSValue val)
{
    delete static_cast<JsPaneData*>(JS_GetOpaque(val, jsPaneClassId));
}

static JSClassDef jsPaneClassDef = { "Pane", jsPaneFinalize };

static JSValue jsPaneNew(JSContext* ctx, PaneId id)
{
    JSValue obj = JS_NewObjectClass(ctx, jsPaneClassId);
    JS_SetOpaque(obj, new JsPaneData{id, true});
    return obj;
}

static JsPaneData* jsPaneGet(JSContext* ctx, JSValueConst val)
{
    return static_cast<JsPaneData*>(JS_GetOpaque(val, jsPaneClassId));
}

// Register a JS object in a global registry by integer key, so filters can find it.
static void registerInGlobal(JSContext* ctx, const char* registryName,
                              uint32_t key, JSValueConst obj)
{
    JSValue global = JS_GetGlobalObject(ctx);
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
static void registerInGlobal(JSContext* ctx, const char* registryName,
                              const std::string& key, JSValueConst obj)
{
    JSValue global = JS_GetGlobalObject(ctx);
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
static void removeFromJSArray(JSContext* ctx, JSValue arr, JSValueConst fn)
{
    if (JS_IsUndefined(arr)) return;
    JSValue indexOfFn = JS_GetPropertyStr(ctx, arr, "indexOf");
    JSValue idxVal    = JS_Call(ctx, indexOfFn, arr, 1, &fn);
    JS_FreeValue(ctx, indexOfFn);
    int32_t idx = -1;
    JS_ToInt32(ctx, &idx, idxVal);
    JS_FreeValue(ctx, idxVal);
    if (idx >= 0) {
        JSValue spliceFn = JS_GetPropertyStr(ctx, arr, "splice");
        JSValue args[2] = { JS_NewInt32(ctx, idx), JS_NewInt32(ctx, 1) };
        JSValue res = JS_Call(ctx, spliceFn, arr, 2, args);
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, spliceFn);
    }
}

static JSValue jsPaneAddEventListener(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    auto* inst = instanceFromCtx(ctx);
    InstanceId instId = inst ? inst->id : 0;

    std::string prop;
    if (strcmp(event, "output") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterOutput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterOutput"); }
        prop = "__output_filters";
        eng->addPaneOutputFilter(pane->id, instId);
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput"); }
        prop = "__input_filters";
        eng->addPaneInputFilter(pane->id, instId);
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "mouse") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: ui"); }
        prop = "__mouse_listeners";
        registerInGlobal(ctx, "__pane_registry", pane->id.toString(), this_val);
    } else if (strcmp(event, "mousemove") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: ui"); }
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

static JSValue jsPaneRemoveEventListener(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;
    std::string prop;
    if      (strcmp(event, "output") == 0) prop = "__output_filters";
    else if (strcmp(event, "input")  == 0) prop = "__input_filters";
    else if (strcmp(event, "mouse")  == 0) prop = "__mouse_listeners";
    else                                   prop = std::string("__evt_") + event;
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    removeFromJSArray(ctx, arr, argv[1]);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsPaneWrite(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "write requires (string)");
    REQUIRE_PERM(ctx, ShellWrite);
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneHasPty(pane->id))
        return JS_ThrowTypeError(ctx, "pane has no PTY");
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    eng->callbacks().writePaneToShell(pane->id, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue jsPanePaste(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "paste requires (string)");
    REQUIRE_PERM(ctx, ShellWrite);
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneHasPty(pane->id))
        return JS_ThrowTypeError(ctx, "pane has no PTY");
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
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

struct CmdObjData {
    Script::CommandInfo info;
    Script::PaneId paneId;
    // Sentinel means "not decoded yet". Freed via JS_FreeValueRT in finalizer.
    JSValue cachedCommand = JS_UNINITIALIZED;
    JSValue cachedOutput  = JS_UNINITIALIZED;
};

static JSClassID jsCommandClassId;

static void jsCommandFinalize(JSRuntime* rt, JSValue val)
{
    auto* d = static_cast<CmdObjData*>(JS_GetOpaque(val, jsCommandClassId));
    if (!d) return;
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
static JSValue buildCommandObject(JSContext* ctx, const Script::CommandInfo& p, PaneId paneId);

static JSValue jsPaneGetTextFromRows(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    if (argc < 4) return JS_ThrowTypeError(ctx, "getTextFromRows requires (startRowId, startCol, endRowId, endCol)");
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneGetText) return JS_NewString(ctx, "");
    uint64_t startId = 0, endId = 0;
    int32_t startCol = 0, endCol = 0;
    JS_ToIndex(ctx, &startId, argv[0]);
    JS_ToInt32(ctx, &startCol, argv[1]);
    JS_ToIndex(ctx, &endId, argv[2]);
    JS_ToInt32(ctx, &endCol, argv[3]);
    auto text = eng->callbacks().paneGetText(pane->id, startId, startCol, endId, endCol);
    return JS_NewStringLen(ctx, text.data(), text.size());
}

static JSValue jsPaneRowIdAt(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "rowIdAt requires (screenRow)");
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);
    auto info = eng->callbacks().paneInfo(pane->id);
    int32_t screenRow = 0;
    JS_ToInt32(ctx, &screenRow, argv[0]);
    if (screenRow < 0 || screenRow >= info.rows) return JS_NULL;
    if (!eng->callbacks().paneLineIdAt) return JS_NULL;
    auto result = eng->callbacks().paneLineIdAt(pane->id, screenRow);
    if (!result.has_value()) return JS_NULL;
    return JS_NewInt64(ctx, static_cast<int64_t>(*result));
}

static JSValue jsPaneLinkAt(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "linkAt requires (rowId, col)");
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneUrlAt) return JS_NULL;
    uint64_t lineId = 0;
    int32_t col = 0;
    JS_ToIndex(ctx, &lineId, argv[0]);
    JS_ToInt32(ctx, &col, argv[1]);
    auto url = eng->callbacks().paneUrlAt(pane->id, lineId, col);
    if (url.empty()) return JS_NULL;
    return JS_NewStringLen(ctx, url.data(), url.size());
}

static JSValue jsPaneGetLinksFromRows(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "getLinksFromRows requires (startRowId, endRowId, limit?)");
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneGetLinksFromRows) return JS_NewArray(ctx);
    uint64_t startId = 0, endId = 0;
    int32_t limit = 0;
    JS_ToIndex(ctx, &startId, argv[0]);
    JS_ToIndex(ctx, &endId, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &limit, argv[2]);
    auto links = eng->callbacks().paneGetLinksFromRows(pane->id, startId, endId, limit);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < links.size(); ++i) {
        const auto& l = links[i];
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "url", JS_NewStringLen(ctx, l.url.data(), l.url.size()));
        JS_SetPropertyStr(ctx, obj, "startRowId", JS_NewInt64(ctx, static_cast<int64_t>(l.startLineId)));
        JS_SetPropertyStr(ctx, obj, "startCol", JS_NewInt32(ctx, l.startCol));
        JS_SetPropertyStr(ctx, obj, "endRowId", JS_NewInt64(ctx, static_cast<int64_t>(l.endLineId)));
        JS_SetPropertyStr(ctx, obj, "endCol", JS_NewInt32(ctx, l.endCol));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

static JSValue jsPaneGetProp(JSContext* ctx, JSValueConst this_val, int magic)
{
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    auto info = eng->callbacks().paneInfo(pane->id);
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
        if (!info.selectedCommandId) return JS_NULL;
        if (!eng->callbacks().paneCommands) return JS_NULL;
        auto list = eng->callbacks().paneCommands(pane->id, 0);
        uint64_t target = *info.selectedCommandId;
        for (const auto& rec : list) {
            if (rec.id == target)
                return buildCommandObject(ctx, rec, pane->id);
        }
        return JS_NULL;
    }
    case 10: { // commands — gated on shell.commands.
        if (!checkPerm(ctx, Perm::ShellReadCommands)) {
            scheduleTermination(ctx);
            return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
        }
        JSValue arr = JS_NewArray(ctx);
        if (!eng->callbacks().paneCommands) return arr;
        auto list = eng->callbacks().paneCommands(pane->id, 0);
        for (size_t i = 0; i < list.size(); ++i)
            JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), buildCommandObject(ctx, list[i], pane->id));
        return arr;
    }
    case 11: { // selection → { startRowId, startCol, endRowId, endCol } | null
        REQUIRE_PERM(ctx, PaneSelection);
        if (!info.hasSelection) return JS_NULL;
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "startRowId", JS_NewInt64(ctx, static_cast<int64_t>(info.selectionStartLineId)));
        JS_SetPropertyStr(ctx, obj, "startCol",   JS_NewInt32(ctx, info.selectionStartCol));
        JS_SetPropertyStr(ctx, obj, "endRowId",   JS_NewInt64(ctx, static_cast<int64_t>(info.selectionEndLineId)));
        JS_SetPropertyStr(ctx, obj, "endCol",     JS_NewInt32(ctx, info.selectionEndCol));
        return obj;
    }
    case 12: { // cursor → { rowId, col }
        REQUIRE_PERM(ctx, PaneSelection);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "rowId", JS_NewInt64(ctx, static_cast<int64_t>(info.cursorLineId)));
        JS_SetPropertyStr(ctx, obj, "col",   JS_NewInt32(ctx, info.cursorCol));
        return obj;
    }
    case 13: return JS_NewInt64(ctx, static_cast<int64_t>(info.oldestLineId));
    case 14: return JS_NewInt64(ctx, static_cast<int64_t>(info.newestLineId));
    case 15: { // mousePosition → { cellX, cellY, pixelX, pixelY } | null
        if (!info.mouseInPane) return JS_NULL;
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "cellX",  JS_NewInt32(ctx, info.mouseCellX));
        JS_SetPropertyStr(ctx, obj, "cellY",  JS_NewInt32(ctx, info.mouseCellY));
        JS_SetPropertyStr(ctx, obj, "pixelX", JS_NewInt32(ctx, info.mousePixelX));
        JS_SetPropertyStr(ctx, obj, "pixelY", JS_NewInt32(ctx, info.mousePixelY));
        return obj;
    }
    case 16: { // selectedCommandId → number | null (gated on shell.commands, matching .commands)
        if (!checkPerm(ctx, Perm::ShellReadCommands)) {
            scheduleTermination(ctx);
            return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
        }
        if (!info.selectedCommandId) return JS_NULL;
        return JS_NewInt64(ctx, static_cast<int64_t>(*info.selectedCommandId));
    }
    case 17: // nodeId → UUID string of this pane's Terminal node in the shared
             // LayoutTree, or null if unattached. Ungated — UUIDs are just
             // handles that round-trip through mb.layout.node(...), which has
             // its own permission discipline for mutations.
        return info.nodeId.empty()
                 ? JS_NULL
                 : JS_NewStringLen(ctx, info.nodeId.data(), info.nodeId.size());
    default: return JS_UNDEFINED;
    }
}

static JSValue jsPaneSelectCommand(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
    if (!checkPerm(ctx, Perm::ShellReadCommands)) {
        scheduleTermination(ctx);
        return JS_ThrowTypeError(ctx, "permission denied: shell.commands not granted");
    }
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().paneSetSelectedCommand) return JS_UNDEFINED;
    std::optional<uint64_t> id;
    if (argc >= 1 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        int64_t v = 0;
        if (JS_ToInt64(ctx, &v, argv[0]) < 0) return JS_EXCEPTION;
        if (v < 0) return JS_ThrowRangeError(ctx, "selectCommand: id must be non-negative");
        id = static_cast<uint64_t>(v);
    }
    eng->callbacks().paneSetSelectedCommand(pane->id, id);
    return JS_UNDEFINED;
}

// Forward declarations — defined after Popup / Embedded classes
static JSValue jsPaneCreatePopup(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue jsPaneGetPopups(JSContext*, JSValueConst);
static JSValue jsPaneCreateEmbedded(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue jsPaneGetEmbeddeds(JSContext*, JSValueConst);

static const JSCFunctionListEntry jsPaneProto[] = {
    // Methods & getters listed here are PANE-SPECIFIC. `inject`, `cols`,
    // `rows`, `cursor`, `kind` are inherited from the Terminal base
    // prototype (see jsTerminalProto).
    JS_CFUNC_DEF("addEventListener", 2, jsPaneAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsPaneRemoveEventListener),
    JS_CFUNC_DEF("write", 1, jsPaneWrite),
    JS_CFUNC_DEF("paste", 1, jsPanePaste),
    JS_CFUNC_DEF("getTextFromRows", 4, jsPaneGetTextFromRows),
    JS_CFUNC_DEF("getLinksFromRows", 2, jsPaneGetLinksFromRows),
    JS_CFUNC_DEF("linkAt", 2, jsPaneLinkAt),
    JS_CFUNC_DEF("rowIdAt", 1, jsPaneRowIdAt),
    JS_CFUNC_DEF("createPopup", 1, jsPaneCreatePopup),
    JS_CFUNC_DEF("createEmbeddedTerminal", 1, jsPaneCreateEmbedded),
    JS_CGETSET_MAGIC_DEF("id", jsPaneGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("title", jsPaneGetProp, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("cwd", jsPaneGetProp, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("hasPty", jsPaneGetProp, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("focused", jsPaneGetProp, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("focusedPopupId", jsPaneGetProp, nullptr, 7),
    JS_CGETSET_MAGIC_DEF("foregroundProcess", jsPaneGetProp, nullptr, 8),
    JS_CGETSET_DEF("popups", jsPaneGetPopups, nullptr),
    JS_CGETSET_DEF("embeddeds", jsPaneGetEmbeddeds, nullptr),
    JS_CGETSET_MAGIC_DEF("selectedCommand", jsPaneGetProp, nullptr, 9),
    JS_CGETSET_MAGIC_DEF("commands",    jsPaneGetProp, nullptr, 10),
    JS_CGETSET_MAGIC_DEF("selection",    jsPaneGetProp, nullptr, 11),
    JS_CGETSET_MAGIC_DEF("oldestRowId",     jsPaneGetProp, nullptr, 13),
    JS_CGETSET_MAGIC_DEF("newestRowId",     jsPaneGetProp, nullptr, 14),
    JS_CGETSET_MAGIC_DEF("mousePosition",   jsPaneGetProp, nullptr, 15),
    JS_CGETSET_MAGIC_DEF("selectedCommandId", jsPaneGetProp, nullptr, 16),
    JS_CGETSET_MAGIC_DEF("nodeId",            jsPaneGetProp, nullptr, 17),
    JS_CFUNC_DEF("selectCommand", 1, jsPaneSelectCommand),
};

// ============================================================================
// Popup JS class — wraps (paneId, popupId string)
// ============================================================================

static JSClassID jsPopupClassId;

struct JsPopupData {
    PaneId paneId;
    std::string popupId;
    bool alive;
};

static void jsPopupFinalize(JSRuntime*, JSValue val)
{
    delete static_cast<JsPopupData*>(JS_GetOpaque(val, jsPopupClassId));
}

static JSClassDef jsPopupClassDef = { "Popup", jsPopupFinalize };

static JSValue jsPopupNew(JSContext* ctx, PaneId paneId, const std::string& popupId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsPopupClassId);
    JS_SetOpaque(obj, new JsPopupData{paneId, popupId, true});
    return obj;
}

static JsPopupData* jsPopupGet(JSContext* ctx, JSValueConst val)
{
    return static_cast<JsPopupData*>(JS_GetOpaque(val, jsPopupClassId));
}

// popup.close()
static JSValue jsPopupClose(JSContext* ctx, JSValueConst this_val,
                               int, JSValueConst*)
{
    REQUIRE_PERM(ctx, UiPopupDestroy);
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_ThrowTypeError(ctx, "popup is destroyed");
    engineFromCtx(ctx)->callbacks().destroyPopup(popup->paneId, popup->popupId);
    popup->alive = false;

    // Clear popup registry so a future popup with the same id on the same pane
    // registers fresh and input is delivered to the new popup's listeners.
    std::string regKey = popup->paneId.toString() + ":" + popup->popupId;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, "__popup_registry");
    if (!JS_IsUndefined(registry))
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_UNDEFINED);
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

// popup.addEventListener(event, fn) — stores on the popup object
static JSValue jsPopupAddEventListener(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_ThrowTypeError(ctx, "popup is destroyed");

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput"); }
        prop = "__input_filters";
    } else if (strcmp(event, "mouse") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: ui"); }
        prop = "__mouse_listeners";
    } else if (strcmp(event, "mousemove") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: ui"); }
        prop = "__evt_mousemove";
    } else {
        prop = std::string("__evt_") + event;
    }
    JS_FreeCString(ctx, event);

    // Register in popup registry for input/mouse delivery
    Engine* eng = engineFromCtx(ctx);
    std::string regKey = popup->paneId.toString() + ":" + popup->popupId;
    // Use a string-keyed property on a global popup registry
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, "__popup_registry");
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__popup_registry", JS_DupValue(ctx, registry));
    }
    JSValue existing = JS_GetPropertyStr(ctx, registry, regKey.c_str());
    if (JS_IsUndefined(existing))
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_DupValue(ctx, this_val));
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

static JSValue jsPopupRemoveEventListener(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_ThrowTypeError(ctx, "popup is destroyed");
    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;
    std::string prop;
    if      (strcmp(event, "input") == 0) prop = "__input_filters";
    else if (strcmp(event, "mouse") == 0) prop = "__mouse_listeners";
    else                                  prop = std::string("__evt_") + event;
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    removeFromJSArray(ctx, arr, argv[1]);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

// popup property getters
static JSValue jsPopupGetProp(JSContext* ctx, JSValueConst this_val, int magic)
{
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
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
    case 5: case 6: {
        // x, y — look up from panePopups (cols/rows come from Terminal base).
        auto popups = eng->callbacks().panePopups(popup->paneId);
        for (const auto& p : popups) {
            if (p.id == popup->popupId) {
                if (magic == 5) return JS_NewInt32(ctx, p.x);
                if (magic == 6) return JS_NewInt32(ctx, p.y);
            }
        }
        return JS_UNDEFINED;
    }
    default: return JS_UNDEFINED;
    }
}

// popup.resize({x, y, w, h})
static JSValue jsPopupResize(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "resize requires ({x, y, w, h})");
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_ThrowTypeError(ctx, "popup is destroyed");

    int32_t x, y, w, h;
    JSValue v;
    // Default to current values if not specified — need to query
    // For simplicity, all four are required
    v = JS_GetPropertyStr(ctx, argv[0], "x"); JS_ToInt32(ctx, &x, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "y"); JS_ToInt32(ctx, &y, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "w"); JS_ToInt32(ctx, &w, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "h"); JS_ToInt32(ctx, &h, v); JS_FreeValue(ctx, v);

    Engine* eng = engineFromCtx(ctx);
    eng->callbacks().resizePopup(popup->paneId, popup->popupId, x, y, w, h);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry jsPopupProto[] = {
    // `inject`, `cols`, `rows`, `cursor`, `kind` inherited from Terminal base.
    JS_CFUNC_DEF("addEventListener", 2, jsPopupAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsPopupRemoveEventListener),
    JS_CFUNC_DEF("resize", 1, jsPopupResize),
    JS_CFUNC_DEF("close", 0, jsPopupClose),
    JS_CGETSET_MAGIC_DEF("paneId", jsPopupGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("id", jsPopupGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("focused", jsPopupGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("x", jsPopupGetProp, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("y", jsPopupGetProp, nullptr, 6),
};

// pane.createPopup({id, x, y, w, h}) -> Popup
static JSValue jsPaneCreatePopup(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "createPopup requires ({id, x, y, w, h})");
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);

    const char* id = nullptr;
    JSValue idVal = JS_GetPropertyStr(ctx, argv[0], "id");
    if (JS_IsString(idVal)) id = JS_ToCString(ctx, idVal);
    JS_FreeValue(ctx, idVal);
    if (!id) return JS_ThrowTypeError(ctx, "createPopup: missing 'id' property");

    int32_t x = 0, y = 0, w = 20, h = 5;
    JSValue v;
    v = JS_GetPropertyStr(ctx, argv[0], "x"); JS_ToInt32(ctx, &x, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "y"); JS_ToInt32(ctx, &y, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "w"); JS_ToInt32(ctx, &w, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "h"); JS_ToInt32(ctx, &h, v); JS_FreeValue(ctx, v);

    std::string popupId(id);
    JS_FreeCString(ctx, id);

    Uuid paneId = pane->id;
    bool ok = eng->callbacks().createPopup(paneId, popupId, x, y, w, h,
        [eng, paneId, popupId](const char* data, size_t len) {
            // Deliver input to popup listeners
            std::string regKey = paneId.toString() + ":" + popupId;
            eng->deliverPopupInput(regKey, data, len);
        });

    if (!ok) return JS_ThrowTypeError(ctx, "createPopup failed (duplicate id?)");

    // Track ownership for cleanup on unload
    auto* inst = instanceFromCtx(ctx);
    if (inst) inst->ownedPopups.push_back({paneId, popupId});

    return jsPopupNew(ctx, paneId, popupId);
}

// pane.popups — returns array of Popup objects for this pane
static JSValue jsPaneGetPopups(JSContext* ctx, JSValueConst this_val)
{
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_NewArray(ctx);
    Engine* eng = engineFromCtx(ctx);
    auto popups = eng->callbacks().panePopups(pane->id);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < popups.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, jsPopupNew(ctx, pane->id, popups[i].id));
    return arr;
}

// ============================================================================
// EmbeddedTerminal JS class — wraps (paneId, lineId)
// ============================================================================

static JSClassID jsEmbeddedClassId;

struct JsEmbeddedData {
    PaneId paneId;
    uint64_t lineId;
    bool alive;
};

static void jsEmbeddedFinalize(JSRuntime*, JSValue val)
{
    delete static_cast<JsEmbeddedData*>(JS_GetOpaque(val, jsEmbeddedClassId));
}

static JSClassDef jsEmbeddedClassDef = { "EmbeddedTerminal", jsEmbeddedFinalize };

static JSValue jsEmbeddedNew(JSContext* ctx, PaneId paneId, uint64_t lineId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsEmbeddedClassId);
    JS_SetOpaque(obj, new JsEmbeddedData{paneId, lineId, true});
    return obj;
}

static JsEmbeddedData* jsEmbeddedGet(JSContext*, JSValueConst val)
{
    return static_cast<JsEmbeddedData*>(JS_GetOpaque(val, jsEmbeddedClassId));
}

// embedded.resize(rows)
static JSValue jsEmbeddedResize(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "resize requires (rows)");
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto* em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) return JS_ThrowTypeError(ctx, "embedded is destroyed");
    int32_t rows = 0;
    if (JS_ToInt32(ctx, &rows, argv[0]) < 0) return JS_EXCEPTION;
    bool ok = engineFromCtx(ctx)->callbacks().resizeEmbedded(em->paneId, em->lineId, rows);
    return JS_NewBool(ctx, ok);
}

// embedded.close()
static JSValue jsEmbeddedClose(JSContext* ctx, JSValueConst this_val,
                                int, JSValueConst*)
{
    REQUIRE_PERM(ctx, UiPopupDestroy);
    auto* em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) return JS_ThrowTypeError(ctx, "embedded is destroyed");
    Engine* eng = engineFromCtx(ctx);
    // Capture regKey before destroy invalidates the embedded.
    std::string regKey = em->paneId.toString() + ":" + std::to_string(em->lineId);
    eng->callbacks().destroyEmbedded(em->paneId, em->lineId);
    em->alive = false;
    // Fire destroyed event to listeners, then clear registry entry so a
    // future embedded at the same lineId (unlikely — ids are monotonic)
    // registers fresh.
    eng->deliverEmbeddedDestroyed(regKey);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, "__embedded_registry");
    if (!JS_IsUndefined(registry))
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_UNDEFINED);
    JS_FreeValue(ctx, registry);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

// embedded.addEventListener(event, fn)
// Supported events: "input" (keystrokes when focused), "destroyed" (eviction or close).
static JSValue jsEmbeddedAddEventListener(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
    auto* em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) return JS_ThrowTypeError(ctx, "embedded is destroyed");

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput"); }
        prop = "__input_filters";
    } else if (strcmp(event, "mouse") == 0 || strcmp(event, "mousemove") == 0) {
        if (!checkPerm(ctx, Perm::GroupUi)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: ui"); }
        // Mirror pane: "mouse" → dedicated list (press/release), "mousemove" → __evt_ bucket.
        prop = (strcmp(event, "mouse") == 0) ? "__mouse_listeners" : "__evt_mousemove";
    } else {
        prop = std::string("__evt_") + event;
    }
    JS_FreeCString(ctx, event);

    std::string regKey = em->paneId.toString() + ":" + std::to_string(em->lineId);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue registry = JS_GetPropertyStr(ctx, global, "__embedded_registry");
    if (JS_IsUndefined(registry)) {
        registry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__embedded_registry", JS_DupValue(ctx, registry));
    }
    JSValue existing = JS_GetPropertyStr(ctx, registry, regKey.c_str());
    if (JS_IsUndefined(existing))
        JS_SetPropertyStr(ctx, registry, regKey.c_str(), JS_DupValue(ctx, this_val));
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

static JSValue jsEmbeddedRemoveEventListener(JSContext* ctx, JSValueConst this_val,
                                               int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
    auto* em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) return JS_ThrowTypeError(ctx, "embedded is destroyed");
    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;
    std::string prop;
    if (strcmp(event, "input") == 0)           prop = "__input_filters";
    else if (strcmp(event, "mouse") == 0)      prop = "__mouse_listeners";
    else                                        prop = std::string("__evt_") + event;
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    removeFromJSArray(ctx, arr, argv[1]);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

// embedded.<property> getter
static JSValue jsEmbeddedGetProp(JSContext* ctx, JSValueConst this_val, int magic)
{
    auto* em = jsEmbeddedGet(ctx, this_val);
    if (!em || !em->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    switch (magic) {
    case 0: { std::string s = em->paneId.toString(); return JS_NewStringLen(ctx, s.data(), s.size()); }
    case 1: return JS_NewInt64(ctx, static_cast<int64_t>(em->lineId));
    case 4: {
        // focused — cols/rows now come from the Terminal base class.
        auto embeds = eng->callbacks().paneEmbeddeds(em->paneId);
        for (const auto& e : embeds)
            if (e.lineId == em->lineId) return JS_NewBool(ctx, e.focused);
        return JS_UNDEFINED;
    }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry jsEmbeddedProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsEmbeddedAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsEmbeddedRemoveEventListener),
    JS_CFUNC_DEF("resize", 1, jsEmbeddedResize),
    JS_CFUNC_DEF("close", 0, jsEmbeddedClose),
    JS_CGETSET_MAGIC_DEF("paneId", jsEmbeddedGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("id",     jsEmbeddedGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("focused", jsEmbeddedGetProp, nullptr, 4),
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
static TerminalEmulator* resolveEmulatorFromVal(JSContext* ctx, JSValueConst val)
{
    Engine* eng = engineFromCtx(ctx);
    if (!eng) return nullptr;
    if (auto* p = static_cast<JsPaneData*>(JS_GetOpaque(val, jsPaneClassId))) {
        if (!p->alive) return nullptr;
        return eng->terminal(p->id);
    }
    if (auto* d = static_cast<JsPopupData*>(JS_GetOpaque(val, jsPopupClassId))) {
        if (!d->alive) return nullptr;
        Terminal* parent = eng->terminal(d->paneId);
        return parent ? parent->findPopup(d->popupId) : nullptr;
    }
    if (auto* d = static_cast<JsEmbeddedData*>(JS_GetOpaque(val, jsEmbeddedClassId))) {
        if (!d->alive) return nullptr;
        Terminal* parent = eng->terminal(d->paneId);
        return parent ? parent->findEmbedded(d->lineId) : nullptr;
    }
    return nullptr;
}

// terminal.inject(data)
static JSValue jsTerminalInject(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "inject requires (string)");
    REQUIRE_PERM(ctx, IoInject);
    TerminalEmulator* emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) return JS_ThrowTypeError(ctx, "terminal is destroyed");
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    emu->injectData(str, len);
    JS_FreeCString(ctx, str);
    if (auto& cb = engineFromCtx(ctx)->callbacks().requestRedraw) cb();
    return JS_UNDEFINED;
}

// Shared getters: 0 = cols, 1 = rows, 2 = cursor, 3 = kind, 4 = cellWidth, 5 = cellHeight.
static JSValue jsTerminalGetProp(JSContext* ctx, JSValueConst this_val, int magic)
{
    if (magic == 3) {
        // `kind` — discriminator for subclass. Doesn't require a live emulator.
        JSClassID cid = JS_GetClassID(this_val);
        if (cid == jsPaneClassId)     return JS_NewString(ctx, "pane");
        if (cid == jsPopupClassId)    return JS_NewString(ctx, "popup");
        if (cid == jsEmbeddedClassId) return JS_NewString(ctx, "embedded");
        return JS_NewString(ctx, "terminal");
    }
    if (magic == 4 || magic == 5) {
        // cellWidth / cellHeight in pixels — window-global font metrics.
        auto& cb = engineFromCtx(ctx)->callbacks().fontCellSize;
        if (!cb) return JS_NewFloat64(ctx, 0.0);
        auto [cw, ch] = cb();
        return JS_NewFloat64(ctx, magic == 4 ? cw : ch);
    }
    TerminalEmulator* emu = resolveEmulatorFromVal(ctx, this_val);
    if (!emu) return JS_UNDEFINED;
    switch (magic) {
    case 0: return JS_NewInt32(ctx, emu->width());
    case 1: return JS_NewInt32(ctx, emu->height());
    case 2: {
        // cursor → { rowId, col, visible }.  PaneSelection gate applies
        // only when the caller is a shell pane — applets querying their
        // own popup/embedded don't need the extra grant (the cursor they
        // see is their own drawing).
        if (JS_GetClassID(this_val) == jsPaneClassId) {
            REQUIRE_PERM(ctx, PaneSelection);
        }
        std::lock_guard<std::recursive_mutex> _lk(emu->mutex());
        int absRow = emu->document().historySize() + emu->cursorY();
        uint64_t rowId = emu->document().lineIdForAbs(absRow);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "rowId",   JS_NewInt64(ctx, static_cast<int64_t>(rowId)));
        JS_SetPropertyStr(ctx, obj, "col",     JS_NewInt32(ctx, emu->cursorX()));
        JS_SetPropertyStr(ctx, obj, "visible", JS_NewBool(ctx, emu->cursorVisible()));
        return obj;
    }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry jsTerminalProto[] = {
    JS_CFUNC_DEF("inject", 1, jsTerminalInject),
    JS_CGETSET_MAGIC_DEF("cols",       jsTerminalGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("rows",       jsTerminalGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("cursor",     jsTerminalGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("kind",       jsTerminalGetProp, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("cellWidth",  jsTerminalGetProp, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("cellHeight", jsTerminalGetProp, nullptr, 5),
};

// pane.createEmbeddedTerminal({rows}) -> EmbeddedTerminal | null
static JSValue jsPaneCreateEmbedded(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createEmbeddedTerminal requires ({rows})");
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_ThrowTypeError(ctx, "pane is destroyed");
    Engine* eng = engineFromCtx(ctx);

    int32_t rows = 0;
    JSValue v = JS_GetPropertyStr(ctx, argv[0], "rows");
    JS_ToInt32(ctx, &rows, v); JS_FreeValue(ctx, v);
    if (rows <= 0) return JS_ThrowTypeError(ctx, "createEmbeddedTerminal: 'rows' must be > 0");

    Uuid paneId = pane->id;
    uint64_t lineId = eng->callbacks().createEmbedded(paneId, rows);

    if (lineId == 0)
        return JS_NULL;

    auto* inst = instanceFromCtx(ctx);
    if (inst) inst->ownedEmbeddeds.push_back({paneId, lineId});

    return jsEmbeddedNew(ctx, paneId, lineId);
}

// pane.embeddeds — array of EmbeddedTerminal objects for this pane
static JSValue jsPaneGetEmbeddeds(JSContext* ctx, JSValueConst this_val)
{
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_NewArray(ctx);
    Engine* eng = engineFromCtx(ctx);
    auto embeds = eng->callbacks().paneEmbeddeds(pane->id);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < embeds.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, jsEmbeddedNew(ctx, pane->id, embeds[i].lineId));
    return arr;
}

static JSClassID jsTabClassId;

struct JsTabData {
    TabId id;
    bool alive;
};

static void jsTabFinalize(JSRuntime*, JSValue val)
{
    delete static_cast<JsTabData*>(JS_GetOpaque(val, jsTabClassId));
}

static JSClassDef jsTabClassDef = { "Tab", jsTabFinalize };

static JSValue jsTabNew(JSContext* ctx, TabId id)
{
    JSValue obj = JS_NewObjectClass(ctx, jsTabClassId);
    JS_SetOpaque(obj, new JsTabData{id, true});
    return obj;
}

static JsTabData* jsTabGet(JSContext* ctx, JSValueConst val)
{
    return static_cast<JsTabData*>(JS_GetOpaque(val, jsTabClassId));
}

static JSValue jsTabAddEventListener(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_ThrowTypeError(ctx, "tab is destroyed");

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;
    std::string prop = std::string("__evt_") + event;
    JS_FreeCString(ctx, event);

    registerInGlobal(ctx, "__tab_registry", static_cast<uint32_t>(tab->id), this_val);

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

static JSValue jsTabRemoveEventListener(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)");
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_ThrowTypeError(ctx, "tab is destroyed");
    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;
    std::string prop = std::string("__evt_") + event;
    JS_FreeCString(ctx, event);
    JSValue arr = JS_GetPropertyStr(ctx, this_val, prop.c_str());
    removeFromJSArray(ctx, arr, argv[1]);
    JS_FreeValue(ctx, arr);
    return JS_UNDEFINED;
}

static JSValue jsTabGetId(JSContext* ctx, JSValueConst this_val)
{
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab) return JS_UNDEFINED;
    return JS_NewInt32(ctx, tab->id);
}

// tab.nodeId → UUID string of this tab's Container in the shared LayoutTree,
// or null when the tab has no tree representation. Ungated — see pane.nodeId.
static JSValue jsTabGetNodeId(JSContext* ctx, JSValueConst this_val)
{
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    for (auto& ti : tabInfos) {
        if (ti.id == tab->id) {
            return ti.nodeId.empty()
                     ? JS_NULL
                     : JS_NewStringLen(ctx, ti.nodeId.data(), ti.nodeId.size());
        }
    }
    return JS_NULL;
}

// tab.panes — returns array of Pane objects
static JSValue jsTabGetPanes(JSContext* ctx, JSValueConst this_val)
{
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    for (auto& ti : tabInfos) {
        if (ti.id == tab->id) {
            JSValue arr = JS_NewArray(ctx);
            for (uint32_t i = 0; i < ti.panes.size(); ++i)
                JS_SetPropertyUint32(ctx, arr, i, jsPaneNew(ctx, ti.panes[i]));
            return arr;
        }
    }
    return JS_NewArray(ctx);
}

// tab.activePane — returns focused pane or undefined
static JSValue jsTabGetActivePane(JSContext* ctx, JSValueConst this_val)
{
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    for (auto& ti : tabInfos) {
        if (ti.id == tab->id && !ti.focusedPane.isNil())
            return jsPaneNew(ctx, ti.focusedPane);
    }
    return JS_UNDEFINED;
}

// tab.close()
static const JSCFunctionListEntry jsTabProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsTabAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, jsTabRemoveEventListener),
    JS_CGETSET_DEF("id", jsTabGetId, nullptr),
    JS_CGETSET_DEF("nodeId", jsTabGetNodeId, nullptr),
    JS_CGETSET_DEF("panes", jsTabGetPanes, nullptr),
    JS_CGETSET_DEF("activePane", jsTabGetActivePane, nullptr),
};

// ============================================================================
// mb global — controller API
// ============================================================================

static JSValue jsMbInvokeAction(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "invokeAction requires (name, ...args)");
    REQUIRE_PERM(ctx, ActionsInvoke);
    Engine* eng = engineFromCtx(ctx);
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

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
        const char* arg = JS_ToCString(ctx, argv[i]);
        if (arg) { args.emplace_back(arg); JS_FreeCString(ctx, arg); }
    }
    bool ok = eng->callbacks().invokeAction(std::string(name), args);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

// mb.addEventListener("action", "ActionName", fn) — 3 args
// mb.addEventListener("tabCreated", fn) — 2 args
static JSValue jsMbAddEventListener(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "addEventListener requires (string, ...)");

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    JSValueConst callback;

    if (strcmp(event, "action") == 0) {
        // Three-arg form: ("action", "ActionName", fn)
        if (argc < 3 || !JS_IsString(argv[1]) || !JS_IsFunction(ctx, argv[2])) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "addEventListener('action', ...) requires (string, string, function)");
        }
        const char* actionName = JS_ToCString(ctx, argv[1]);
        if (!actionName) { JS_FreeCString(ctx, event); return JS_EXCEPTION; }
        prop = std::string("__evt_action_") + actionName;
        JS_FreeCString(ctx, actionName);
        callback = argv[2];
    } else {
        // Two-arg form: ("tabCreated", fn)
        if (!JS_IsFunction(ctx, argv[1])) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "addEventListener requires (string, function)");
        }
        prop = std::string("__evt_") + event;
        callback = argv[1];
    }
    JS_FreeCString(ctx, event);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb = JS_GetPropertyStr(ctx, global, "mb");
    JSValue arr = JS_GetPropertyStr(ctx, mb, prop.c_str());
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

static JSValue jsMbRemoveEventListener(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "removeEventListener requires (string, ...)");

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    JSValueConst callback;
    if (strcmp(event, "action") == 0) {
        if (argc < 3 || !JS_IsString(argv[1]) || !JS_IsFunction(ctx, argv[2])) {
            JS_FreeCString(ctx, event);
            return JS_ThrowTypeError(ctx, "removeEventListener('action', ...) requires (string, string, function)");
        }
        const char* actionName = JS_ToCString(ctx, argv[1]);
        if (!actionName) { JS_FreeCString(ctx, event); return JS_EXCEPTION; }
        prop = std::string("__evt_action_") + actionName;
        JS_FreeCString(ctx, actionName);
        callback = argv[2];
    } else {
        if (!JS_IsFunction(ctx, argv[1])) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "removeEventListener requires (string, function)"); }
        prop = std::string("__evt_") + event;
        callback = argv[1];
    }
    JS_FreeCString(ctx, event);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb = JS_GetPropertyStr(ctx, global, "mb");
    JSValue arr = JS_GetPropertyStr(ctx, mb, prop.c_str());
    removeFromJSArray(ctx, arr, callback);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, mb);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

static JSValue jsMbGetTabs(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < tabInfos.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, jsTabNew(ctx, tabInfos[i].id));
    return arr;
}

static JSValue jsMbGetActivePane(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    for (auto& ti : tabInfos)
        if (ti.active && !ti.focusedPane.isNil())
            return jsPaneNew(ctx, ti.focusedPane);
    return JS_UNDEFINED;
}

// PascalCase → snake_case: "FocusPane" → "focus_pane"
static std::string toSnakeCase(std::string_view name) {
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 0) result += '_';
            result += static_cast<char>(c + 32);
        } else {
            result += c;
        }
    }
    return result;
}

// PascalCase → spaced label: "FocusPane" → "Focus Pane"
static std::string toLabel(std::string_view name) {
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z' && i > 0)
            result += ' ';
        result += c;
    }
    return result;
}

static JSValue jsMbActionsRegister(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue jsMbActionsUnregister(JSContext*, JSValueConst, int, JSValueConst*);

// mb.actions -> array of {name, label, builtin, args?} objects plus
// register/unregister methods for handler ownership of JS-owned actions.
static JSValue jsMbGetActions(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);

    // Directional arg expansions for actions that take Direction
    struct ArgVariant { const char* arg; const char* labelSuffix; };
    static const std::unordered_map<std::string_view, std::vector<ArgVariant>> argVariants = {
        {"SplitPane",  {{"right", "Right"}, {"down", "Down"}, {"left", "Left"}, {"up", "Up"}}},
        {"FocusPane",  {{"next", "Next"}, {"prev", "Previous"}, {"left", "Left"}, {"right", "Right"}, {"up", "Up"}, {"down", "Down"}}},
        {"AdjustPaneSize", {{"left", "Left"}, {"right", "Right"}, {"up", "Up"}, {"down", "Down"}}},
        {"ScrollToPrompt", {{"-1", "Previous"}, {"1", "Next"}}},
    };

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;

    for (Action::TypeIndex i = 0; i < Action::count; ++i) {
        auto pascalName = Action::nameTable[i];
        // Skip ScriptAction — script actions come from the registered set below
        if (pascalName == "ScriptAction") continue;

        std::string snakeName = toSnakeCase(pascalName);
        std::string baseLabel = toLabel(pascalName);

        auto vit = argVariants.find(pascalName);
        if (vit != argVariants.end()) {
            // Expand into one entry per variant
            for (const auto& v : vit->second) {
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
    for (const auto& fullName : eng->registeredActions()) {
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
    JS_SetPropertyStr(ctx, arr, "register",
        JS_NewCFunction(ctx, jsMbActionsRegister, "register", 2));
    JS_SetPropertyStr(ctx, arr, "unregister",
        JS_NewCFunction(ctx, jsMbActionsUnregister, "unregister", 1));

    return arr;
}

// mb.createUuid() -> 36-char UUID v4 string ("xxxxxxxx-xxxx-4xxx-Nxxx-xxxxxxxxxxxx").
// Ungated — providing randomness confers no capability. String form is the only
// JS-safe representation; 128-bit integers don't round-trip through JS Number.
static JSValue jsMbCreateUuid(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    std::string s = Uuid::generate().toString();
    return JS_NewStringLen(ctx, s.data(), s.size());
}

// mb.createSecureToken(length = 32) -> hex string
// Generates `length` cryptographically-secure random bytes and returns them as a 2*length
// hex string. Ungated — providing randomness confers no capability.
static JSValue jsMbCreateSecureToken(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    int length = 32;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t n;
        if (JS_ToInt32(ctx, &n, argv[0]) == 0 && n > 0 && n <= 4096)
            length = n;
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
static JSValue loadResultToJs(JSContext* ctx, const Engine::LoadResult& res)
{
    JSValue obj = JS_NewObject(ctx);
    const char* statusStr = "error";
    switch (res.status) {
        case Engine::LoadResult::Status::Loaded:  statusStr = "loaded";  break;
        case Engine::LoadResult::Status::Pending: statusStr = "pending"; break;
        case Engine::LoadResult::Status::Denied:  statusStr = "denied";  break;
        case Engine::LoadResult::Status::Error:   statusStr = "error";   break;
    }
    JS_SetPropertyStr(ctx, obj, "status", JS_NewString(ctx, statusStr));
    if (res.status == Engine::LoadResult::Status::Loaded)
        JS_SetPropertyStr(ctx, obj, "id", JS_NewInt64(ctx, static_cast<int64_t>(res.id)));
    if (res.status == Engine::LoadResult::Status::Error && !res.error.empty())
        JS_SetPropertyStr(ctx, obj, "error",
                          JS_NewStringLen(ctx, res.error.data(), res.error.size()));
    return obj;
}

// mb.loadScript(path, permissionsStr) -> { status, id?, error? }
static JSValue jsMbLoadScript(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) {
        Engine::LoadResult res{ Engine::LoadResult::Status::Error, 0, "path argument required" };
        return loadResultToJs(ctx, res);
    }
    REQUIRE_PERM(ctx, ScriptsLoad);
    Engine* eng = engineFromCtx(ctx);

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    std::string permsStr;
    if (argc >= 2) {
        const char* p = JS_ToCString(ctx, argv[1]);
        if (p) { permsStr = p; JS_FreeCString(ctx, p); }
    }

    uint32_t perms = parsePermissions(permsStr);
    Engine::LoadResult res = eng->loadScript(std::string(path), perms);
    JS_FreeCString(ctx, path);
    return loadResultToJs(ctx, res);
}

// mb.approveScript(path, response) — response is "y", "n", "a", "d".
// Returns the final LoadResult as { status, id?, error? }.
static JSValue jsMbApproveScript(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) {
        Engine::LoadResult res{ Engine::LoadResult::Status::Error, 0, "approveScript requires path and response" };
        return loadResultToJs(ctx, res);
    }
    // Only built-in scripts can approve
    auto* inst = instanceFromCtx(ctx);
    if (!inst || !inst->builtIn)
        return JS_ThrowTypeError(ctx, "approveScript is only available to built-in scripts");

    Engine* eng = engineFromCtx(ctx);
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    const char* resp = JS_ToCString(ctx, argv[1]);
    if (!resp) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }

    char response = resp[0];
    JS_FreeCString(ctx, resp);

    Engine::LoadResult res = eng->approveScript(std::string(path), response);
    JS_FreeCString(ctx, path);
    return loadResultToJs(ctx, res);
}

// mb.unloadScript(id)
static JSValue jsMbUnloadScript(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "unloadScript requires (id)");
    REQUIRE_PERM(ctx, ScriptsUnload);
    Engine* eng = engineFromCtx(ctx);
    int64_t id;
    JS_ToInt64(ctx, &id, argv[0]);
    if (id > 0) eng->unload(static_cast<uint64_t>(id));
    return JS_UNDEFINED;
}

// mb.exit() — unload the calling script instance via a zero-delay timer
static JSValue jsMbExit(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);
    auto* inst = instanceFromCtx(ctx);
    if (!inst) return JS_UNDEFINED;
    if (inst->builtIn) return JS_ThrowTypeError(ctx, "built-in scripts cannot call exit()");

    InstanceId id = inst->id;
    if (eng->loop()) {
        eng->loop()->addTimer(0, false, [eng, id]() { eng->unload(id); });
    } else {
        eng->unload(id);
    }

    return JS_UNDEFINED;
}

// mb.quit() — quit the application. Distinct from mb.exit() (which only
// unloads the calling script instance). Used by the default UI controller
// when the last terminal has exited.
static JSValue jsMbQuit(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);
    if (eng && eng->callbacks().quit) eng->callbacks().quit();
    return JS_UNDEFINED;
}

// mb.setNamespace(name) — claim a namespace for this script instance
static JSValue jsMbSetNamespace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "setNamespace requires a non-empty string argument");

    const char* ns = JS_ToCString(ctx, argv[0]);
    if (!ns) return JS_EXCEPTION;
    std::string nsStr(ns);
    JS_FreeCString(ctx, ns);

    if (nsStr.empty())
        return JS_ThrowTypeError(ctx, "namespace must not be empty");

    Engine* eng = engineFromCtx(ctx);
    auto* inst = instanceFromCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "no script instance");

    if (!eng->setNamespace(inst->id, nsStr))
        return JS_ThrowTypeError(ctx, "namespace '%s' is already taken or already set", nsStr.c_str());

    return JS_UNDEFINED;
}

// mb.registerAction(name) — register a script action as "namespace.name"
static JSValue jsMbRegisterAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    REQUIRE_PERM(ctx, ActionsInvoke);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "registerAction requires a string argument");

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    std::string nameStr(name);
    JS_FreeCString(ctx, name);

    Engine* eng = engineFromCtx(ctx);
    auto* inst = instanceFromCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "no script instance");

    if (!eng->registerAction(inst->id, nameStr))
        return JS_ThrowTypeError(ctx, "registerAction failed: namespace not set or action already registered");

    return JS_UNDEFINED;
}

// ============================================================================
// Microtask event dispatch
// ============================================================================

static JSValue eventJobFunc(JSContext* ctx, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, argc - 1, argv + 1);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        sLog().error("ScriptEngine: event handler error: {}", s ? s : "(null)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);
    return JS_UNDEFINED;
}

static void enqueueListeners(JSContext* ctx, JSValue arr, int extraArgc, JSValue* extraArgv)
{
    if (JS_IsUndefined(arr)) return;
    JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

    for (int32_t i = 0; i < len; ++i) {
        JSValue fn = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsFunction(ctx, fn)) {
            std::vector<JSValue> jobArgs;
            jobArgs.push_back(fn);
            for (int j = 0; j < extraArgc; ++j)
                jobArgs.push_back(extraArgv[j]);
            JS_EnqueueJob(ctx, eventJobFunc,
                          static_cast<int>(jobArgs.size()), jobArgs.data());
        }
        JS_FreeValue(ctx, fn);
    }
}

// ============================================================================
// ============================================================================
// setTimeout / setInterval / clearTimeout / clearInterval
// ============================================================================

// Fire a JS timer by jsId. For one-shot timers, removes the entry after firing.
static void fireJsTimer(Engine* eng, uint32_t jsId)
{
    auto& timers = eng->jsTimers();
    auto it = timers.find(jsId);
    if (it == timers.end()) return;

    JSContext* ctx = it->second.ctx;
    JSValue cb     = it->second.callback;
    bool interval  = it->second.interval;

    JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        sLog().error("ScriptEngine: timer error: {}", s ? s : "(null)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);

    if (!interval) {
        JS_FreeValue(ctx, cb);
        timers.erase(jsId);
    }
}

// setTimeout(fn, ms) -> timerId
static JSValue jsSetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    if (!eng->loop()) return JS_UNDEFINED;

    int64_t ms = 0;
    if (argc >= 2) JS_ToInt64(ctx, &ms, argv[1]);
    if (ms < 0) ms = 0;

    uint32_t jsId = eng->nextTimer();
    EventLoop::TimerId loopId = eng->loop()->addTimer(
        static_cast<uint64_t>(ms), false,
        [eng, jsId]() { fireJsTimer(eng, jsId); });

    auto& t = eng->jsTimers()[jsId];
    t.ctx = ctx; t.callback = JS_DupValue(ctx, argv[0]);
    t.loopId = loopId; t.interval = false; t.ms = static_cast<uint64_t>(ms);
    return JS_NewUint32(ctx, jsId);
}

// setInterval(fn, ms) -> timerId
static JSValue jsSetInterval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    if (!eng->loop()) return JS_UNDEFINED;

    int64_t ms = 0;
    JS_ToInt64(ctx, &ms, argv[1]);
    if (ms < 1) ms = 1;

    uint32_t jsId = eng->nextTimer();
    EventLoop::TimerId loopId = eng->loop()->addTimer(
        static_cast<uint64_t>(ms), true,
        [eng, jsId]() { fireJsTimer(eng, jsId); });

    auto& t = eng->jsTimers()[jsId];
    t.ctx = ctx; t.callback = JS_DupValue(ctx, argv[0]);
    t.loopId = loopId; t.interval = true; t.ms = static_cast<uint64_t>(ms);
    return JS_NewUint32(ctx, jsId);
}

// clearTimeout(id) / clearInterval(id) — same implementation
static JSValue jsClearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    if (!eng->loop()) return JS_UNDEFINED;

    uint32_t jsId;
    JS_ToUint32(ctx, &jsId, argv[0]);

    auto& timers = eng->jsTimers();
    auto it = timers.find(jsId);
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

static std::string jsConsoleMsg(JSContext* ctx, int argc, JSValueConst* argv)
{
    std::string msg;
    if (sJsIndent > 0) msg.append(static_cast<size_t>(sJsIndent * 2), ' ');
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += ' ';
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { msg += s; JS_FreeCString(ctx, s); }
    }
    return msg;
}

static JSValue jsConsoleGroup(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc > 0)
        jsLogger()->info("{}", jsConsoleMsg(ctx, argc, argv));
    ++sJsIndent;
    return JS_UNDEFINED;
}

static JSValue jsConsoleGroupEnd(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (sJsIndent > 0) --sJsIndent;
    return JS_UNDEFINED;
}

static JSValue jsConsoleTrace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    jsLogger()->trace("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleDebug(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    jsLogger()->debug("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleInfo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    jsLogger()->info("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    jsLogger()->info("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleWarn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    jsLogger()->warn("{}", jsConsoleMsg(ctx, argc, argv));
    return JS_UNDEFINED;
}

static JSValue jsConsoleError(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
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

    rt_ = JS_NewRuntime();
    JS_SetRuntimeOpaque(rt_, this);

    JS_SetModuleLoaderFunc(rt_, moduleNormalize, moduleLoader, this);

    JS_NewClassID(rt_, &jsTerminalClassId);
    JS_NewClass(rt_, jsTerminalClassId, &jsTerminalClassDef);

    JS_NewClassID(rt_, &jsPaneClassId);
    JS_NewClass(rt_, jsPaneClassId, &jsPaneClassDef);

    JS_NewClassID(rt_, &jsTabClassId);
    JS_NewClass(rt_, jsTabClassId, &jsTabClassDef);

    JS_NewClassID(rt_, &jsPopupClassId);
    JS_NewClass(rt_, jsPopupClassId, &jsPopupClassDef);

    JS_NewClassID(rt_, &jsEmbeddedClassId);
    JS_NewClass(rt_, jsEmbeddedClassId, &jsEmbeddedClassDef);

    JS_NewClassID(rt_, &jsCommandClassId);
    JS_NewClass(rt_, jsCommandClassId, &jsCommandClassDef);
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
    for (const auto& inst : instances_) ids.push_back(inst.id);
    for (InstanceId id : ids) unload(id);
    // Any remaining entries (dead contexts flagged during iteration) are
    // already freed; just drop the vector slots. Timer callbacks not
    // attached to an instance, plus global action-handler/timer state, get
    // swept here as a belt-and-braces step.
    for (auto& [_, t] : jsTimers_) {
        if (loop_) loop_->removeTimer(t.loopId);
        JS_FreeValue(t.ctx, t.callback);
    }
    jsTimers_.clear();
    for (auto& [_, h] : actionHandlers_) {
        JS_FreeValue(h.ctx, h.fn);
    }
    actionHandlers_.clear();
    for (auto& inst : instances_) {
        if (inst.ctx) JS_FreeContext(inst.ctx);
    }
    if (rt_) JS_FreeRuntime(rt_);
}

// Engine::terminal / insertTerminal / extractTerminal are defined inline in
// the header so Layout.cpp (which tests link directly without pulling in
// ScriptEngine.cpp's heavy deps on QuickJS / libwebsockets) can resolve the
// symbols. Destruction of terminals_ is still anchored here via the
// out-of-line ~Engine() since ScriptEngine.cpp includes Terminal.h, so
// unique_ptr<Terminal>::~unique_ptr instantiates against the complete type.

void Engine::setCallbacks(AppCallbacks cbs) { callbacks_ = std::move(cbs); }

JSContext* Engine::createContext()
{
    JSContext* ctx = JS_NewContext(rt_);

    // Terminal base prototype — shared across Pane / Popup / EmbeddedTerminal.
    // Methods on the base (`inject`, `cols`, `rows`, `cursor`, `kind`) resolve
    // the backing TerminalEmulator via a class-id-branching resolver, so
    // applets don't need to know or care which concrete kind they're holding.
    JSValue terminalProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, terminalProto,
        jsTerminalProto, sizeof(jsTerminalProto) / sizeof(jsTerminalProto[0]));
    JS_SetClassProto(ctx, jsTerminalClassId, terminalProto);

    // Expose `Terminal` on the global so `x instanceof Terminal` works.
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "Terminal", JS_DupValue(ctx, terminalProto));
        JS_FreeValue(ctx, global);
    }

    auto makeSubProto = [&](const JSCFunctionListEntry* entries, size_t count) {
        JSValue p = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, p, entries, static_cast<int>(count));
        // Chain to Terminal.prototype so instances inherit base methods.
        JS_SetPrototype(ctx, p, terminalProto);
        return p;
    };

    JSValue paneProto = makeSubProto(jsPaneProto, sizeof(jsPaneProto) / sizeof(jsPaneProto[0]));
    JS_SetClassProto(ctx, jsPaneClassId, paneProto);

    JSValue tabProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, tabProto,
        jsTabProto, sizeof(jsTabProto) / sizeof(jsTabProto[0]));
    JS_SetClassProto(ctx, jsTabClassId, tabProto);

    JSValue popupProto = makeSubProto(jsPopupProto, sizeof(jsPopupProto) / sizeof(jsPopupProto[0]));
    JS_SetClassProto(ctx, jsPopupClassId, popupProto);

    JSValue embeddedProto = makeSubProto(jsEmbeddedProto, sizeof(jsEmbeddedProto) / sizeof(jsEmbeddedProto[0]));
    JS_SetClassProto(ctx, jsEmbeddedClassId, embeddedProto);

    JSValue commandProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, commandProto, jsCommandProto, jsCommandProtoCount);
    JS_SetClassProto(ctx, jsCommandClassId, commandProto);

    // Timer globals
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, jsSetTimeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, jsSetInterval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, jsClearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, jsClearTimer, "clearInterval", 1));

    // console object
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "group",
        JS_NewCFunction(ctx, jsConsoleGroup, "group", 0));
    JS_SetPropertyStr(ctx, console, "groupEnd",
        JS_NewCFunction(ctx, jsConsoleGroupEnd, "groupEnd", 0));
    JS_SetPropertyStr(ctx, console, "groupCollapsed",
        JS_NewCFunction(ctx, jsConsoleGroup, "groupCollapsed", 0)); // same as group
    JS_SetPropertyStr(ctx, console, "trace",
        JS_NewCFunction(ctx, jsConsoleTrace, "trace", 0));
    JS_SetPropertyStr(ctx, console, "debug",
        JS_NewCFunction(ctx, jsConsoleDebug, "debug", 0));
    JS_SetPropertyStr(ctx, console, "info",
        JS_NewCFunction(ctx, jsConsoleInfo, "info", 0));
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, jsConsoleLog, "log", 0));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, jsConsoleWarn, "warn", 0));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, jsConsoleError, "error", 0));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);

    return ctx;
}

static JSValue jsMbRegisterTcap(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue jsMbUnregisterTcap(JSContext*, JSValueConst, int, JSValueConst*);

// mb.tabBarPosition — "top" | "bottom" (from the [tab_bar] config section).
// Read by default-ui.js to order the root Container's children.
static JSValue jsMbTabBarPosition(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    auto& cb = engineFromCtx(ctx)->callbacks().tabBarPosition;
    std::string pos = cb ? cb() : std::string("bottom");
    return JS_NewStringLen(ctx, pos.data(), pos.size());
}

// mb.pane(nodeId) -> Pane | null. Construct a Pane object wrapping the
// terminal at `nodeId`. Returns null when the UUID is malformed or doesn't
// refer to a live Terminal in the engine's terminal map.
static JSValue jsMbPane(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_NULL;
    Uuid u = Uuid::fromString(std::string_view(s, len));
    JS_FreeCString(ctx, s);
    if (u.isNil()) return JS_NULL;
    Engine* eng = engineFromCtx(ctx);
    if (!eng->terminal(u)) return JS_NULL;
    return jsPaneNew(ctx, u);
}

// mb.getClipboard(source?) -> string.  source = "clipboard" | "primary" (default "clipboard")
static JSValue jsMbGetClipboard(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    REQUIRE_PERM(ctx, ClipboardRead);
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().getClipboard) return JS_NewString(ctx, "");
    std::string source = "clipboard";
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { source = s; JS_FreeCString(ctx, s); }
    }
    auto text = eng->callbacks().getClipboard(source);
    return JS_NewStringLen(ctx, text.data(), text.size());
}

// mb.setClipboard(text, source?).  source = "clipboard" | "primary" (default "clipboard")
static JSValue jsMbSetClipboard(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "setClipboard requires (text, source?)");
    REQUIRE_PERM(ctx, ClipboardWrite);
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().setClipboard) return JS_UNDEFINED;
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    std::string source = "clipboard";
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) { source = s; JS_FreeCString(ctx, s); }
    }
    eng->callbacks().setClipboard(source, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

void Engine::setupGlobals(JSContext* ctx, InstanceId id)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mb, "invokeAction",
        JS_NewCFunction(ctx, jsMbInvokeAction, "invokeAction", 1));
    JS_SetPropertyStr(ctx, mb, "addEventListener",
        JS_NewCFunction(ctx, jsMbAddEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, mb, "removeEventListener",
        JS_NewCFunction(ctx, jsMbRemoveEventListener, "removeEventListener", 2));
    // Getter properties on mb object
    auto defineGetter = [&](const char* name, JSCFunction* getter) {
        JSAtom atom = JS_NewAtom(ctx, name);
        JS_DefinePropertyGetSet(ctx, mb, atom,
            JS_NewCFunction(ctx, getter, name, 0), JS_UNDEFINED,
            JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, atom);
    };
    defineGetter("tabs", jsMbGetTabs);
    defineGetter("activePane", jsMbGetActivePane);
    defineGetter("actions", jsMbGetActions);
    defineGetter("tabBarPosition", jsMbTabBarPosition);
    JS_SetPropertyStr(ctx, mb, "pane",
        JS_NewCFunction(ctx, jsMbPane, "pane", 1));
    JS_SetPropertyStr(ctx, mb, "unloadScript",
        JS_NewCFunction(ctx, jsMbUnloadScript, "unloadScript", 1));
    JS_SetPropertyStr(ctx, mb, "loadScript",
        JS_NewCFunction(ctx, jsMbLoadScript, "loadScript", 2));
    JS_SetPropertyStr(ctx, mb, "createSecureToken",
        JS_NewCFunction(ctx, jsMbCreateSecureToken, "createSecureToken", 1));
    JS_SetPropertyStr(ctx, mb, "createUuid",
        JS_NewCFunction(ctx, jsMbCreateUuid, "createUuid", 0));
    JS_SetPropertyStr(ctx, mb, "approveScript",
        JS_NewCFunction(ctx, jsMbApproveScript, "approveScript", 2));
    JS_SetPropertyStr(ctx, mb, "setNamespace",
        JS_NewCFunction(ctx, jsMbSetNamespace, "setNamespace", 1));
    JS_SetPropertyStr(ctx, mb, "registerAction",
        JS_NewCFunction(ctx, jsMbRegisterAction, "registerAction", 1));
    JS_SetPropertyStr(ctx, mb, "exit",
        JS_NewCFunction(ctx, jsMbExit, "exit", 0));
    JS_SetPropertyStr(ctx, mb, "quit",
        JS_NewCFunction(ctx, jsMbQuit, "quit", 0));
    JS_SetPropertyStr(ctx, mb, "registerTcap",
        JS_NewCFunction(ctx, jsMbRegisterTcap, "registerTcap", 2));
    JS_SetPropertyStr(ctx, mb, "unregisterTcap",
        JS_NewCFunction(ctx, jsMbUnregisterTcap, "unregisterTcap", 1));
    JS_SetPropertyStr(ctx, mb, "getClipboard",
        JS_NewCFunction(ctx, jsMbGetClipboard, "getClipboard", 0));
    JS_SetPropertyStr(ctx, mb, "setClipboard",
        JS_NewCFunction(ctx, jsMbSetClipboard, "setClipboard", 1));

    installLayoutBindings(*this, ctx, mb);

    JS_SetPropertyStr(ctx, global, "mb", mb);
    JS_FreeValue(ctx, global);
}

InstanceId Engine::loadController(const std::string& path) {
    std::string src = io::readFile(path);
    if (src.empty()) {
        sLog().error("ScriptEngine: failed to read '{}'", path);
        return 0;
    }
    JSContext* ctx = createContext();
    InstanceId id = nextId_++;
    setupGlobals(ctx, id);

    instances_.push_back({id, ctx, path, /*contentHash=*/"", Perm::All, /*builtIn=*/true});
    JS_SetContextOpaque(ctx, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));

    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), path.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        sLog().error("ScriptEngine: '{}' error: {}", path, str ? str : "(null)");
        if (str) JS_FreeCString(ctx, str);
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

void Engine::unload(InstanceId id)
{
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
        if (it->id != id) continue;

        JSContext* ctx = it->ctx;
        sLog().info("ScriptEngine: unloading '{}' (id={})", it->path, id);

        // 1. Kill all timers belonging to this context
        {
            std::vector<uint32_t> toRemove;
            for (auto& [jsId, t] : jsTimers_) {
                if (t.ctx == ctx) toRemove.push_back(jsId);
            }
            for (uint32_t jsId : toRemove) {
                auto it = jsTimers_.find(jsId);
                if (it == jsTimers_.end()) continue;
                if (loop_) loop_->removeTimer(it->second.loopId);
                JS_FreeValue(it->second.ctx, it->second.callback);
                jsTimers_.erase(it);
            }
        }

        // 2. Destroy owned popups
        for (auto& ref : it->ownedPopups)
            callbacks_.destroyPopup(ref.pane, ref.popupId);
        // 2b. Destroy owned embedded terminals
        for (auto& ref : it->ownedEmbeddeds)
            if (callbacks_.destroyEmbedded) callbacks_.destroyEmbedded(ref.pane, ref.lineId);

        // 3. Decrement filter counts for this instance's registrations
        for (auto pane : it->paneOutputFilters) {
            auto fc = paneOutputFilterCount_.find(pane);
            if (fc != paneOutputFilterCount_.end() && --fc->second <= 0)
                paneOutputFilterCount_.erase(fc);
        }
        for (auto pane : it->paneInputFilters) {
            auto fc = paneInputFilterCount_.find(pane);
            if (fc != paneInputFilterCount_.end() && --fc->second <= 0)
                paneInputFilterCount_.erase(fc);
        }
        for (auto pane : it->paneMouseMoveListeners) {
            auto fc = paneMouseMoveCount_.find(pane);
            if (fc != paneMouseMoveCount_.end() && --fc->second <= 0)
                paneMouseMoveCount_.erase(fc);
        }

        // 5. Remove registered actions for this instance's namespace
        if (!it->ns.empty()) {
            std::string prefix = it->ns + ".";
            for (auto ait = registeredActions_.begin(); ait != registeredActions_.end(); ) {
                if (ait->substr(0, prefix.size()) == prefix)
                    ait = registeredActions_.erase(ait);
                else
                    ++ait;
            }
        }

        // 6. Drop any action handlers registered by this instance.
        for (auto ahit = actionHandlers_.begin(); ahit != actionHandlers_.end(); ) {
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

void Engine::setConfigDir(const std::string& dir) {
    configDir_ = dir;
    allowlist_.load(dir);
}

// Collect {path, sha256} for every .js file in the directory tree of scriptPath,
// sorted for deterministic comparison.
static std::vector<std::pair<std::string, std::string>>
collectDirModules(const std::string& scriptPath)
{
    std::vector<std::pair<std::string, std::string>> result;
    fs::path dir = fs::path(scriptPath).parent_path();
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        if (entry.path().extension() != ".js") continue;
        std::string p = entry.path().string();
        std::string content = io::readFile(p);
        if (!content.empty())
            result.emplace_back(std::move(p), sha256Hex(content));
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Returns true if the modules stored in the allowlist entry match the current
// state of the script's directory (same set of files, same hashes).
static bool verifyModuleHashes(const Allowlist::AllowEntry& entry)
{
    if (entry.modules.empty()) return true; // no modules recorded — allow (old entry)
    auto current = collectDirModules(entry.path);
    return current == entry.modules;
}

Engine::LoadResult Engine::loadScript(const std::string& path, uint32_t requestedPerms) {
    std::string content = io::readFile(path);
    if (content.empty()) {
        sLog().error("ScriptEngine: failed to read '{}'", path);
        return { LoadResult::Status::Error, 0, "failed to read '" + path + "'" };
    }

    std::string hash = sha256Hex(content);

    if (allowlist_.isDenied(path, hash)) {
        sLog().info("ScriptEngine: script '{}' is permanently denied", path);
        return { LoadResult::Status::Denied, 0, {} };
    }

    const auto* entry = allowlist_.check(path, hash);
    if (entry) {
        if ((requestedPerms & ~entry->permissions) == 0) {
            if (verifyModuleHashes(*entry)) {
                InstanceId id = loadScriptInternal(path, content, requestedPerms);
                if (id == 0)
                    return { LoadResult::Status::Error, 0, "script evaluation failed" };
                return { LoadResult::Status::Loaded, id, {} };
            }
            sLog().info("ScriptEngine: module files changed for '{}', re-prompting", path);
        }
        // Requesting new perms beyond what was granted, or modules changed — re-prompt
    }

    // Store pending script and notify JS to show permission prompt
    std::string pendingKey = path; // keyed by path
    pendingScripts_[pendingKey] = {path, content, hash, requestedPerms, "", Uuid{}};

    // Fire scriptPermissionRequired event on mb
    notifyPermissionRequired(path, permissionsToString(requestedPerms), hash);
    return { LoadResult::Status::Pending, 0, {} };
}

InstanceId Engine::loadScriptInternal(const std::string& path, const std::string& content,
                                       uint32_t permissions) {
    std::string hash = sha256Hex(content);

    // Idempotent reload: if an existing instance has identical path, content hash,
    // and permissions, return its id without unloading/reloading. Avoids pointless
    // churn (owned resources, registered handlers, WS servers, etc.) when the same
    // script is loaded repeatedly — e.g. applet-loader handling repeated OSC 58237
    // triggers from multiple shells.
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        if (!inst.builtIn && inst.path == path
            && inst.contentHash == hash && inst.permissions == permissions) {
            sLog().info("ScriptEngine: identical reload of '{}' (id={}), no-op", path, inst.id);
            return inst.id;
        }
    }

    // Unload any existing instance with the same path (content or perms differ)
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        if (inst.path == path && !inst.builtIn) {
            sLog().info("ScriptEngine: replacing existing instance of '{}'", path);
            unload(inst.id);
            break;
        }
    }

    JSContext* ctx = createContext();
    InstanceId id = nextId_++;
    setupGlobals(ctx, id);

    instances_.push_back({id, ctx, path, hash, permissions, false});
    JS_SetContextOpaque(ctx, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));

    JSValue result = JS_Eval(ctx, content.c_str(), content.size(), path.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        sLog().error("ScriptEngine: '{}' error: {}", path, str ? str : "(null)");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        instances_.pop_back();
        return 0;
    }
    JS_FreeValue(ctx, result);
    sLog().info("ScriptEngine: loaded script '{}' (id={}, perms={})", path, id, permissionsToString(permissions));
    return id;
}

Engine::LoadResult Engine::approveScript(const std::string& path, char response) {
    auto it = pendingScripts_.find(path);
    if (it == pendingScripts_.end()) {
        sLog().warn("ScriptEngine: no pending script for '{}'", path);
        return { LoadResult::Status::Error, 0, "no pending script for '" + path + "'" };
    }

    PendingScript pending = std::move(it->second);
    pendingScripts_.erase(it);

    auto tryLoad = [&]() -> LoadResult {
        InstanceId id = loadScriptInternal(pending.path, pending.content, pending.requestedPerms);
        if (id == 0)
            return { LoadResult::Status::Error, 0, "script evaluation failed" };
        return { LoadResult::Status::Loaded, id, {} };
    };

    switch (response) {
    case 'y': case 'Y':
        return tryLoad();
    case 'a': case 'A': {
        auto modules = collectDirModules(pending.path);
        allowlist_.allow(pending.path, pending.hash, pending.requestedPerms, modules);
        allowlist_.save();
        return tryLoad();
    }
    case 'd': case 'D':
        allowlist_.deny(pending.path, pending.hash);
        allowlist_.save();
        sLog().info("ScriptEngine: permanently denied '{}'", pending.path);
        return { LoadResult::Status::Denied, 0, {} };
    case 'n': case 'N':
    default:
        sLog().info("ScriptEngine: denied '{}' (one-time)", pending.path);
        return { LoadResult::Status::Denied, 0, {} };
    }
}

// ============================================================================
// Custom XTGETTCAP

std::optional<std::string> Engine::lookupCustomTcap(const std::string& name) const
{
    auto it = customTcaps_.find(name);
    if (it != customTcaps_.end())
        return it->second;
    return std::nullopt;
}

void Engine::registerTcap(const std::string& name, const std::string& value)
{
    customTcaps_[name] = value;
}

void Engine::unregisterTcap(const std::string& name)
{
    customTcaps_.erase(name);
}

static JSValue jsMbRegisterTcap(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    REQUIRE_PERM(ctx, ActionsInvoke);
    if (argc < 2 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "registerTcap requires (name, value)");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    std::string value;
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char* v = JS_ToCString(ctx, argv[1]);
        if (v) { value = v; JS_FreeCString(ctx, v); }
    }
    engineFromCtx(ctx)->registerTcap(name, value);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue jsMbUnregisterTcap(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    REQUIRE_PERM(ctx, ActionsInvoke);
    if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "unregisterTcap requires (name)");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    engineFromCtx(ctx)->unregisterTcap(name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue jsMbActionsRegister(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    REQUIRE_PERM(ctx, LayoutModify);
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "mb.actions.register(name, fn)");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    auto* inst = instanceFromCtx(ctx);
    if (!inst) { JS_FreeCString(ctx, name); return JS_ThrowTypeError(ctx, "no instance"); }
    engineFromCtx(ctx)->registerActionHandler(inst->id, name, argv[1]);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue jsMbActionsUnregister(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    REQUIRE_PERM(ctx, LayoutModify);
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "mb.actions.unregister(name)");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
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

bool Engine::filterPaneOutput(PaneId pane, std::string& data)
{
    return runPaneFilters(pane, "__output_filters", data);
}

bool Engine::filterPaneInput(PaneId pane, std::string& data)
{
    return runPaneFilters(pane, "__input_filters", data);
}

// Run filters on all Pane JS objects with matching id across controller contexts.
// Pane objects are found via a __pane_registry global keyed by id.
bool Engine::runPaneFilters(PaneId pane, const char* filterProp, std::string& data)
{
    IterGuard guard(this);
    bool modified = false;
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        JS_FreeValue(inst.ctx, registry);
        if (JS_IsUndefined(paneObj)) continue;

        JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, filterProp);
        if (!JS_IsUndefined(arr)) {
            JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
            int32_t len = 0;
            JS_ToInt32(inst.ctx, &len, lenVal);
            JS_FreeValue(inst.ctx, lenVal);

            for (int32_t i = 0; i < len; ++i) {
                JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                if (JS_IsFunction(inst.ctx, fn)) {
                    JSValue arg = JS_NewStringLen(inst.ctx, data.c_str(), data.size());
                    JSValue ret = JS_Call(inst.ctx, fn, paneObj, 1, &arg);
                    JS_FreeValue(inst.ctx, arg);

                    if (JS_IsException(ret)) {
                        JSValue exc = JS_GetException(inst.ctx);
                        const char* s = JS_ToCString(inst.ctx, exc);
                        sLog().error("ScriptEngine: filter error: {}", s ? s : "(null)");
                        if (s) JS_FreeCString(inst.ctx, s);
                        JS_FreeValue(inst.ctx, exc);
                    } else if (JS_IsString(ret)) {
                        size_t rlen;
                        const char* rstr = JS_ToCStringLen(inst.ctx, &rlen, ret);
                        if (rstr) { data.assign(rstr, rlen); modified = true; JS_FreeCString(inst.ctx, rstr); }
                    } else if (JS_IsNull(ret)) {
                        data.clear(); modified = true;
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

void Engine::notifyAction(const std::string& actionName)
{
    IterGuard guard(this);
    std::string prop = std::string("__evt_action_") + actionName;
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr = JS_GetPropertyStr(inst.ctx, mb, prop.c_str());
        enqueueListeners(inst.ctx, arr, 0, nullptr);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyPermissionRequired(const std::string& path,
                                       const std::string& permissions,
                                       const std::string& hash)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        if (!inst.builtIn) continue; // only built-in scripts see permission requests

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr = JS_GetPropertyStr(inst.ctx, mb, "__evt_scriptPermissionRequired");
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
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr = JS_GetPropertyStr(inst.ctx, mb, "__evt_paneCreated");
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
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
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
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, "__evt_paneDestroyed");
        std::string ps = pane.toString();
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

void Engine::notifyTabCreated(TabId tab)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr = JS_GetPropertyStr(inst.ctx, mb, "__evt_tabCreated");
        JSValue tabObj = jsTabNew(inst.ctx, tab);
        enqueueListeners(inst.ctx, arr, 1, &tabObj);
        JS_FreeValue(inst.ctx, tabObj);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyTabDestroyed(TabId tab, Uuid nodeId)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, "__evt_tabDestroyed");
        JSValue args[2] = {
            JS_NewInt32(inst.ctx, tab),
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

    cleanupTab(tab);
}

void Engine::notifyTerminalExited(PaneId pane, Uuid nodeId)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb     = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr    = JS_GetPropertyStr(inst.ctx, mb, "__evt_terminalExited");
        JSValue payload = JS_NewObject(inst.ctx);
        std::string ps = pane.toString();
        JS_SetPropertyStr(inst.ctx, payload, "paneId",
            JS_NewStringLen(inst.ctx, ps.data(), ps.size()));
        JS_SetPropertyStr(inst.ctx, payload, "paneNodeId",
            nodeId.isNil() ? JS_NULL
                           : JS_NewStringLen(inst.ctx, nodeId.toString().c_str(), 36));
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
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;


        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_resized");
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

void Engine::notifyOSC(PaneId pane, int oscNum, const std::string& payload)
{
    IterGuard guard(this);
    std::string prop = "__evt_osc:" + std::to_string(oscNum);

    for (auto& inst : instances_) {
        if (!inst.ctx) continue;


        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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
        JSValue mb = JS_GetPropertyStr(inst.ctx, global2, "mb");
        JSValue mbArr = JS_GetPropertyStr(inst.ctx, mb, prop.c_str());
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

void Engine::notifyForegroundProcessChanged(PaneId pane, const std::string& processName)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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

void Engine::notifyFocusedPopupChanged(PaneId pane, const std::string& popupId)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_mousemove");
            if (!JS_IsUndefined(arr)) {
                JSValue ev = JS_NewObject(inst.ctx);
                JS_SetPropertyStr(inst.ctx, ev, "cellX",  JS_NewInt32(inst.ctx, cellX));
                JS_SetPropertyStr(inst.ctx, ev, "cellY",  JS_NewInt32(inst.ctx, cellY));
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

static JSValue jsCmdMakePos(JSContext* ctx, uint64_t rowId, int absRow, int col)
{
    JSValue pos = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pos, "rowId",  JS_NewInt64(ctx, static_cast<int64_t>(rowId)));
    JS_SetPropertyStr(ctx, pos, "absRow", JS_NewInt32(ctx, absRow));
    JS_SetPropertyStr(ctx, pos, "col",    JS_NewInt32(ctx, col));
    return pos;
}

// Magic numbers map to property names in jsCommandProto below.
static JSValue jsCmdGet(JSContext* ctx, JSValueConst this_val, int magic)
{
    auto* d = static_cast<CmdObjData*>(JS_GetOpaque2(ctx, this_val, jsCommandClassId));
    if (!d) return JS_EXCEPTION;
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
                Engine* eng = engineFromCtx(ctx);
                if (eng && eng->callbacks().paneGetText &&
                    d->info.commandStartCol >= 0 && d->info.outputStartLineId) {
                    std::string s = eng->callbacks().paneGetText(
                        d->paneId, d->info.commandStartLineId, d->info.commandStartCol,
                        d->info.outputStartLineId, d->info.outputStartCol);
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' ||
                                          s.back() == '\r' || s.back() == '\t'))
                        s.pop_back();
                    d->cachedCommand = JS_NewStringLen(ctx, s.data(), s.size());
                } else {
                    d->cachedCommand = JS_NewString(ctx, "");
                }
            }
            return JS_DupValue(ctx, d->cachedCommand);
        }
        case 6: { // output text — lazy
            if (JS_IsUninitialized(d->cachedOutput)) {
                Engine* eng = engineFromCtx(ctx);
                if (eng && eng->callbacks().paneGetText &&
                    d->info.outputStartCol >= 0 && d->info.outputEndLineId) {
                    std::string s = eng->callbacks().paneGetText(
                        d->paneId, d->info.outputStartLineId, d->info.outputStartCol,
                        d->info.outputEndLineId, d->info.outputEndCol);
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
        case 7:  return jsCmdMakePos(ctx, d->info.promptStartLineId,  d->info.promptStartAbsRow,  d->info.promptStartCol);
        case 8:  return jsCmdMakePos(ctx, d->info.commandStartLineId, d->info.commandStartAbsRow, d->info.commandStartCol);
        case 9:  return jsCmdMakePos(ctx, d->info.outputStartLineId,  d->info.outputStartAbsRow,  d->info.outputStartCol);
        case 10: return jsCmdMakePos(ctx, d->info.outputEndLineId,    d->info.outputEndAbsRow,    d->info.outputEndCol);
        default: return JS_UNDEFINED;
    }
}

const JSCFunctionListEntry jsCommandProto[] = {
    JS_CGETSET_MAGIC_DEF("id",           jsCmdGet, nullptr,  0),
    JS_CGETSET_MAGIC_DEF("cwd",          jsCmdGet, nullptr,  1),
    JS_CGETSET_MAGIC_DEF("exitCode",     jsCmdGet, nullptr,  2),
    JS_CGETSET_MAGIC_DEF("startMs",      jsCmdGet, nullptr,  3),
    JS_CGETSET_MAGIC_DEF("endMs",        jsCmdGet, nullptr,  4),
    JS_CGETSET_MAGIC_DEF("command",      jsCmdGet, nullptr,  5),
    JS_CGETSET_MAGIC_DEF("output",       jsCmdGet, nullptr,  6),
    JS_CGETSET_MAGIC_DEF("promptStart",  jsCmdGet, nullptr,  7),
    JS_CGETSET_MAGIC_DEF("commandStart", jsCmdGet, nullptr,  8),
    JS_CGETSET_MAGIC_DEF("outputStart",  jsCmdGet, nullptr,  9),
    JS_CGETSET_MAGIC_DEF("outputEnd",    jsCmdGet, nullptr, 10),
};
const size_t jsCommandProtoCount = sizeof(jsCommandProto) / sizeof(jsCommandProto[0]);

static JSValue buildCommandObject(JSContext* ctx, const Script::CommandInfo& p, PaneId paneId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsCommandClassId);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, new CmdObjData{p, paneId, JS_UNINITIALIZED, JS_UNINITIALIZED});
    return obj;
}

void Engine::notifyCommandComplete(PaneId pane, const CommandInfo& rec)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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

void Engine::notifyCommandSelectionChanged(PaneId pane, std::optional<uint64_t> commandId)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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

void Engine::deliverInput(const char* registryName, uint32_t key,
                           const char* data, size_t len)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, registryName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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
                    JSValue exc = JS_GetException(inst.ctx);
                    const char* s = JS_ToCString(inst.ctx, exc);
                    sLog().error("ScriptEngine: input listener error: {}", s ? s : "(null)");
                    if (s) JS_FreeCString(inst.ctx, s);
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

void Engine::deliverPopupInput(const std::string& regKey, const char* data, size_t len)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__popup_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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
                            JSValue exc = JS_GetException(inst.ctx);
                            const char* s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: popup input error: {}", s ? s : "(null)");
                            if (s) JS_FreeCString(inst.ctx, s);
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

void Engine::deliverEmbeddedInput(const std::string& regKey, const char* data, size_t len)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__embedded_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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
                            JSValue exc = JS_GetException(inst.ctx);
                            const char* s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: embedded input error: {}", s ? s : "(null)");
                            if (s) JS_FreeCString(inst.ctx, s);
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

void Engine::deliverEmbeddedDestroyed(const std::string& regKey)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__embedded_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, regKey.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__evt_destroyed");
            if (!JS_IsUndefined(arr)) {
                // Fire listeners with no argument.
                enqueueListeners(inst.ctx, arr, 0, nullptr);
            }
            JS_FreeValue(inst.ctx, arr);

            // Mark JsEmbeddedData as !alive so subsequent method calls throw.
            auto* emData = static_cast<JsEmbeddedData*>(JS_GetOpaque(obj, jsEmbeddedClassId));
            if (emData) emData->alive = false;
        }
        JS_FreeValue(inst.ctx, obj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::deliverPopupMouseEvent(PaneId pane, const std::string& popupId,
                                     const std::string& type, int cellX, int cellY,
                                     int pixelX, int pixelY, int button)
{
    IterGuard guard(this);
    std::string regKey = pane.toString() + ":" + popupId;

    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__popup_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, regKey.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__mouse_listeners");
            if (!JS_IsUndefined(arr)) {
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0;
                JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);

                // Build event object: {type, cellX, cellY, pixelX, pixelY, button}
                JSValue ev = JS_NewObject(inst.ctx);
                JS_SetPropertyStr(inst.ctx, ev, "type", JS_NewString(inst.ctx, type.c_str()));
                JS_SetPropertyStr(inst.ctx, ev, "cellX", JS_NewInt32(inst.ctx, cellX));
                JS_SetPropertyStr(inst.ctx, ev, "cellY", JS_NewInt32(inst.ctx, cellY));
                JS_SetPropertyStr(inst.ctx, ev, "pixelX", JS_NewInt32(inst.ctx, pixelX));
                JS_SetPropertyStr(inst.ctx, ev, "pixelY", JS_NewInt32(inst.ctx, pixelY));
                JS_SetPropertyStr(inst.ctx, ev, "button", JS_NewInt32(inst.ctx, button));

                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &ev);
                        if (JS_IsException(ret)) {
                            JSValue exc = JS_GetException(inst.ctx);
                            const char* s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: popup mouse error: {}", s ? s : "(null)");
                            if (s) JS_FreeCString(inst.ctx, s);
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

void Engine::deliverMouseToRegistry(const char* registryName,
                                     const std::string& key, const std::string& type,
                                     int cellX, int cellY, int pixelX, int pixelY, int button)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, registryName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

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

                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &ev);
                        if (JS_IsException(ret)) {
                            JSValue exc = JS_GetException(inst.ctx);
                            const char* s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: mouse event error: {}", s ? s : "(null)");
                            if (s) JS_FreeCString(inst.ctx, s);
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
                                        const std::string& type,
                                        int cellX, int cellY, int pixelX, int pixelY,
                                        int button)
{
    deliverMouseToRegistry("__embedded_registry",
                           pane.toString() + ":" + std::to_string(lineId),
                           type, cellX, cellY, pixelX, pixelY, button);
}

void Engine::deliverMousemoveToRegistry(const char* regName,
                                         const std::string& key,
                                         int cellX, int cellY, int pixelX, int pixelY)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, regName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, key.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__evt_mousemove");
            if (!JS_IsUndefined(arr)) {
                JSValue ev = JS_NewObject(inst.ctx);
                JS_SetPropertyStr(inst.ctx, ev, "cellX",  JS_NewInt32(inst.ctx, cellX));
                JS_SetPropertyStr(inst.ctx, ev, "cellY",  JS_NewInt32(inst.ctx, cellY));
                JS_SetPropertyStr(inst.ctx, ev, "pixelX", JS_NewInt32(inst.ctx, pixelX));
                JS_SetPropertyStr(inst.ctx, ev, "pixelY", JS_NewInt32(inst.ctx, pixelY));
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0; JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);
                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 1, &ev);
                        if (JS_IsException(ret)) {
                            JSValue exc = JS_GetException(inst.ctx);
                            const char* s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: mousemove error: {}", s ? s : "(null)");
                            if (s) JS_FreeCString(inst.ctx, s);
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

void Engine::deliverPopupMouseMove(PaneId pane, const std::string& popupId,
                                    int cellX, int cellY, int pixelX, int pixelY)
{
    deliverMousemoveToRegistry("__popup_registry",
                               pane.toString() + ":" + popupId,
                               cellX, cellY, pixelX, pixelY);
}

void Engine::deliverEmbeddedMouseMove(PaneId pane, uint64_t lineId,
                                       int cellX, int cellY, int pixelX, int pixelY)
{
    deliverMousemoveToRegistry("__embedded_registry",
                               pane.toString() + ":" + std::to_string(lineId),
                               cellX, cellY, pixelX, pixelY);
}

void Engine::deliverResizedToRegistry(const char* regName, const std::string& key,
                                       int cols, int rows)
{
    IterGuard guard(this);
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, regName);
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;
        JSValue obj = JS_GetPropertyStr(inst.ctx, registry, key.c_str());
        if (!JS_IsUndefined(obj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, obj, "__evt_resized");
            if (!JS_IsUndefined(arr)) {
                JSValue argv[2] = {
                    JS_NewInt32(inst.ctx, cols),
                    JS_NewInt32(inst.ctx, rows),
                };
                JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
                int32_t arrLen = 0; JS_ToInt32(inst.ctx, &arrLen, lenVal);
                JS_FreeValue(inst.ctx, lenVal);
                for (int32_t i = 0; i < arrLen; ++i) {
                    JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                    if (JS_IsFunction(inst.ctx, fn)) {
                        JSValue ret = JS_Call(inst.ctx, fn, obj, 2, argv);
                        if (JS_IsException(ret)) {
                            JSValue exc = JS_GetException(inst.ctx);
                            const char* s = JS_ToCString(inst.ctx, exc);
                            sLog().error("ScriptEngine: resized error: {}", s ? s : "(null)");
                            if (s) JS_FreeCString(inst.ctx, s);
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

void Engine::deliverPopupResized(PaneId pane, const std::string& popupId, int cols, int rows)
{
    deliverResizedToRegistry("__popup_registry",
                             pane.toString() + ":" + popupId, cols, rows);
}

void Engine::deliverEmbeddedResized(PaneId pane, uint64_t lineId, int cols, int rows)
{
    deliverResizedToRegistry("__embedded_registry",
                             pane.toString() + ":" + std::to_string(lineId), cols, rows);
}

void Engine::deliverPaneMouseEvent(PaneId pane, const std::string& type,
                                    int cellX, int cellY, int pixelX, int pixelY, int button)
{
    deliverMouseToRegistry("__pane_registry", pane.toString(),
                                 type, cellX, cellY, pixelX, pixelY, button);
}

void Engine::executePendingJobs()
{
    JSContext* pctx;
    while (JS_ExecutePendingJob(rt_, &pctx) > 0) {}
}

// ============================================================================
// Cleanup
// ============================================================================

void Engine::cleanupPane(PaneId pane)
{
    IterGuard guard(this);
    paneOutputFilterCount_.erase(pane);
    paneInputFilterCount_.erase(pane);

    for (auto& inst : instances_) {
        if (!inst.ctx) continue;


        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyStr(inst.ctx, registry, pane.toString().c_str());
        if (!JS_IsUndefined(paneObj)) {
            // Fire destroyed listeners
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_destroyed");
            enqueueListeners(inst.ctx, arr, 0, nullptr);
            JS_FreeValue(inst.ctx, arr);

            // Mark dead
            auto* data = jsPaneGet(inst.ctx, paneObj);
            if (data) data->alive = false;

            // Remove from registry
            JS_SetPropertyStr(inst.ctx, registry, pane.toString().c_str(), JS_UNDEFINED);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::cleanupTab(TabId tab)
{
    IterGuard guard(this);

    for (auto& inst : instances_) {
        if (!inst.ctx) continue;


        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__tab_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue tabObj = JS_GetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(tab));
        if (!JS_IsUndefined(tabObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, tabObj, "__evt_destroyed");
            enqueueListeners(inst.ctx, arr, 0, nullptr);
            JS_FreeValue(inst.ctx, arr);

            auto* data = jsTabGet(inst.ctx, tabObj);
            if (data) data->alive = false;

            JS_SetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(tab), JS_UNDEFINED);
        }
        JS_FreeValue(inst.ctx, tabObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

// ============================================================================
// Helpers
// ============================================================================

Engine::Instance* Engine::findInstance(InstanceId id)
{
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        if (inst.id == id) return &inst;
    }
    return nullptr;
}

Engine::Instance* Engine::findInstanceByCtx(JSContext* ctx)
{
    for (auto& inst : instances_) {
        if (!inst.ctx) continue;
        if (inst.ctx == ctx) return &inst;
    }
    return nullptr;
}

void Engine::addPaneOutputFilter(PaneId pane, InstanceId instId) {
    paneOutputFilterCount_[pane]++;
    if (auto* inst = findInstance(instId))
        inst->paneOutputFilters.push_back(pane);
}

void Engine::addPaneInputFilter(PaneId pane, InstanceId instId) {
    paneInputFilterCount_[pane]++;
    if (auto* inst = findInstance(instId))
        inst->paneInputFilters.push_back(pane);
}

void Engine::addPaneMouseMoveListener(PaneId pane, InstanceId instId) {
    paneMouseMoveCount_[pane]++;
    if (auto* inst = findInstance(instId))
        inst->paneMouseMoveListeners.push_back(pane);
}

bool Engine::setNamespace(InstanceId id, const std::string& ns)
{
    auto* inst = findInstance(id);
    if (!inst) return false;
    if (!inst->ns.empty()) return false; // already set

    // Check no other instance holds this namespace
    for (auto& other : instances_) {
        if (!other.ctx) continue;
        if (other.id != id && other.ns == ns) return false;
    }

    inst->ns = ns;
    sLog().info("ScriptEngine: instance {} claimed namespace '{}'", id, ns);
    return true;
}

bool Engine::registerAction(InstanceId id, const std::string& name)
{
    auto* inst = findInstance(id);
    if (!inst) return false;
    if (inst->ns.empty()) return false; // no namespace set

    std::string fullName = inst->ns + "." + name;
    if (registeredActions_.count(fullName)) return false; // already registered

    registeredActions_.insert(fullName);
    sLog().info("ScriptEngine: registered action '{}'", fullName);
    return true;
}

bool Engine::isActionRegistered(const std::string& fullName) const
{
    return registeredActions_.count(fullName) > 0;
}

bool Engine::registerActionHandler(InstanceId id, const std::string& name, JSValue fn)
{
    Instance* inst = findInstance(id);
    if (!inst || !inst->ctx) return false;
    auto it = actionHandlers_.find(name);
    if (it != actionHandlers_.end()) {
        JS_FreeValue(it->second.ctx, it->second.fn);
        it->second = ActionHandler{id, inst->ctx, JS_DupValue(inst->ctx, fn)};
    } else {
        actionHandlers_.emplace(name,
            ActionHandler{id, inst->ctx, JS_DupValue(inst->ctx, fn)});
    }
    return true;
}

bool Engine::unregisterActionHandler(const std::string& name)
{
    auto it = actionHandlers_.find(name);
    if (it == actionHandlers_.end()) return false;
    JS_FreeValue(it->second.ctx, it->second.fn);
    actionHandlers_.erase(it);
    return true;
}

bool Engine::invokeActionHandler(const std::string& name,
                                 const std::function<JSValue(JSContext*)>& buildArgs)
{
    auto it = actionHandlers_.find(name);
    if (it == actionHandlers_.end()) return false;
    JSContext* ctx = it->second.ctx;
    JSValue fn = JS_DupValue(ctx, it->second.fn); // protect against re-entry unregister
    JSValue args = buildArgs ? buildArgs(ctx) : JS_UNDEFINED;
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, &args);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        sLog().error("action handler '{}' threw: {}", name, s ? s : "(unknown)");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, args);
    JS_FreeValue(ctx, fn);
    return true;
}

} // namespace Script
