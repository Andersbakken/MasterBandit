#include "ScriptLayoutBindings.h"

#include "LayoutTree.h"
#include "ScriptEngine.h"
#include "ScriptPermissions.h"
#include "Uuid.h"

#include <string>

namespace Script {

namespace {

// ---------------------------------------------------------------------------
// Runtime lookups — mirror the helpers in ScriptEngine.cpp. Duplicated here
// (rather than exported) to keep the binding module self-contained.
// ---------------------------------------------------------------------------

Engine* engineFromCtx(JSContext* ctx)
{
    return static_cast<Engine*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

Engine::Instance* instanceFromCtx(JSContext* ctx)
{
    auto* eng = engineFromCtx(ctx);
    uintptr_t id = reinterpret_cast<uintptr_t>(JS_GetContextOpaque(ctx));
    return eng ? eng->findInstance(static_cast<InstanceId>(id)) : nullptr;
}

bool checkPerm(JSContext* ctx, uint32_t required)
{
    auto* inst = instanceFromCtx(ctx);
    return inst && (inst->permissions & required) != 0;
}

// Pull a UUID from a JS argument. On malformed input, throws and returns nil.
// Caller must bail on JS_HasException(ctx) to propagate the exception.
Uuid parseUuidArg(JSContext* ctx, JSValueConst v, const char* argName)
{
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "%s: expected UUID string", argName);
        return {};
    }
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return {};
    Uuid u = Uuid::fromString(std::string_view(s, len));
    JS_FreeCString(ctx, s);
    if (u.isNil()) {
        JS_ThrowTypeError(ctx, "%s: malformed UUID", argName);
        return {};
    }
    return u;
}

JSValue uuidToJs(JSContext* ctx, Uuid u)
{
    std::string s = u.toString();
    return JS_NewStringLen(ctx, s.data(), s.size());
}

// Read an optional int32 property. If the property is absent or undefined,
// returns `defaultVal`. If present but not a number, throws and returns 0.
int32_t optionalInt(JSContext* ctx, JSValueConst obj, const char* prop, int32_t defaultVal, bool* err)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return defaultVal;
    }
    int32_t out = defaultVal;
    if (JS_ToInt32(ctx, &out, v) != 0) {
        JS_FreeValue(ctx, v);
        *err = true;
        return 0;
    }
    JS_FreeValue(ctx, v);
    return out;
}

// Parse ChildSlot from an optional JS options object. `slotId` is the child's
// UUID (already resolved). All fields default to the ChildSlot defaults.
bool parseChildSlot(JSContext* ctx, JSValueConst opts, Uuid slotId, ChildSlot& out)
{
    out = ChildSlot{};
    out.id = slotId;
    if (JS_IsUndefined(opts) || JS_IsNull(opts)) return true;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "appendChild: options must be an object");
        return false;
    }
    bool err = false;
    out.stretch    = optionalInt(ctx, opts, "stretch",    1, &err);
    out.minCells   = optionalInt(ctx, opts, "minCells",   0, &err);
    out.maxCells   = optionalInt(ctx, opts, "maxCells",   0, &err);
    out.fixedCells = optionalInt(ctx, opts, "fixedCells", 0, &err);
    return !err;
}

// ---------------------------------------------------------------------------
// Node creation — all return a UUID string. Permission: layout.modify.
// ---------------------------------------------------------------------------

JSValue jsLayoutCreateTerminal(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    return uuidToJs(ctx, engineFromCtx(ctx)->layoutTree().createTerminal());
}

JSValue jsLayoutCreateContainer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    SplitDir dir = SplitDir::Horizontal;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        std::string v(s);
        JS_FreeCString(ctx, s);
        if      (v == "horizontal" || v == "h") dir = SplitDir::Horizontal;
        else if (v == "vertical"   || v == "v") dir = SplitDir::Vertical;
        else return JS_ThrowTypeError(ctx,
                    "createContainer: direction must be 'horizontal' or 'vertical'");
    }
    return uuidToJs(ctx, engineFromCtx(ctx)->layoutTree().createContainer(dir));
}

JSValue jsLayoutCreateStack(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    return uuidToJs(ctx, engineFromCtx(ctx)->layoutTree().createStack());
}

JSValue jsLayoutCreateTabBar(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    return uuidToJs(ctx, engineFromCtx(ctx)->layoutTree().createTabBar());
}

// ---------------------------------------------------------------------------
// Root
// ---------------------------------------------------------------------------

JSValue jsLayoutSetRoot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 1) return JS_ThrowTypeError(ctx, "setRoot: expected a UUID");
    Uuid id = parseUuidArg(ctx, argv[0], "setRoot(id)");
    if (id.isNil()) return JS_EXCEPTION;
    if (!engineFromCtx(ctx)->layoutTree().setRoot(id))
        return JS_ThrowTypeError(ctx, "setRoot: unknown node or node already has a parent");
    return JS_UNDEFINED;
}

JSValue jsLayoutGetRoot(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    Uuid r = engineFromCtx(ctx)->layoutTree().root();
    if (r.isNil()) return JS_NULL;
    return uuidToJs(ctx, r);
}

// ---------------------------------------------------------------------------
// Child management
// ---------------------------------------------------------------------------

JSValue jsLayoutAppendChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "appendChild(parent, child[, options])");
    Uuid parent = parseUuidArg(ctx, argv[0], "appendChild(parent)");
    if (parent.isNil()) return JS_EXCEPTION;
    Uuid child  = parseUuidArg(ctx, argv[1], "appendChild(child)");
    if (child.isNil()) return JS_EXCEPTION;
    ChildSlot slot;
    if (argc >= 3) {
        if (!parseChildSlot(ctx, argv[2], child, slot)) return JS_EXCEPTION;
    } else {
        slot.id = child;
    }
    if (!engineFromCtx(ctx)->layoutTree().appendChild(parent, slot))
        return JS_ThrowTypeError(ctx,
                "appendChild: parent or child missing, parent is a leaf, or child already has a parent");
    return JS_UNDEFINED;
}

JSValue jsLayoutRemoveChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 2) return JS_ThrowTypeError(ctx, "removeChild(parent, child)");
    Uuid parent = parseUuidArg(ctx, argv[0], "removeChild(parent)"); if (parent.isNil()) return JS_EXCEPTION;
    Uuid child  = parseUuidArg(ctx, argv[1], "removeChild(child)");  if (child.isNil())  return JS_EXCEPTION;
    if (!engineFromCtx(ctx)->layoutTree().removeChild(parent, child))
        return JS_ThrowTypeError(ctx, "removeChild: parent missing, leaf parent, or child not in parent's list");
    return JS_UNDEFINED;
}

JSValue jsLayoutSetActiveChild(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 2) return JS_ThrowTypeError(ctx, "setActiveChild(stack, child)");
    Uuid stack = parseUuidArg(ctx, argv[0], "setActiveChild(stack)"); if (stack.isNil()) return JS_EXCEPTION;
    Uuid child = parseUuidArg(ctx, argv[1], "setActiveChild(child)"); if (child.isNil()) return JS_EXCEPTION;
    if (!engineFromCtx(ctx)->layoutTree().setActiveChild(stack, child))
        return JS_ThrowTypeError(ctx, "setActiveChild: not a Stack, or child is not a direct child of it");
    return JS_UNDEFINED;
}

JSValue jsLayoutSetTabBarStack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 2) return JS_ThrowTypeError(ctx, "setTabBarStack(tabBar, stackOrNull)");
    Uuid bar = parseUuidArg(ctx, argv[0], "setTabBarStack(tabBar)"); if (bar.isNil()) return JS_EXCEPTION;

    Uuid target;
    // Allow null / undefined to clear the binding; otherwise require a UUID.
    if (!JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        target = parseUuidArg(ctx, argv[1], "setTabBarStack(stack)");
        if (target.isNil()) return JS_EXCEPTION;
    }
    if (!engineFromCtx(ctx)->layoutTree().setTabBarStack(bar, target))
        return JS_ThrowTypeError(ctx, "setTabBarStack: tabBar is not a TabBar, or target is not a Stack");
    return JS_UNDEFINED;
}

JSValue jsLayoutSetLabel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 2) return JS_ThrowTypeError(ctx, "setLabel(id, label)");
    Uuid id = parseUuidArg(ctx, argv[0], "setLabel(id)"); if (id.isNil()) return JS_EXCEPTION;
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!s) return JS_EXCEPTION;
    engineFromCtx(ctx)->layoutTree().setLabel(id, std::string(s, len));
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

JSValue jsLayoutDestroyNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 1) return JS_ThrowTypeError(ctx, "destroyNode(id)");
    Uuid id = parseUuidArg(ctx, argv[0], "destroyNode(id)"); if (id.isNil()) return JS_EXCEPTION;
    engineFromCtx(ctx)->layoutTree().destroyNode(id);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Lifecycle primitives — go through AppCallbacks so the tree + Platform
// stay in sync (PTY spawn, graveyard, tab-bar chrome, etc.).
// ---------------------------------------------------------------------------

JSValue jsLayoutCreateTab(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    auto* eng = engineFromCtx(ctx);
    if (!eng->callbacks().createEmptyTab)
        return JS_ThrowTypeError(ctx, "createTab: not wired");
    auto [idx, nodeIdStr] = eng->callbacks().createEmptyTab();
    if (idx < 0) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",     JS_NewInt32(ctx, idx));
    JS_SetPropertyStr(ctx, o, "nodeId", nodeIdStr.empty() ? JS_NULL :
        JS_NewStringLen(ctx, nodeIdStr.data(), nodeIdStr.size()));
    return o;
}

JSValue jsLayoutActivateTab(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 1) return JS_ThrowTypeError(ctx, "activateTab(idOrNodeId)");
    auto* eng = engineFromCtx(ctx);
    int idx = -1;
    if (JS_IsNumber(argv[0])) {
        if (JS_ToInt32(ctx, &idx, argv[0]) != 0) return JS_EXCEPTION;
    } else if (JS_IsString(argv[0])) {
        // Resolve via tabs() callback — match by tab's nodeId.
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s) return JS_EXCEPTION;
        std::string want(s, len);
        JS_FreeCString(ctx, s);
        if (eng->callbacks().tabs) {
            auto all = eng->callbacks().tabs();
            for (const auto& t : all) {
                if (t.nodeId == want) { idx = t.id; break; }
            }
        }
    }
    if (idx < 0) return JS_ThrowTypeError(ctx, "activateTab: unknown tab");
    if (eng->callbacks().activateTab) eng->callbacks().activateTab(idx);
    return JS_UNDEFINED;
}

JSValue jsLayoutFocusPane(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 1) return JS_ThrowTypeError(ctx, "focusPane(idOrNodeId)");
    auto* eng = engineFromCtx(ctx);
    int paneId = -1;
    if (JS_IsNumber(argv[0])) {
        if (JS_ToInt32(ctx, &paneId, argv[0]) != 0) return JS_EXCEPTION;
    } else if (JS_IsString(argv[0])) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s) return JS_EXCEPTION;
        // paneInfo doesn't take a nodeId, so iterate tabs() → paneInfo.
        std::string want(s, len);
        JS_FreeCString(ctx, s);
        if (eng->callbacks().tabs && eng->callbacks().paneInfo) {
            auto all = eng->callbacks().tabs();
            for (const auto& t : all) {
                for (int pid : t.panes) {
                    auto pi = eng->callbacks().paneInfo(pid);
                    if (pi.nodeId == want) { paneId = pid; break; }
                }
                if (paneId >= 0) break;
            }
        }
    }
    if (paneId < 0) return JS_FALSE;
    bool ok = eng->callbacks().focusPane && eng->callbacks().focusPane(paneId);
    return JS_NewBool(ctx, ok);
}

JSValue jsLayoutClosePane(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 1) return JS_ThrowTypeError(ctx, "closePane(idOrNodeId)");
    auto* eng = engineFromCtx(ctx);
    int paneId = -1;
    if (JS_IsNumber(argv[0])) {
        if (JS_ToInt32(ctx, &paneId, argv[0]) != 0) return JS_EXCEPTION;
    } else if (JS_IsString(argv[0])) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s) return JS_EXCEPTION;
        std::string want(s, len);
        JS_FreeCString(ctx, s);
        if (eng->callbacks().tabs && eng->callbacks().paneInfo) {
            auto all = eng->callbacks().tabs();
            for (const auto& t : all) {
                for (int pid : t.panes) {
                    auto pi = eng->callbacks().paneInfo(pid);
                    if (pi.nodeId == want) { paneId = pid; break; }
                }
                if (paneId >= 0) break;
            }
        }
    }
    if (paneId < 0) return JS_FALSE;
    bool ok = eng->callbacks().closePane && eng->callbacks().closePane(paneId);
    return JS_NewBool(ctx, ok);
}

JSValue jsLayoutCloseTab(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 1) return JS_ThrowTypeError(ctx, "closeTab(idOrNodeId)");
    auto* eng = engineFromCtx(ctx);
    int idx = -1;
    if (JS_IsNumber(argv[0])) {
        if (JS_ToInt32(ctx, &idx, argv[0]) != 0) return JS_EXCEPTION;
    } else if (JS_IsString(argv[0])) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s) return JS_EXCEPTION;
        std::string want(s, len);
        JS_FreeCString(ctx, s);
        if (eng->callbacks().tabs) {
            auto all = eng->callbacks().tabs();
            for (const auto& t : all) {
                if (t.nodeId == want) { idx = t.id; break; }
            }
        }
    }
    if (idx < 0) return JS_FALSE;
    if (eng->callbacks().closeTab) eng->callbacks().closeTab(idx);
    return JS_TRUE;
}

// Replaces the old tree-only createTerminal. Now spawns a PTY and attaches
// the Terminal node under `parentContainerNodeId` in one shot.
JSValue jsLayoutCreateTerminalComposite(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 1) return JS_ThrowTypeError(ctx, "createTerminal(parentNodeId, opts?)");
    Uuid parent = parseUuidArg(ctx, argv[0], "createTerminal(parent)");
    if (parent.isNil()) return JS_EXCEPTION;

    std::string cwd;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue cwdv = JS_GetPropertyStr(ctx, argv[1], "cwd");
        if (JS_IsString(cwdv)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(ctx, &len, cwdv);
            if (s) { cwd.assign(s, len); JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, cwdv);
    }

    auto* eng = engineFromCtx(ctx);
    if (!eng->callbacks().createTerminalInContainer)
        return JS_ThrowTypeError(ctx, "createTerminal: not wired");
    auto np = eng->callbacks().createTerminalInContainer(parent.toString(), cwd);
    if (!np.ok) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",     JS_NewInt32(ctx, np.paneId));
    JS_SetPropertyStr(ctx, o, "nodeId", np.nodeId.empty() ? JS_NULL :
        JS_NewStringLen(ctx, np.nodeId.data(), np.nodeId.size()));
    return o;
}

// splitPane(existingPaneNodeId, dir, newIsFirst?) → {id, nodeId}
JSValue jsLayoutSplitPane(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 2) return JS_ThrowTypeError(ctx, "splitPane(existingNodeId, dir, newIsFirst?)");
    Uuid existing = parseUuidArg(ctx, argv[0], "splitPane(existing)");
    if (existing.isNil()) return JS_EXCEPTION;
    if (!JS_IsString(argv[1])) return JS_ThrowTypeError(ctx,
        "splitPane: dir must be 'horizontal'|'vertical'|'h'|'v'|'left'|'right'|'up'|'down'");
    size_t dlen = 0;
    const char* ds = JS_ToCStringLen(ctx, &dlen, argv[1]);
    if (!ds) return JS_EXCEPTION;
    std::string dir(ds, dlen);
    JS_FreeCString(ctx, ds);
    bool newIsFirst = false;
    std::string wireDir = "horizontal";
    if (dir == "horizontal" || dir == "h") { wireDir = "horizontal"; }
    else if (dir == "vertical" || dir == "v") { wireDir = "vertical"; }
    else if (dir == "right") { wireDir = "horizontal"; }
    else if (dir == "left")  { wireDir = "horizontal"; newIsFirst = true; }
    else if (dir == "down")  { wireDir = "vertical"; }
    else if (dir == "up")    { wireDir = "vertical"; newIsFirst = true; }
    else return JS_ThrowTypeError(ctx, "splitPane: bad dir");
    if (argc >= 3) newIsFirst = JS_ToBool(ctx, argv[2]) ? true : newIsFirst;

    auto* eng = engineFromCtx(ctx);
    if (!eng->callbacks().splitPaneByNodeId)
        return JS_ThrowTypeError(ctx, "splitPane: not wired");
    auto np = eng->callbacks().splitPaneByNodeId(existing.toString(), wireDir, newIsFirst);
    if (!np.ok) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",     JS_NewInt32(ctx, np.paneId));
    JS_SetPropertyStr(ctx, o, "nodeId", np.nodeId.empty() ? JS_NULL :
        JS_NewStringLen(ctx, np.nodeId.data(), np.nodeId.size()));
    return o;
}

// Shared implementation for setSlotStretch/MinCells/MaxCells/FixedCells.
// `which` picks which LayoutTree setter to call.
enum class SlotField { Stretch, MinCells, MaxCells, FixedCells };
JSValue jsLayoutSetSlotField(JSContext* ctx, int argc, JSValueConst* argv,
                             SlotField which, const char* sig)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 3) return JS_ThrowTypeError(ctx, "%s", sig);
    Uuid parent = parseUuidArg(ctx, argv[0], "parent"); if (parent.isNil()) return JS_EXCEPTION;
    Uuid child  = parseUuidArg(ctx, argv[1], "child");  if (child.isNil())  return JS_EXCEPTION;
    int32_t value;
    if (JS_ToInt32(ctx, &value, argv[2]) != 0) return JS_EXCEPTION;
    auto& tree = engineFromCtx(ctx)->layoutTree();
    bool ok = false;
    switch (which) {
    case SlotField::Stretch:    ok = tree.setSlotStretch(parent, child, value);    break;
    case SlotField::MinCells:   ok = tree.setSlotMinCells(parent, child, value);   break;
    case SlotField::MaxCells:   ok = tree.setSlotMaxCells(parent, child, value);   break;
    case SlotField::FixedCells: ok = tree.setSlotFixedCells(parent, child, value); break;
    }
    return JS_NewBool(ctx, ok);
}

JSValue jsLayoutSetSlotStretch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsLayoutSetSlotField(ctx, argc, argv, SlotField::Stretch,
        "setSlotStretch(parent, child, stretch)");
}
JSValue jsLayoutSetSlotMinCells(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsLayoutSetSlotField(ctx, argc, argv, SlotField::MinCells,
        "setSlotMinCells(parent, child, minCells)");
}
JSValue jsLayoutSetSlotMaxCells(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsLayoutSetSlotField(ctx, argc, argv, SlotField::MaxCells,
        "setSlotMaxCells(parent, child, maxCells)");
}
JSValue jsLayoutSetSlotFixedCells(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsLayoutSetSlotField(ctx, argc, argv, SlotField::FixedCells,
        "setSlotFixedCells(parent, child, fixedCells)");
}

JSValue jsLayoutAdjustPaneSize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    if (argc < 3) return JS_ThrowTypeError(ctx, "adjustPaneSize(paneNodeId, dir, amount)");
    Uuid pane = parseUuidArg(ctx, argv[0], "paneNodeId"); if (pane.isNil()) return JS_EXCEPTION;
    size_t len = 0;
    const char* ds = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!ds) return JS_EXCEPTION;
    std::string dir(ds, len);
    JS_FreeCString(ctx, ds);
    int32_t amount;
    if (JS_ToInt32(ctx, &amount, argv[2]) != 0) return JS_EXCEPTION;
    auto* eng = engineFromCtx(ctx);
    if (!eng->callbacks().adjustPaneSize)
        return JS_ThrowTypeError(ctx, "adjustPaneSize: not wired");
    return JS_NewBool(ctx, eng->callbacks().adjustPaneSize(pane.toString(), dir, amount));
}

JSValue jsLayoutSetZoom(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (!checkPerm(ctx, Perm::LayoutModify))
        return JS_ThrowTypeError(ctx, "permission denied: layout.modify not granted");
    std::string target;
    if (argc >= 1 && JS_IsString(argv[0])) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s) return JS_EXCEPTION;
        target.assign(s, len);
        JS_FreeCString(ctx, s);
    }
    auto* eng = engineFromCtx(ctx);
    if (!eng->callbacks().setZoom) return JS_ThrowTypeError(ctx, "setZoom: not wired");
    return JS_NewBool(ctx, eng->callbacks().setZoom(target));
}

// focusedPane() → {id, nodeId, tabId, tabNodeId} | null
JSValue jsLayoutFocusedPane(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    auto* eng = engineFromCtx(ctx);
    if (!eng->callbacks().tabs || !eng->callbacks().paneInfo) return JS_NULL;
    auto all = eng->callbacks().tabs();
    for (const auto& t : all) {
        if (!t.active) continue;
        if (t.focusedPane < 0) return JS_NULL;
        auto pi = eng->callbacks().paneInfo(t.focusedPane);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "id",        JS_NewInt32(ctx, t.focusedPane));
        JS_SetPropertyStr(ctx, o, "nodeId",    pi.nodeId.empty() ? JS_NULL :
            JS_NewStringLen(ctx, pi.nodeId.data(), pi.nodeId.size()));
        JS_SetPropertyStr(ctx, o, "tabId",     JS_NewInt32(ctx, t.id));
        JS_SetPropertyStr(ctx, o, "tabNodeId", t.nodeId.empty() ? JS_NULL :
            JS_NewStringLen(ctx, t.nodeId.data(), t.nodeId.size()));
        return o;
    }
    return JS_NULL;
}

// activeTab() → {id, nodeId} | null
JSValue jsLayoutActiveTab(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    auto* eng = engineFromCtx(ctx);
    if (!eng->callbacks().tabs) return JS_NULL;
    auto all = eng->callbacks().tabs();
    for (const auto& t : all) {
        if (!t.active) continue;
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "id",     JS_NewInt32(ctx, t.id));
        JS_SetPropertyStr(ctx, o, "nodeId", t.nodeId.empty() ? JS_NULL :
            JS_NewStringLen(ctx, t.nodeId.data(), t.nodeId.size()));
        return o;
    }
    return JS_NULL;
}

// ---------------------------------------------------------------------------
// Introspection — ungated
// ---------------------------------------------------------------------------

const char* kindName(NodeKind k)
{
    switch (k) {
        case NodeKind::Terminal:  return "terminal";
        case NodeKind::Container: return "container";
        case NodeKind::Stack:     return "stack";
        case NodeKind::TabBar:    return "tabbar";
    }
    return "unknown";
}

JSValue childSlotToJs(JSContext* ctx, const ChildSlot& s)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",         uuidToJs(ctx, s.id));
    JS_SetPropertyStr(ctx, o, "stretch",    JS_NewInt32(ctx, s.stretch));
    JS_SetPropertyStr(ctx, o, "minCells",   JS_NewInt32(ctx, s.minCells));
    JS_SetPropertyStr(ctx, o, "maxCells",   JS_NewInt32(ctx, s.maxCells));
    JS_SetPropertyStr(ctx, o, "fixedCells", JS_NewInt32(ctx, s.fixedCells));
    return o;
}

JSValue childListToJs(JSContext* ctx, const std::vector<ChildSlot>& v)
{
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < v.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, childSlotToJs(ctx, v[i]));
    }
    return arr;
}

JSValue jsLayoutNode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "node(id)");
    Uuid id = parseUuidArg(ctx, argv[0], "node(id)"); if (id.isNil()) return JS_EXCEPTION;
    const Node* n = engineFromCtx(ctx)->layoutTree().node(id);
    if (!n) return JS_NULL;

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "id",    uuidToJs(ctx, n->id));
    JS_SetPropertyStr(ctx, o, "kind",  JS_NewString(ctx, kindName(n->kind())));
    JS_SetPropertyStr(ctx, o, "label", JS_NewStringLen(ctx, n->label.data(), n->label.size()));
    JS_SetPropertyStr(ctx, o, "parent",
        n->parent.isNil() ? JS_NULL : uuidToJs(ctx, n->parent));

    std::visit([&](const auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, ContainerData>) {
            JS_SetPropertyStr(ctx, o, "direction",
                JS_NewString(ctx, d.dir == SplitDir::Horizontal ? "horizontal" : "vertical"));
            JS_SetPropertyStr(ctx, o, "children", childListToJs(ctx, d.children));
        } else if constexpr (std::is_same_v<T, StackData>) {
            JS_SetPropertyStr(ctx, o, "children",    childListToJs(ctx, d.children));
            JS_SetPropertyStr(ctx, o, "activeChild",
                d.activeChild.isNil() ? JS_NULL : uuidToJs(ctx, d.activeChild));
            JS_SetPropertyStr(ctx, o, "opaque",      JS_NewBool(ctx, d.opaque));
        } else if constexpr (std::is_same_v<T, TabBarData>) {
            JS_SetPropertyStr(ctx, o, "boundStack",
                d.boundStack.isNil() ? JS_NULL : uuidToJs(ctx, d.boundStack));
        }
        // Terminal: no extra fields.
    }, n->data);
    return o;
}

// computeRects({x, y, w, h}, cellW, cellH) → { [uuid]: {x, y, w, h} }
JSValue jsLayoutComputeRects(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "computeRects(window, cellW, cellH)");

    bool err = false;
    LayoutRect win;
    win.x = optionalInt(ctx, argv[0], "x", 0, &err);
    win.y = optionalInt(ctx, argv[0], "y", 0, &err);
    win.w = optionalInt(ctx, argv[0], "w", 0, &err);
    win.h = optionalInt(ctx, argv[0], "h", 0, &err);
    if (err) return JS_EXCEPTION;

    int32_t cellW = 1, cellH = 1;
    if (argc >= 2 && JS_ToInt32(ctx, &cellW, argv[1]) != 0) return JS_EXCEPTION;
    if (argc >= 3 && JS_ToInt32(ctx, &cellH, argv[2]) != 0) return JS_EXCEPTION;

    auto rects = engineFromCtx(ctx)->layoutTree().computeRects(win, cellW, cellH);

    JSValue o = JS_NewObject(ctx);
    for (const auto& [uuid, r] : rects) {
        JSValue ro = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ro, "x", JS_NewInt32(ctx, r.x));
        JS_SetPropertyStr(ctx, ro, "y", JS_NewInt32(ctx, r.y));
        JS_SetPropertyStr(ctx, ro, "w", JS_NewInt32(ctx, r.w));
        JS_SetPropertyStr(ctx, ro, "h", JS_NewInt32(ctx, r.h));
        std::string key = uuid.toString();
        JS_SetPropertyStr(ctx, o, key.c_str(), ro);
    }
    return o;
}

} // namespace

void installLayoutBindings(Engine&, JSContext* ctx, JSValue mb)
{
    JSValue layout = JS_NewObject(ctx);

    // Composite createTerminal: spawns PTY, attaches under parentContainerNodeId.
    JS_SetPropertyStr(ctx, layout, "createTerminal",
        JS_NewCFunction(ctx, jsLayoutCreateTerminalComposite, "createTerminal", 2));
    // Bare tree-node creation (no PTY). For specialized cases — normal
    // controllers should use createTerminal.
    JS_SetPropertyStr(ctx, layout, "createTerminalNode",
        JS_NewCFunction(ctx, jsLayoutCreateTerminal, "createTerminalNode", 0));
    JS_SetPropertyStr(ctx, layout, "createContainer",
        JS_NewCFunction(ctx, jsLayoutCreateContainer, "createContainer", 1));
    JS_SetPropertyStr(ctx, layout, "createStack",
        JS_NewCFunction(ctx, jsLayoutCreateStack, "createStack", 0));
    JS_SetPropertyStr(ctx, layout, "createTabBar",
        JS_NewCFunction(ctx, jsLayoutCreateTabBar, "createTabBar", 0));

    // Lifecycle + query primitives on mb.layout
    JS_SetPropertyStr(ctx, layout, "createTab",
        JS_NewCFunction(ctx, jsLayoutCreateTab, "createTab", 0));
    JS_SetPropertyStr(ctx, layout, "closeTab",
        JS_NewCFunction(ctx, jsLayoutCloseTab, "closeTab", 1));
    JS_SetPropertyStr(ctx, layout, "activateTab",
        JS_NewCFunction(ctx, jsLayoutActivateTab, "activateTab", 1));
    JS_SetPropertyStr(ctx, layout, "focusPane",
        JS_NewCFunction(ctx, jsLayoutFocusPane, "focusPane", 1));
    JS_SetPropertyStr(ctx, layout, "closePane",
        JS_NewCFunction(ctx, jsLayoutClosePane, "closePane", 1));
    JS_SetPropertyStr(ctx, layout, "splitPane",
        JS_NewCFunction(ctx, jsLayoutSplitPane, "splitPane", 3));
    JS_SetPropertyStr(ctx, layout, "setZoom",
        JS_NewCFunction(ctx, jsLayoutSetZoom, "setZoom", 1));
    JS_SetPropertyStr(ctx, layout, "setSlotStretch",
        JS_NewCFunction(ctx, jsLayoutSetSlotStretch, "setSlotStretch", 3));
    JS_SetPropertyStr(ctx, layout, "setSlotMinCells",
        JS_NewCFunction(ctx, jsLayoutSetSlotMinCells, "setSlotMinCells", 3));
    JS_SetPropertyStr(ctx, layout, "setSlotMaxCells",
        JS_NewCFunction(ctx, jsLayoutSetSlotMaxCells, "setSlotMaxCells", 3));
    JS_SetPropertyStr(ctx, layout, "setSlotFixedCells",
        JS_NewCFunction(ctx, jsLayoutSetSlotFixedCells, "setSlotFixedCells", 3));
    JS_SetPropertyStr(ctx, layout, "adjustPaneSize",
        JS_NewCFunction(ctx, jsLayoutAdjustPaneSize, "adjustPaneSize", 3));
    JS_SetPropertyStr(ctx, layout, "focusedPane",
        JS_NewCFunction(ctx, jsLayoutFocusedPane, "focusedPane", 0));
    JS_SetPropertyStr(ctx, layout, "activeTab",
        JS_NewCFunction(ctx, jsLayoutActiveTab, "activeTab", 0));

    JS_SetPropertyStr(ctx, layout, "setRoot",
        JS_NewCFunction(ctx, jsLayoutSetRoot, "setRoot", 1));
    JS_SetPropertyStr(ctx, layout, "getRoot",
        JS_NewCFunction(ctx, jsLayoutGetRoot, "getRoot", 0));

    JS_SetPropertyStr(ctx, layout, "appendChild",
        JS_NewCFunction(ctx, jsLayoutAppendChild, "appendChild", 3));
    JS_SetPropertyStr(ctx, layout, "removeChild",
        JS_NewCFunction(ctx, jsLayoutRemoveChild, "removeChild", 2));
    JS_SetPropertyStr(ctx, layout, "setActiveChild",
        JS_NewCFunction(ctx, jsLayoutSetActiveChild, "setActiveChild", 2));
    JS_SetPropertyStr(ctx, layout, "setTabBarStack",
        JS_NewCFunction(ctx, jsLayoutSetTabBarStack, "setTabBarStack", 2));
    JS_SetPropertyStr(ctx, layout, "setLabel",
        JS_NewCFunction(ctx, jsLayoutSetLabel, "setLabel", 2));
    JS_SetPropertyStr(ctx, layout, "destroyNode",
        JS_NewCFunction(ctx, jsLayoutDestroyNode, "destroyNode", 1));

    JS_SetPropertyStr(ctx, layout, "node",
        JS_NewCFunction(ctx, jsLayoutNode, "node", 1));
    JS_SetPropertyStr(ctx, layout, "computeRects",
        JS_NewCFunction(ctx, jsLayoutComputeRects, "computeRects", 3));

    JS_SetPropertyStr(ctx, mb, "layout", layout);
}

} // namespace Script
