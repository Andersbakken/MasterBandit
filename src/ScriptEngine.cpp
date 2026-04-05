#include "ScriptEngine.h"

#include <quickjs.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace Script {

static Engine* engineFromCtx(JSContext* ctx) {
    return static_cast<Engine*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

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

static JSValue jsPaneAddEventListener(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_UNDEFINED;
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    if (strcmp(event, "output") == 0) {
        prop = "__output_filters";
        eng->addPaneOutputFilter(pane->id);
        registerInGlobal(ctx, "__pane_registry", static_cast<uint32_t>(pane->id), this_val);
    } else if (strcmp(event, "input") == 0) {
        prop = "__input_filters";
        eng->addPaneInputFilter(pane->id);
        registerInGlobal(ctx, "__pane_registry", static_cast<uint32_t>(pane->id), this_val);
    } else {
        prop = std::string("__evt_") + event;
        // Register for lifecycle events too (so destroyed can be found)
        registerInGlobal(ctx, "__pane_registry", static_cast<uint32_t>(pane->id), this_val);
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

static JSValue jsPaneInject(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    engineFromCtx(ctx)->callbacks().injectPaneData(pane->id, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue jsPaneWrite(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
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

static JSValue jsPaneGetProp(JSContext* ctx, JSValueConst this_val, int magic)
{
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    auto info = eng->callbacks().paneInfo(pane->id);
    switch (magic) {
    case 0: return JS_NewInt32(ctx, pane->id);
    case 1: return JS_NewInt32(ctx, info.cols);
    case 2: return JS_NewInt32(ctx, info.rows);
    case 3: return JS_NewString(ctx, info.title.c_str());
    case 4: return JS_NewString(ctx, info.cwd.c_str());
    case 5: return JS_NewBool(ctx, info.hasPty);
    default: return JS_UNDEFINED;
    }
}

static const JSCFunctionListEntry jsPaneProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsPaneAddEventListener),
    JS_CFUNC_DEF("inject", 1, jsPaneInject),
    JS_CFUNC_DEF("write", 1, jsPaneWrite),
    JS_CGETSET_MAGIC_DEF("id", jsPaneGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("cols", jsPaneGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("rows", jsPaneGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("title", jsPaneGetProp, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("cwd", jsPaneGetProp, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("hasPty", jsPaneGetProp, nullptr, 5),
};

// ============================================================================
// Overlay JS class
// ============================================================================

static JSClassID jsOverlayClassId;

struct JsOverlayData {
    TabId tabId;  // overlay is identified by its parent tab
    bool alive;
};

static void jsOverlayFinalize(JSRuntime*, JSValue val)
{
    delete static_cast<JsOverlayData*>(JS_GetOpaque(val, jsOverlayClassId));
}

static JSClassDef jsOverlayClassDef = { "Overlay", jsOverlayFinalize };

static JSValue jsOverlayNew(JSContext* ctx, TabId tabId)
{
    JSValue obj = JS_NewObjectClass(ctx, jsOverlayClassId);
    JS_SetOpaque(obj, new JsOverlayData{tabId, true});
    return obj;
}

static JsOverlayData* jsOverlayGet(JSContext* ctx, JSValueConst val)
{
    return static_cast<JsOverlayData*>(JS_GetOpaque(val, jsOverlayClassId));
}

static JSValue jsOverlayAddEventListener(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_UNDEFINED;
    auto* ov = jsOverlayGet(ctx, this_val);
    if (!ov || !ov->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    if (strcmp(event, "output") == 0) {
        prop = "__output_filters";
        eng->addOverlayOutputFilter(ov->tabId);
        registerInGlobal(ctx, "__overlay_registry", static_cast<uint32_t>(ov->tabId), this_val);
    } else if (strcmp(event, "input") == 0) {
        prop = "__input_filters";
        eng->addOverlayInputFilter(ov->tabId);
        registerInGlobal(ctx, "__overlay_registry", static_cast<uint32_t>(ov->tabId), this_val);
    } else {
        prop = std::string("__evt_") + event;
        registerInGlobal(ctx, "__overlay_registry", static_cast<uint32_t>(ov->tabId), this_val);
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

static JSValue jsOverlayInject(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    auto* ov = jsOverlayGet(ctx, this_val);
    if (!ov || !ov->alive) return JS_UNDEFINED;
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    engineFromCtx(ctx)->callbacks().injectOverlayData(ov->tabId, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue jsOverlayWrite(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    auto* ov = jsOverlayGet(ctx, this_val);
    if (!ov || !ov->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    if (!eng->callbacks().overlayHasPty(ov->tabId))
        return JS_ThrowTypeError(ctx, "overlay has no PTY");
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    eng->callbacks().writeOverlayToShell(ov->tabId, std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue jsOverlayGetProp(JSContext* ctx, JSValueConst this_val, int magic)
{
    auto* ov = jsOverlayGet(ctx, this_val);
    if (!ov || !ov->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    auto info = eng->callbacks().overlayInfo(ov->tabId);
    if (!info.exists) return JS_UNDEFINED;
    switch (magic) {
    case 0: return JS_NewInt32(ctx, info.cols);
    case 1: return JS_NewInt32(ctx, info.rows);
    case 2: return JS_NewBool(ctx, info.hasPty);
    default: return JS_UNDEFINED;
    }
}

static const JSCFunctionListEntry jsOverlayProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsOverlayAddEventListener),
    JS_CFUNC_DEF("inject", 1, jsOverlayInject),
    JS_CFUNC_DEF("write", 1, jsOverlayWrite),
    JS_CGETSET_MAGIC_DEF("cols", jsOverlayGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("rows", jsOverlayGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("hasPty", jsOverlayGetProp, nullptr, 2),
};

// ============================================================================
// Tab JS class
// ============================================================================

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
        return JS_UNDEFINED;
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;

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

// tab.overlay — returns Overlay or undefined
static JSValue jsTabGetOverlay(JSContext* ctx, JSValueConst this_val)
{
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    auto info = eng->callbacks().overlayInfo(tab->id);
    if (!info.exists) return JS_UNDEFINED;
    return jsOverlayNew(ctx, tab->id);
}

static JSValue jsTabGetId(JSContext* ctx, JSValueConst this_val)
{
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab) return JS_UNDEFINED;
    return JS_NewInt32(ctx, tab->id);
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
        if (ti.id == tab->id && ti.focusedPane >= 0)
            return jsPaneNew(ctx, ti.focusedPane);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry jsTabProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsTabAddEventListener),
    JS_CGETSET_DEF("overlay", jsTabGetOverlay, nullptr),
    JS_CGETSET_DEF("id", jsTabGetId, nullptr),
    JS_CGETSET_DEF("panes", jsTabGetPanes, nullptr),
    JS_CGETSET_DEF("activePane", jsTabGetActivePane, nullptr),
};

// ============================================================================
// mb global — controller API
// ============================================================================

static JSValue jsMbInvokeAction(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_FALSE;
    Engine* eng = engineFromCtx(ctx);
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

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
    if (argc < 2 || !JS_IsString(argv[0])) return JS_UNDEFINED;

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    JSValueConst callback;

    if (strcmp(event, "action") == 0) {
        // Three-arg form: ("action", "ActionName", fn)
        if (argc < 3 || !JS_IsString(argv[1]) || !JS_IsFunction(ctx, argv[2])) {
            JS_FreeCString(ctx, event);
            return JS_UNDEFINED;
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
            return JS_UNDEFINED;
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

static JSValue jsMbTabs(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < tabInfos.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, jsTabNew(ctx, tabInfos[i].id));
    return arr;
}

static JSValue jsMbActivePane(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    for (auto& ti : tabInfos)
        if (ti.active && ti.focusedPane >= 0)
            return jsPaneNew(ctx, ti.focusedPane);
    return JS_UNDEFINED;
}

static JSValue jsMbActiveTab(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Engine* eng = engineFromCtx(ctx);
    auto tabInfos = eng->callbacks().tabs();
    for (auto& ti : tabInfos)
        if (ti.active) return jsTabNew(ctx, ti.id);
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
        spdlog::error("ScriptEngine: event handler error: {}", s ? s : "(null)");
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
// console.log / console.warn / console.error
// ============================================================================

static JSValue jsConsoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += ' ';
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { msg += s; JS_FreeCString(ctx, s); }
    }
    spdlog::info("JS: {}", msg);
    return JS_UNDEFINED;
}

static JSValue jsConsoleWarn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += ' ';
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { msg += s; JS_FreeCString(ctx, s); }
    }
    spdlog::warn("JS: {}", msg);
    return JS_UNDEFINED;
}

static JSValue jsConsoleError(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) msg += ' ';
        const char* s = JS_ToCString(ctx, argv[i]);
        if (s) { msg += s; JS_FreeCString(ctx, s); }
    }
    spdlog::error("JS: {}", msg);
    return JS_UNDEFINED;
}

// ============================================================================
// Engine implementation
// ============================================================================

Engine::Engine()
{
    rt_ = JS_NewRuntime();
    JS_SetRuntimeOpaque(rt_, this);

    JS_NewClassID(rt_, &jsPaneClassId);
    JS_NewClass(rt_, jsPaneClassId, &jsPaneClassDef);

    JS_NewClassID(rt_, &jsOverlayClassId);
    JS_NewClass(rt_, jsOverlayClassId, &jsOverlayClassDef);

    JS_NewClassID(rt_, &jsTabClassId);
    JS_NewClass(rt_, jsTabClassId, &jsTabClassDef);
}

Engine::~Engine()
{
    for (auto& inst : instances_)
        JS_FreeContext(inst.ctx);
    if (rt_) JS_FreeRuntime(rt_);
}

void Engine::setCallbacks(AppCallbacks cbs) { callbacks_ = std::move(cbs); }

JSContext* Engine::createContext()
{
    JSContext* ctx = JS_NewContext(rt_);

    JSValue paneProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, paneProto,
        jsPaneProto, sizeof(jsPaneProto) / sizeof(jsPaneProto[0]));
    JS_SetClassProto(ctx, jsPaneClassId, paneProto);

    JSValue overlayProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, overlayProto,
        jsOverlayProto, sizeof(jsOverlayProto) / sizeof(jsOverlayProto[0]));
    JS_SetClassProto(ctx, jsOverlayClassId, overlayProto);

    JSValue tabProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, tabProto,
        jsTabProto, sizeof(jsTabProto) / sizeof(jsTabProto[0]));
    JS_SetClassProto(ctx, jsTabClassId, tabProto);

    // console object
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
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

void Engine::setupAppletGlobals(JSContext* ctx, InstanceId id)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue term = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, term, "inject",
        JS_NewCFunction(ctx, jsPaneInject, "inject", 1));
    JS_SetPropertyStr(ctx, global, "term", term);
    JS_FreeValue(ctx, global);
}

void Engine::setupControllerGlobals(JSContext* ctx, InstanceId id)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mb = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mb, "invokeAction",
        JS_NewCFunction(ctx, jsMbInvokeAction, "invokeAction", 1));
    JS_SetPropertyStr(ctx, mb, "addEventListener",
        JS_NewCFunction(ctx, jsMbAddEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, mb, "tabs",
        JS_NewCFunction(ctx, jsMbTabs, "tabs", 0));
    JS_SetPropertyStr(ctx, mb, "activePane",
        JS_NewCFunction(ctx, jsMbActivePane, "activePane", 0));
    JS_SetPropertyStr(ctx, mb, "activeTab",
        JS_NewCFunction(ctx, jsMbActiveTab, "activeTab", 0));
    JS_SetPropertyStr(ctx, global, "mb", mb);
    JS_FreeValue(ctx, global);
}

InstanceId Engine::loadApplet(const std::string& path)
{
    std::string src = readFile(path);
    if (src.empty()) {
        spdlog::error("ScriptEngine: failed to read applet '{}'", path);
        return 0;
    }
    JSContext* ctx = createContext();
    InstanceId id = nextId_++;
    setupAppletGlobals(ctx, id);

    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), path.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        spdlog::error("ScriptEngine: applet '{}' error: {}", path, str ? str : "(null)");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        return 0;
    }
    JS_FreeValue(ctx, result);
    instances_.push_back({id, ctx, Instance::Type::Applet});
    spdlog::info("ScriptEngine: loaded applet '{}' (id={})", path, id);
    return id;
}

InstanceId Engine::loadController(const std::string& path)
{
    std::string src = readFile(path);
    if (src.empty()) {
        spdlog::error("ScriptEngine: failed to read controller '{}'", path);
        return 0;
    }
    JSContext* ctx = createContext();
    InstanceId id = nextId_++;
    setupControllerGlobals(ctx, id);

    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), path.c_str(), JS_EVAL_TYPE_MODULE);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        spdlog::error("ScriptEngine: controller '{}' error: {}", path, str ? str : "(null)");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        return 0;
    }
    JS_FreeValue(ctx, result);
    instances_.push_back({id, ctx, Instance::Type::Controller});
    spdlog::info("ScriptEngine: loaded controller '{}' (id={})", path, id);
    return id;
}

void Engine::unload(InstanceId id)
{
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
        if (it->id == id) {
            JS_FreeContext(it->ctx);
            instances_.erase(it);
            return;
        }
    }
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

bool Engine::hasOverlayOutputFilters(TabId tab) const
{
    auto it = overlayOutputFilterCount_.find(tab);
    return it != overlayOutputFilterCount_.end() && it->second > 0;
}

bool Engine::hasOverlayInputFilters(TabId tab) const
{
    auto it = overlayInputFilterCount_.find(tab);
    return it != overlayInputFilterCount_.end() && it->second > 0;
}

bool Engine::filterPaneOutput(PaneId pane, std::string& data)
{
    return runPaneFilters(pane, "__output_filters", data);
}

bool Engine::filterPaneInput(PaneId pane, std::string& data)
{
    return runPaneFilters(pane, "__input_filters", data);
}

bool Engine::filterOverlayOutput(TabId tab, std::string& data)
{
    return runOverlayFilters(tab, "__output_filters", data);
}

bool Engine::filterOverlayInput(TabId tab, std::string& data)
{
    return runOverlayFilters(tab, "__input_filters", data);
}

// Run filters on all Pane JS objects with matching id across controller contexts.
// Pane objects are found via a __pane_registry global keyed by id.
bool Engine::runPaneFilters(PaneId pane, const char* filterProp, std::string& data)
{
    bool modified = false;
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(pane));
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
                        spdlog::error("ScriptEngine: filter error: {}", s ? s : "(null)");
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

bool Engine::runOverlayFilters(TabId tab, const char* filterProp, std::string& data)
{
    bool modified = false;
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__overlay_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue overlayObj = JS_GetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(tab));
        JS_FreeValue(inst.ctx, registry);
        if (JS_IsUndefined(overlayObj)) continue;

        JSValue arr = JS_GetPropertyStr(inst.ctx, overlayObj, filterProp);
        if (!JS_IsUndefined(arr)) {
            JSValue lenVal = JS_GetPropertyStr(inst.ctx, arr, "length");
            int32_t len = 0;
            JS_ToInt32(inst.ctx, &len, lenVal);
            JS_FreeValue(inst.ctx, lenVal);

            for (int32_t i = 0; i < len; ++i) {
                JSValue fn = JS_GetPropertyUint32(inst.ctx, arr, i);
                if (JS_IsFunction(inst.ctx, fn)) {
                    JSValue arg = JS_NewStringLen(inst.ctx, data.c_str(), data.size());
                    JSValue ret = JS_Call(inst.ctx, fn, overlayObj, 1, &arg);
                    JS_FreeValue(inst.ctx, arg);

                    if (JS_IsException(ret)) {
                        JSValue exc = JS_GetException(inst.ctx);
                        const char* s = JS_ToCString(inst.ctx, exc);
                        spdlog::error("ScriptEngine: overlay filter error: {}", s ? s : "(null)");
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
        JS_FreeValue(inst.ctx, overlayObj);
    }
    return modified;
}

// ============================================================================
// Async notifications
// ============================================================================

void Engine::notifyAction(const std::string& actionName)
{
    std::string prop = std::string("__evt_action_") + actionName;
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue mb = JS_GetPropertyStr(inst.ctx, global, "mb");
        JSValue arr = JS_GetPropertyStr(inst.ctx, mb, prop.c_str());
        enqueueListeners(inst.ctx, arr, 0, nullptr);
        JS_FreeValue(inst.ctx, arr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global);
    }
}

void Engine::notifyPaneCreated(TabId tab, PaneId pane)
{
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;
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

void Engine::notifyPaneDestroyed(PaneId pane)
{
    cleanupPane(pane);
}

void Engine::notifyTabCreated(TabId tab)
{
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;
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

void Engine::notifyTabDestroyed(TabId tab)
{
    cleanupTab(tab);
}

void Engine::notifyOverlayCreated(TabId tab)
{
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__tab_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue tabObj = JS_GetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(tab));
        if (!JS_IsUndefined(tabObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, tabObj, "__evt_overlayCreated");
            JSValue overlayObj = jsOverlayNew(inst.ctx, tab);
            enqueueListeners(inst.ctx, arr, 1, &overlayObj);
            JS_FreeValue(inst.ctx, overlayObj);
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, tabObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::notifyOverlayDestroyed(TabId tab)
{
    // Fire overlayDestroyed listeners on tab objects
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

        // Fire destroyed on overlay objects
        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue oRegistry = JS_GetPropertyStr(inst.ctx, global, "__overlay_registry");
        if (!JS_IsUndefined(oRegistry)) {
            JSValue overlayObj = JS_GetPropertyUint32(inst.ctx, oRegistry, static_cast<uint32_t>(tab));
            if (!JS_IsUndefined(overlayObj)) {
                JSValue arr = JS_GetPropertyStr(inst.ctx, overlayObj, "__evt_destroyed");
                enqueueListeners(inst.ctx, arr, 0, nullptr);
                JS_FreeValue(inst.ctx, arr);
                auto* data = jsOverlayGet(inst.ctx, overlayObj);
                if (data) data->alive = false;
                JS_SetPropertyUint32(inst.ctx, oRegistry, static_cast<uint32_t>(tab), JS_UNDEFINED);
            }
            JS_FreeValue(inst.ctx, overlayObj);
        }
        JS_FreeValue(inst.ctx, oRegistry);

        // Fire overlayDestroyed on tab objects
        JSValue tRegistry = JS_GetPropertyStr(inst.ctx, global, "__tab_registry");
        if (!JS_IsUndefined(tRegistry)) {
            JSValue tabObj = JS_GetPropertyUint32(inst.ctx, tRegistry, static_cast<uint32_t>(tab));
            if (!JS_IsUndefined(tabObj)) {
                JSValue arr = JS_GetPropertyStr(inst.ctx, tabObj, "__evt_overlayDestroyed");
                enqueueListeners(inst.ctx, arr, 0, nullptr);
                JS_FreeValue(inst.ctx, arr);
            }
            JS_FreeValue(inst.ctx, tabObj);
        }
        JS_FreeValue(inst.ctx, tRegistry);
        JS_FreeValue(inst.ctx, global);
    }

    overlayOutputFilterCount_.erase(tab);
    overlayInputFilterCount_.erase(tab);
}

void Engine::notifyPaneResized(PaneId pane, int cols, int rows)
{
    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(pane));
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
    std::string prop = "__evt_osc:" + std::to_string(oscNum);

    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(pane));
        if (!JS_IsUndefined(paneObj)) {
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, prop.c_str());
            if (!JS_IsUndefined(arr)) {
                JSValue arg = JS_NewStringLen(inst.ctx, payload.c_str(), payload.size());
                enqueueListeners(inst.ctx, arr, 1, &arg);
                JS_FreeValue(inst.ctx, arg);
            }
            JS_FreeValue(inst.ctx, arr);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);

        // Also check mb-level listeners
        JSValue global2 = JS_GetGlobalObject(inst.ctx);
        JSValue mb = JS_GetPropertyStr(inst.ctx, global2, "mb");
        JSValue mbArr = JS_GetPropertyStr(inst.ctx, mb, prop.c_str());
        if (!JS_IsUndefined(mbArr)) {
            JSValue args[] = {
                JS_NewInt32(inst.ctx, pane),
                JS_NewStringLen(inst.ctx, payload.c_str(), payload.size())
            };
            enqueueListeners(inst.ctx, mbArr, 2, args);
            JS_FreeValue(inst.ctx, args[0]);
            JS_FreeValue(inst.ctx, args[1]);
        }
        JS_FreeValue(inst.ctx, mbArr);
        JS_FreeValue(inst.ctx, mb);
        JS_FreeValue(inst.ctx, global2);
    }
}

void Engine::deliverAppletInput(InstanceId id, const std::string& data)
{
    Instance* inst = findInstance(id);
    if (!inst || inst->type != Instance::Type::Applet) return;

    JSValue global = JS_GetGlobalObject(inst->ctx);
    JSValue onInput = JS_GetPropertyStr(inst->ctx, global, "onInput");
    if (JS_IsFunction(inst->ctx, onInput)) {
        JSValue args[] = { onInput, JS_NewStringLen(inst->ctx, data.c_str(), data.size()) };
        JS_EnqueueJob(inst->ctx, eventJobFunc, 2, args);
        JS_FreeValue(inst->ctx, args[1]);
    }
    JS_FreeValue(inst->ctx, onInput);
    JS_FreeValue(inst->ctx, global);
}

void Engine::deliverAppletResize(InstanceId id, int cols, int rows)
{
    Instance* inst = findInstance(id);
    if (!inst || inst->type != Instance::Type::Applet) return;

    JSValue global = JS_GetGlobalObject(inst->ctx);
    JSValue onResize = JS_GetPropertyStr(inst->ctx, global, "onResize");
    if (JS_IsFunction(inst->ctx, onResize)) {
        JSValue args[] = { onResize, JS_NewInt32(inst->ctx, cols), JS_NewInt32(inst->ctx, rows) };
        JS_EnqueueJob(inst->ctx, eventJobFunc, 3, args);
    }
    JS_FreeValue(inst->ctx, onResize);
    JS_FreeValue(inst->ctx, global);
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
    paneOutputFilterCount_.erase(pane);
    paneInputFilterCount_.erase(pane);

    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

        JSValue global = JS_GetGlobalObject(inst.ctx);
        JSValue registry = JS_GetPropertyStr(inst.ctx, global, "__pane_registry");
        JS_FreeValue(inst.ctx, global);
        if (JS_IsUndefined(registry)) continue;

        JSValue paneObj = JS_GetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(pane));
        if (!JS_IsUndefined(paneObj)) {
            // Fire destroyed listeners
            JSValue arr = JS_GetPropertyStr(inst.ctx, paneObj, "__evt_destroyed");
            enqueueListeners(inst.ctx, arr, 0, nullptr);
            JS_FreeValue(inst.ctx, arr);

            // Mark dead
            auto* data = jsPaneGet(inst.ctx, paneObj);
            if (data) data->alive = false;

            // Remove from registry
            JS_SetPropertyUint32(inst.ctx, registry, static_cast<uint32_t>(pane), JS_UNDEFINED);
        }
        JS_FreeValue(inst.ctx, paneObj);
        JS_FreeValue(inst.ctx, registry);
    }
}

void Engine::cleanupTab(TabId tab)
{
    overlayOutputFilterCount_.erase(tab);
    overlayInputFilterCount_.erase(tab);

    for (auto& inst : instances_) {
        if (inst.type != Instance::Type::Controller) continue;

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
    for (auto& inst : instances_)
        if (inst.id == id) return &inst;
    return nullptr;
}

Engine::Instance* Engine::findInstanceByCtx(JSContext* ctx)
{
    for (auto& inst : instances_)
        if (inst.ctx == ctx) return &inst;
    return nullptr;
}

std::string Engine::readFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace Script
