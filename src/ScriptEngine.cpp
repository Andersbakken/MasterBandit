#include "ScriptEngine.h"

#include <quickjs.h>
#include <spdlog/spdlog.h>
#include <uv.h>

#include <fstream>
#include <sstream>

namespace Script {

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

#define REQUIRE_PERM(ctx, perm) \
    do { if (!checkPerm(ctx, Script::Perm::perm)) \
        return JS_ThrowTypeError(ctx, "permission denied: " #perm " not granted"); \
    } while(0)

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

    auto* inst = instanceFromCtx(ctx);
    InstanceId instId = inst ? inst->id : 0;

    std::string prop;
    if (strcmp(event, "output") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterOutput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterOutput"); }
        prop = "__output_filters";
        eng->addPaneOutputFilter(pane->id, instId);
        registerInGlobal(ctx, "__pane_registry", static_cast<uint32_t>(pane->id), this_val);
    } else if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput"); }
        prop = "__input_filters";
        eng->addPaneInputFilter(pane->id, instId);
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
    REQUIRE_PERM(ctx, IoInject);
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
    REQUIRE_PERM(ctx, ShellWrite);
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
    case 6: return JS_NewBool(ctx, info.focused);
    case 7: return info.focusedPopupId.empty()
                 ? JS_NULL
                 : JS_NewString(ctx, info.focusedPopupId.c_str());
    default: return JS_UNDEFINED;
    }
}

// Forward declarations — defined after Popup class
static JSValue jsPaneCreatePopup(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue jsPaneDestroyPopup(JSContext*, JSValueConst, int, JSValueConst*);
static JSValue jsPaneGetPopups(JSContext*, JSValueConst);

static const JSCFunctionListEntry jsPaneProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsPaneAddEventListener),
    JS_CFUNC_DEF("inject", 1, jsPaneInject),
    JS_CFUNC_DEF("write", 1, jsPaneWrite),
    JS_CFUNC_DEF("createPopup", 1, jsPaneCreatePopup),
    JS_CFUNC_DEF("destroyPopup", 1, jsPaneDestroyPopup),
    JS_CGETSET_MAGIC_DEF("id", jsPaneGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("cols", jsPaneGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("rows", jsPaneGetProp, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("title", jsPaneGetProp, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("cwd", jsPaneGetProp, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("hasPty", jsPaneGetProp, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("focused", jsPaneGetProp, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("focusedPopupId", jsPaneGetProp, nullptr, 7),
    JS_CGETSET_DEF("popups", jsPaneGetPopups, nullptr),
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

// popup.inject(data)
static JSValue jsPopupInject(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    REQUIRE_PERM(ctx, IoInject);
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_UNDEFINED;
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    engineFromCtx(ctx)->callbacks().injectPopupData(popup->paneId, popup->popupId,
                                                     std::string(str, len));
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

// popup.destroy()
static JSValue jsPopupDestroy(JSContext* ctx, JSValueConst this_val,
                               int, JSValueConst*)
{
    REQUIRE_PERM(ctx, UiPopupDestroy);
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_UNDEFINED;
    engineFromCtx(ctx)->callbacks().destroyPopup(popup->paneId, popup->popupId);
    popup->alive = false;
    return JS_UNDEFINED;
}

// popup.addEventListener(event, fn) — stores on the popup object
static JSValue jsPopupAddEventListener(JSContext* ctx, JSValueConst this_val,
                                        int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_UNDEFINED;
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_UNDEFINED;

    const char* event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_EXCEPTION;

    std::string prop;
    if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput"); }
        prop = "__input_filters";
    } else {
        prop = std::string("__evt_") + event;
    }
    JS_FreeCString(ctx, event);

    // Register in popup registry for input delivery
    Engine* eng = engineFromCtx(ctx);
    std::string regKey = std::to_string(popup->paneId) + ":" + popup->popupId;
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

// popup.paneId, popup.id, popup.focused (getters)
static JSValue jsPopupGetProp(JSContext* ctx, JSValueConst this_val, int magic)
{
    auto* popup = jsPopupGet(ctx, this_val);
    if (!popup || !popup->alive) return JS_UNDEFINED;
    switch (magic) {
    case 0: return JS_NewInt32(ctx, popup->paneId);
    case 1: return JS_NewString(ctx, popup->popupId.c_str());
    case 2: {
        Engine* eng = engineFromCtx(ctx);
        auto info = eng->callbacks().paneInfo(popup->paneId);
        return JS_NewBool(ctx, info.focusedPopupId == popup->popupId);
    }
    default: return JS_UNDEFINED;
    }
}

static const JSCFunctionListEntry jsPopupProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsPopupAddEventListener),
    JS_CFUNC_DEF("inject", 1, jsPopupInject),
    JS_CFUNC_DEF("destroy", 0, jsPopupDestroy),
    JS_CGETSET_MAGIC_DEF("paneId", jsPopupGetProp, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("id", jsPopupGetProp, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("focused", jsPopupGetProp, nullptr, 2),
};

// pane.createPopup({id, x, y, w, h}) -> Popup
static JSValue jsPaneCreatePopup(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    REQUIRE_PERM(ctx, UiPopupCreate);
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);

    const char* id = nullptr;
    JSValue idVal = JS_GetPropertyStr(ctx, argv[0], "id");
    if (JS_IsString(idVal)) id = JS_ToCString(ctx, idVal);
    JS_FreeValue(ctx, idVal);
    if (!id) return JS_UNDEFINED;

    int32_t x = 0, y = 0, w = 20, h = 5;
    JSValue v;
    v = JS_GetPropertyStr(ctx, argv[0], "x"); JS_ToInt32(ctx, &x, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "y"); JS_ToInt32(ctx, &y, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "w"); JS_ToInt32(ctx, &w, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "h"); JS_ToInt32(ctx, &h, v); JS_FreeValue(ctx, v);

    std::string popupId(id);
    JS_FreeCString(ctx, id);

    int paneId = pane->id;
    bool ok = eng->callbacks().createPopup(paneId, popupId, x, y, w, h,
        [eng, paneId, popupId](const char* data, size_t len) {
            // Deliver input to popup listeners
            std::string regKey = std::to_string(paneId) + ":" + popupId;
            eng->deliverPopupInput(regKey, data, len);
        });

    if (!ok) return JS_UNDEFINED;

    // Track ownership for cleanup on unload
    auto* inst = instanceFromCtx(ctx);
    if (inst) inst->ownedPopups.push_back({paneId, popupId});

    return jsPopupNew(ctx, paneId, popupId);
}

// pane.destroyPopup(id)
static JSValue jsPaneDestroyPopup(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    REQUIRE_PERM(ctx, UiPopupDestroy);
    auto* pane = jsPaneGet(ctx, this_val);
    if (!pane || !pane->alive) return JS_UNDEFINED;
    const char* id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    engineFromCtx(ctx)->callbacks().destroyPopup(pane->id, std::string(id));
    JS_FreeCString(ctx, id);
    return JS_UNDEFINED;
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

    auto* inst = instanceFromCtx(ctx);
    InstanceId instId = inst ? inst->id : 0;

    std::string prop;
    if (strcmp(event, "output") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterOutput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterOutput"); }
        prop = "__output_filters";
        eng->addOverlayOutputFilter(ov->tabId, instId);
        registerInGlobal(ctx, "__overlay_registry", static_cast<uint32_t>(ov->tabId), this_val);
    } else if (strcmp(event, "input") == 0) {
        if (!checkPerm(ctx, Perm::IoFilterInput)) { JS_FreeCString(ctx, event); return JS_ThrowTypeError(ctx, "permission denied: IoFilterInput"); }
        prop = "__input_filters";
        eng->addOverlayInputFilter(ov->tabId, instId);
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
    REQUIRE_PERM(ctx, IoInject);
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
    REQUIRE_PERM(ctx, ShellWrite);
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

// Forward declaration — defined later after mb functions
static JSValue jsOverlayClose(JSContext* ctx, JSValueConst this_val, int, JSValueConst*);

static const JSCFunctionListEntry jsOverlayProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsOverlayAddEventListener),
    JS_CFUNC_DEF("inject", 1, jsOverlayInject),
    JS_CFUNC_DEF("write", 1, jsOverlayWrite),
    JS_CFUNC_DEF("close", 0, jsOverlayClose),
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

// tab.createOverlay() -> Overlay or undefined
static JSValue jsTabCreateOverlay(JSContext* ctx, JSValueConst this_val,
                                   int, JSValueConst*)
{
    REQUIRE_PERM(ctx, UiOverlayCreate);
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);

    JSValue overlayObj = jsOverlayNew(ctx, tab->id);

    int tabId = tab->id;
    bool ok = eng->callbacks().createOverlay(tabId, [eng, tabId](const char* data, size_t len) {
        eng->deliverInput("__overlay_registry", static_cast<uint32_t>(tabId), data, len);
    });

    if (!ok) {
        JS_FreeValue(ctx, overlayObj);
        return JS_UNDEFINED;
    }
    registerInGlobal(ctx, "__overlay_registry", static_cast<uint32_t>(tab->id), overlayObj);

    // Track ownership for cleanup on unload
    auto* inst = instanceFromCtx(ctx);
    if (inst) inst->ownedOverlays.push_back(tab->id);

    return overlayObj;
}

// tab.closeOverlay()
static JSValue jsTabCloseOverlay(JSContext* ctx, JSValueConst this_val,
                                  int, JSValueConst*)
{
    REQUIRE_PERM(ctx, UiOverlayClose);
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;
    engineFromCtx(ctx)->callbacks().popOverlay(tab->id);
    return JS_UNDEFINED;
}

// tab.close()
static JSValue jsTabClose(JSContext* ctx, JSValueConst this_val,
                           int, JSValueConst*)
{
    REQUIRE_PERM(ctx, TabsClose);
    auto* tab = jsTabGet(ctx, this_val);
    if (!tab || !tab->alive) return JS_UNDEFINED;
    engineFromCtx(ctx)->callbacks().closeTab(tab->id);
    tab->alive = false;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry jsTabProto[] = {
    JS_CFUNC_DEF("addEventListener", 2, jsTabAddEventListener),
    JS_CFUNC_DEF("createOverlay", 0, jsTabCreateOverlay),
    JS_CFUNC_DEF("closeOverlay", 0, jsTabCloseOverlay),
    JS_CFUNC_DEF("close", 0, jsTabClose),
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
    REQUIRE_PERM(ctx, ActionsInvoke);
    Engine* eng = engineFromCtx(ctx);
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    uint32_t extraPerm = actionPermission(std::string(name));
    if (extraPerm && !checkPerm(ctx, extraPerm)) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "permission denied: action requires additional permissions");
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

// mb.loadApplet(path) -> instanceId or 0
static JSValue jsMbLoadApplet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NewInt64(ctx, 0);
    REQUIRE_PERM(ctx, ScriptsLoad);
    Engine* eng = engineFromCtx(ctx);

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    // loadApplet with full permissions for backward compat (built-in callers are trusted)
    uint64_t id = eng->loadScript(std::string(path), Perm::All);
    JS_FreeCString(ctx, path);
    return JS_NewInt64(ctx, static_cast<int64_t>(id));
}

// mb.loadController(path) -> instanceId or 0
static JSValue jsMbLoadController(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NewInt64(ctx, 0);
    REQUIRE_PERM(ctx, ScriptsLoad);
    Engine* eng = engineFromCtx(ctx);

    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    uint64_t id = eng->loadController(std::string(path));
    JS_FreeCString(ctx, path);
    return JS_NewInt64(ctx, static_cast<int64_t>(id));
}

// mb.loadScript(path, permissionsStr) -> instanceId or 0
static JSValue jsMbLoadScript(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NewInt64(ctx, 0);
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
    uint64_t id = eng->loadScript(std::string(path), perms);
    JS_FreeCString(ctx, path);
    return JS_NewInt64(ctx, static_cast<int64_t>(id));
}

// mb.createTab() -> Tab
static JSValue jsMbCreateTab(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    REQUIRE_PERM(ctx, TabsCreate);
    Engine* eng = engineFromCtx(ctx);
    int tabId = eng->callbacks().createTab();
    return jsTabNew(ctx, tabId);
}

// mb.closeTab(tabId?)
static JSValue jsMbCloseTab(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    REQUIRE_PERM(ctx, TabsClose);
    Engine* eng = engineFromCtx(ctx);
    int tabId = -1;
    if (argc >= 1 && JS_IsNumber(argv[0]))
        JS_ToInt32(ctx, &tabId, argv[0]);

    if (tabId < 0) {
        auto tabInfos = eng->callbacks().tabs();
        for (auto& ti : tabInfos)
            if (ti.active) { tabId = ti.id; break; }
    }
    if (tabId >= 0)
        eng->callbacks().closeTab(tabId);
    return JS_UNDEFINED;
}

// mb.unloadScript(id)
static JSValue jsMbUnloadScript(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
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
    if (!inst || inst->builtIn) return JS_UNDEFINED; // built-ins can't self-unload

    InstanceId id = inst->id;
    auto* timer = new uv_timer_t;
    struct ExitData { Engine* eng; InstanceId id; };
    timer->data = new ExitData{eng, id};
    uv_timer_init(eng->loop(), timer);
    uv_timer_start(timer, [](uv_timer_t* handle) {
        auto* data = static_cast<ExitData*>(handle->data);
        data->eng->unload(data->id);
        delete data;
        uv_timer_stop(handle);
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
            delete reinterpret_cast<uv_timer_t*>(h);
        });
    }, 0, 0);

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
    if (!inst) return JS_UNDEFINED;

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
    if (!inst) return JS_UNDEFINED;

    if (!eng->registerAction(inst->id, nameStr))
        return JS_ThrowTypeError(ctx, "registerAction failed: namespace not set or action already registered");

    return JS_UNDEFINED;
}

// overlay.close() — pops the overlay
static JSValue jsOverlayClose(JSContext* ctx, JSValueConst this_val,
                               int, JSValueConst*)
{
    auto* ov = jsOverlayGet(ctx, this_val);
    if (!ov || !ov->alive) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    eng->callbacks().popOverlay(ov->tabId);
    ov->alive = false;
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
// ============================================================================
// setTimeout / setInterval / clearTimeout / clearInterval
// ============================================================================

struct TimerData {
    JSContext* ctx;
    JSValue callback;
    uint32_t id;
    bool interval;
};

static void timerCallback(uv_timer_t* handle)
{
    auto* td = static_cast<TimerData*>(handle->data);

    JSValue ret = JS_Call(td->ctx, td->callback, JS_UNDEFINED, 0, nullptr);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(td->ctx);
        const char* s = JS_ToCString(td->ctx, exc);
        spdlog::error("ScriptEngine: timer error: {}", s ? s : "(null)");
        if (s) JS_FreeCString(td->ctx, s);
        JS_FreeValue(td->ctx, exc);
    }
    JS_FreeValue(td->ctx, ret);

    if (!td->interval) {
        uv_timer_stop(handle);
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
            auto* td = static_cast<TimerData*>(h->data);
            JS_FreeValue(td->ctx, td->callback);
            delete td;
            delete reinterpret_cast<uv_timer_t*>(h);
        });
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

    uint32_t id = eng->nextTimer();
    auto* td = new TimerData{ctx, JS_DupValue(ctx, argv[0]), id, false};
    auto* timer = new uv_timer_t;
    timer->data = td;
    uv_timer_init(eng->loop(), timer);
    uv_timer_start(timer, timerCallback, static_cast<uint64_t>(ms), 0);

    return JS_NewUint32(ctx, id);
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

    uint32_t id = eng->nextTimer();
    auto* td = new TimerData{ctx, JS_DupValue(ctx, argv[0]), id, true};
    auto* timer = new uv_timer_t;
    timer->data = td;
    uv_timer_init(eng->loop(), timer);
    uv_timer_start(timer, timerCallback, static_cast<uint64_t>(ms), static_cast<uint64_t>(ms));

    return JS_NewUint32(ctx, id);
}

// clearTimeout(id) / clearInterval(id) — same implementation
// Note: this is O(n) over active handles. Fine for reasonable timer counts.
static JSValue jsClearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    Engine* eng = engineFromCtx(ctx);
    if (!eng->loop()) return JS_UNDEFINED;

    uint32_t id;
    JS_ToUint32(ctx, &id, argv[0]);

    // Walk active handles to find the timer
    uv_walk(eng->loop(), [](uv_handle_t* handle, void* arg) {
        if (handle->type != UV_TIMER) return;
        auto* td = static_cast<TimerData*>(handle->data);
        if (!td || td->id != *static_cast<uint32_t*>(arg)) return;
        uv_timer_stop(reinterpret_cast<uv_timer_t*>(handle));
        uv_close(handle, [](uv_handle_t* h) {
            auto* td = static_cast<TimerData*>(h->data);
            JS_FreeValue(td->ctx, td->callback);
            delete td;
            delete reinterpret_cast<uv_timer_t*>(h);
        });
    }, &id);

    return JS_UNDEFINED;
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

    JS_NewClassID(rt_, &jsPopupClassId);
    JS_NewClass(rt_, jsPopupClassId, &jsPopupClassDef);
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

    JSValue popupProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, popupProto,
        jsPopupProto, sizeof(jsPopupProto) / sizeof(jsPopupProto[0]));
    JS_SetClassProto(ctx, jsPopupClassId, popupProto);

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

void Engine::setupGlobals(JSContext* ctx, InstanceId id)
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
    JS_SetPropertyStr(ctx, mb, "loadApplet",
        JS_NewCFunction(ctx, jsMbLoadApplet, "loadApplet", 1));
    JS_SetPropertyStr(ctx, mb, "loadController",
        JS_NewCFunction(ctx, jsMbLoadController, "loadController", 1));
    JS_SetPropertyStr(ctx, mb, "createTab",
        JS_NewCFunction(ctx, jsMbCreateTab, "createTab", 0));
    JS_SetPropertyStr(ctx, mb, "closeTab",
        JS_NewCFunction(ctx, jsMbCloseTab, "closeTab", 0));
    JS_SetPropertyStr(ctx, mb, "unloadScript",
        JS_NewCFunction(ctx, jsMbUnloadScript, "unloadScript", 1));
    JS_SetPropertyStr(ctx, mb, "loadScript",
        JS_NewCFunction(ctx, jsMbLoadScript, "loadScript", 2));
    JS_SetPropertyStr(ctx, mb, "setNamespace",
        JS_NewCFunction(ctx, jsMbSetNamespace, "setNamespace", 1));
    JS_SetPropertyStr(ctx, mb, "registerAction",
        JS_NewCFunction(ctx, jsMbRegisterAction, "registerAction", 1));
    JS_SetPropertyStr(ctx, mb, "exit",
        JS_NewCFunction(ctx, jsMbExit, "exit", 0));
    JS_SetPropertyStr(ctx, global, "mb", mb);
    JS_FreeValue(ctx, global);
}

InstanceId Engine::loadController(const std::string& path) {
    std::string src = readFile(path);
    if (src.empty()) {
        spdlog::error("ScriptEngine: failed to read '{}'", path);
        return 0;
    }
    JSContext* ctx = createContext();
    InstanceId id = nextId_++;
    setupGlobals(ctx, id);

    instances_.push_back({id, ctx, path, /*contentHash=*/"", Perm::All, /*builtIn=*/true});
    JS_SetContextOpaque(ctx, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));

    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), path.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        spdlog::error("ScriptEngine: '{}' error: {}", path, str ? str : "(null)");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        instances_.pop_back();
        return 0;
    }
    JS_FreeValue(ctx, result);
    spdlog::info("ScriptEngine: loaded built-in '{}' (id={})", path, id);
    return id;
}

void Engine::unload(InstanceId id)
{
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
        if (it->id != id) continue;

        JSContext* ctx = it->ctx;
        spdlog::info("ScriptEngine: unloading '{}' (id={})", it->path, id);

        // 1. Kill all timers belonging to this context
        if (loop_) {
            uv_walk(loop_, [](uv_handle_t* handle, void* arg) {
                if (handle->type != UV_TIMER) return;
                auto* td = static_cast<TimerData*>(handle->data);
                if (!td || td->ctx != static_cast<JSContext*>(arg)) return;
                uv_timer_stop(reinterpret_cast<uv_timer_t*>(handle));
                uv_close(handle, [](uv_handle_t* h) {
                    auto* td = static_cast<TimerData*>(h->data);
                    JS_FreeValue(td->ctx, td->callback);
                    delete td;
                    delete reinterpret_cast<uv_timer_t*>(h);
                });
            }, ctx);
        }

        // 2. Destroy owned popups
        for (auto& ref : it->ownedPopups)
            callbacks_.destroyPopup(ref.pane, ref.popupId);

        // 3. Pop owned overlays
        for (auto tabId : it->ownedOverlays)
            callbacks_.popOverlay(tabId);

        // 4. Decrement filter counts for this instance's registrations
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
        for (auto tab : it->overlayOutputFilters) {
            auto fc = overlayOutputFilterCount_.find(tab);
            if (fc != overlayOutputFilterCount_.end() && --fc->second <= 0)
                overlayOutputFilterCount_.erase(fc);
        }
        for (auto tab : it->overlayInputFilters) {
            auto fc = overlayInputFilterCount_.find(tab);
            if (fc != overlayInputFilterCount_.end() && --fc->second <= 0)
                overlayInputFilterCount_.erase(fc);
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

        // 6. Free context and remove instance
        JS_FreeContext(ctx);
        instances_.erase(it);
        return;
    }
}

void Engine::setConfigDir(const std::string& dir) {
    configDir_ = dir;
    allowlist_.load(dir);
}

InstanceId Engine::loadScript(const std::string& path, uint32_t requestedPerms) {
    std::string content = readFile(path);
    if (content.empty()) {
        spdlog::error("ScriptEngine: failed to read '{}'", path);
        return 0;
    }

    std::string hash = sha256Hex(content);

    if (allowlist_.isDenied(path, hash)) {
        spdlog::info("ScriptEngine: script '{}' is permanently denied", path);
        return 0;
    }

    auto allowed = allowlist_.check(path, hash);
    if (allowed.has_value()) {
        uint32_t grantedPerms = *allowed;
        if ((requestedPerms & ~grantedPerms) == 0)
            return loadScriptInternal(path, content, requestedPerms);
        // Requesting new perms beyond what was granted — re-prompt
    }

    showPermissionPrompt(path, content, hash, requestedPerms);
    return 0;
}

InstanceId Engine::loadScriptInternal(const std::string& path, const std::string& content,
                                       uint32_t permissions) {
    // Unload any existing instance with the same path
    for (auto& inst : instances_) {
        if (inst.path == path && !inst.builtIn) {
            spdlog::info("ScriptEngine: replacing existing instance of '{}'", path);
            unload(inst.id);
            break;
        }
    }

    JSContext* ctx = createContext();
    InstanceId id = nextId_++;
    setupGlobals(ctx, id);

    std::string hash = sha256Hex(content);
    instances_.push_back({id, ctx, path, hash, permissions, false});
    JS_SetContextOpaque(ctx, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));

    JSValue result = JS_Eval(ctx, content.c_str(), content.size(), path.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        spdlog::error("ScriptEngine: '{}' error: {}", path, str ? str : "(null)");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        instances_.pop_back();
        return 0;
    }
    JS_FreeValue(ctx, result);
    spdlog::info("ScriptEngine: loaded script '{}' (id={}, perms={})", path, id, permissionsToString(permissions));
    return id;
}

void Engine::showPermissionPrompt(const std::string& path, const std::string& content,
                                   const std::string& hash, uint32_t requestedPerms) {
    auto tabInfos = callbacks_.tabs();
    PaneId activePaneId = -1;
    for (auto& ti : tabInfos) {
        if (ti.active && ti.focusedPane >= 0) {
            activePaneId = ti.focusedPane;
            break;
        }
    }
    if (activePaneId < 0) {
        spdlog::warn("ScriptEngine: no active pane for permission prompt");
        return;
    }

    auto paneInfoVal = callbacks_.paneInfo(activePaneId);
    std::string popupId = "__mb_perm_" + std::to_string(nextId_++);

    int popW = 50, popH = 8;
    int popX = std::max(0, (paneInfoVal.cols - popW) / 2);
    int popY = std::max(0, (paneInfoVal.rows - popH) / 2);

    PendingScript pending;
    pending.path = path;
    pending.content = content;
    pending.hash = hash;
    pending.requestedPerms = requestedPerms;
    pending.popupId = popupId;
    pending.promptPaneId = activePaneId;
    pendingScripts_[popupId] = std::move(pending);

    auto selfPopupId = popupId;
    bool ok = callbacks_.createPopup(activePaneId, popupId, popX, popY, popW, popH,
        [this, selfPopupId](const char* data, size_t len) {
            if (len == 1)
                handlePermissionResponse(selfPopupId, data[0]);
        });

    if (!ok) {
        pendingScripts_.erase(popupId);
        spdlog::error("ScriptEngine: failed to create permission prompt popup");
        return;
    }

    // Render prompt content
    std::string permStr = permissionsToString(requestedPerms);
    // Extract filename from path
    std::string filename = path;
    auto slash = filename.rfind('/');
    if (slash != std::string::npos) filename = filename.substr(slash + 1);

    std::string display;
    display += "\x1b[H\x1b[1;33m Script Permission Request \x1b[0m";
    display += "\x1b[3;2H Path: \x1b[1m" + filename + "\x1b[0m";
    display += "\x1b[4;2H Perms: \x1b[36m" + permStr + "\x1b[0m";
    display += "\x1b[5;2H Hash: \x1b[90m" + hash.substr(0, 16) + "...\x1b[0m";
    display += "\x1b[7;2H \x1b[1;32my\x1b[0m=allow \x1b[1;31mn\x1b[0m=deny "
               "\x1b[1;36ma\x1b[0m=always \x1b[1;35md\x1b[0m=never";

    callbacks_.injectPopupData(activePaneId, popupId, display);
    spdlog::info("ScriptEngine: showing permission prompt for '{}'", path);
}

void Engine::handlePermissionResponse(const std::string& popupId, char response) {
    auto it = pendingScripts_.find(popupId);
    if (it == pendingScripts_.end()) return;

    PendingScript pending = std::move(it->second);
    pendingScripts_.erase(it);

    callbacks_.destroyPopup(pending.promptPaneId, popupId);

    switch (response) {
    case 'y': case 'Y':
        loadScriptInternal(pending.path, pending.content, pending.requestedPerms);
        break;
    case 'a': case 'A':
        allowlist_.allow(pending.path, pending.hash, pending.requestedPerms);
        allowlist_.save();
        loadScriptInternal(pending.path, pending.content, pending.requestedPerms);
        break;
    case 'd': case 'D':
        allowlist_.deny(pending.path, pending.hash);
        allowlist_.save();
        spdlog::info("ScriptEngine: permanently denied '{}'", pending.path);
        break;
    case 'n': case 'N':
    default:
        spdlog::info("ScriptEngine: denied '{}' (one-time)", pending.path);
        break;
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

void Engine::deliverInput(const char* registryName, uint32_t key,
                           const char* data, size_t len)
{
    for (auto& inst : instances_) {
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
                    spdlog::error("ScriptEngine: input listener error: {}", s ? s : "(null)");
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
    for (auto& inst : instances_) {
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
                            spdlog::error("ScriptEngine: popup input error: {}", s ? s : "(null)");
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

void Engine::addOverlayOutputFilter(TabId tab, InstanceId instId) {
    overlayOutputFilterCount_[tab]++;
    if (auto* inst = findInstance(instId))
        inst->overlayOutputFilters.push_back(tab);
}

void Engine::addOverlayInputFilter(TabId tab, InstanceId instId) {
    overlayInputFilterCount_[tab]++;
    if (auto* inst = findInstance(instId))
        inst->overlayInputFilters.push_back(tab);
}

bool Engine::setNamespace(InstanceId id, const std::string& ns)
{
    auto* inst = findInstance(id);
    if (!inst) return false;
    if (!inst->ns.empty()) return false; // already set

    // Check no other instance holds this namespace
    for (auto& other : instances_) {
        if (other.id != id && other.ns == ns) return false;
    }

    inst->ns = ns;
    spdlog::info("ScriptEngine: instance {} claimed namespace '{}'", id, ns);
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
    spdlog::info("ScriptEngine: registered action '{}'", fullName);
    return true;
}

bool Engine::isActionRegistered(const std::string& fullName) const
{
    return registeredActions_.count(fullName) > 0;
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
