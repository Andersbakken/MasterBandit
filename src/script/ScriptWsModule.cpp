#include "ScriptWsModule.h"
#include "ScriptEngine.h"
#include "ScriptPermissions.h"

#include <eventloop/EventLoop.h>
#include <libwebsockets.h>
#include <quickjs.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ============================================================================
// mb:ws — WebSocket server module for JS scripts.
//
// Single lws_context per Engine (shared across servers). Each createServer()
// allocates a new vhost bound to 127.0.0.1:port. Token auth is implemented by
// using the token AS the protocol name ("mb-shell.<token>") — lws's built-in
// protocol matching rejects non-matching clients automatically during the WS
// handshake. No custom FILTER_PROTOCOL_CONNECTION logic needed.
// ============================================================================

using Script::Engine;
using Script::InstanceId;

namespace {

struct WsServer;

struct WsConnection {
    lws* wsi = nullptr;
    WsServer* server = nullptr;
    std::vector<uint8_t> rxBuffer;
    bool rxBinary = false;
    // Each pending tx: (bytes-with-LWS_PRE-pre-pad, isBinary). The first
    // LWS_PRE bytes are reserved padding; payload starts at +LWS_PRE.
    std::deque<std::pair<std::vector<uint8_t>, bool>> txQueue;
    bool closed = false;
    JSValue jsObj = JS_UNDEFINED; // strong ref, freed on connection close
};

// per_session_data_size payload — lws allocates this per wsi.
struct PerSessionData {
    WsConnection* conn;
};

struct WsServer {
    InstanceId owner = 0;
    JSContext* ctx = nullptr;
    std::string host;
    std::string token;
    std::string protocolName;         // "mb-shell.<token>" — also lws protocol name
    int boundPort = 0;
    lws_vhost* vhost = nullptr;
    // Storage for lws_protocols array (must outlive the vhost).
    std::vector<lws_protocols> protocols;
    std::vector<std::unique_ptr<WsConnection>> connections;
    bool closed = false;
    JSValue jsObj = JS_UNDEFINED; // strong ref, freed on server close
};

struct ModuleState {
    Engine* engine = nullptr;
    lws_context* ctx = nullptr;
    std::vector<std::unique_ptr<WsServer>> servers;
    // Engine pointer is captured in fd-watch lambdas; that's fine since fds are
    // removed before the context is destroyed.
};

// One ModuleState per Engine. Raw map — we never delete Engine mid-session.
std::unordered_map<Engine*, std::unique_ptr<ModuleState>> g_modules;

ModuleState* getModuleState(Engine* eng)
{
    auto it = g_modules.find(eng);
    if (it == g_modules.end()) return nullptr;
    return it->second.get();
}

ModuleState* ensureModuleState(Engine* eng)
{
    auto it = g_modules.find(eng);
    if (it != g_modules.end()) return it->second.get();
    auto st = std::make_unique<ModuleState>();
    st->engine = eng;
    auto* raw = st.get();
    g_modules.emplace(eng, std::move(st));
    return raw;
}

Engine* engineFromJsCtx(JSContext* ctx)
{
    return static_cast<Engine*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

// ============================================================================
// lws callback — shared across all vhosts this module creates.
// ============================================================================

static int wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                      void* user, void* in, size_t len);

// ============================================================================
// JS class: WsConnection
// ============================================================================

static JSClassID jsWsConnClassId = 0;

struct JsWsConnData {
    WsConnection* conn; // non-owning; nulled when C++ side is gone
};

static void jsWsConnFinalize(JSRuntime*, JSValue val)
{
    delete static_cast<JsWsConnData*>(JS_GetOpaque(val, jsWsConnClassId));
}

static JSClassDef jsWsConnClassDef = { "WsConnection", jsWsConnFinalize };

static WsConnection* jsWsConnGet(JSContext* ctx, JSValueConst this_val)
{
    auto* d = static_cast<JsWsConnData*>(JS_GetOpaque(this_val, jsWsConnClassId));
    return d ? d->conn : nullptr;
}

static JSValue jsWsConnSend(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_UNDEFINED;
    WsConnection* c = jsWsConnGet(ctx, this_val);
    if (!c || c->closed || !c->wsi) return JS_UNDEFINED;

    // Accept: string | ArrayBuffer | TypedArray (Uint8Array etc.)
    bool isBinary = false;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;

    size_t abLen = 0;
    uint8_t* abPtr = JS_GetArrayBuffer(ctx, &abLen, argv[0]);
    if (abPtr) {
        isBinary = true;
        data = abPtr;
        dataLen = abLen;
    } else {
        // Try TypedArray (e.g. Uint8Array)
        size_t byteOff = 0, byteLen = 0, bytesPerEl = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &byteLen, &bytesPerEl);
        if (!JS_IsException(ab) && !JS_IsUndefined(ab)) {
            size_t ablen2 = 0;
            uint8_t* ab2 = JS_GetArrayBuffer(ctx, &ablen2, ab);
            JS_FreeValue(ctx, ab);
            if (ab2) {
                isBinary = true;
                data = ab2 + byteOff;
                dataLen = byteLen;
            }
        } else if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }

    std::string stringBuf;
    if (!data) {
        // Fall back to string
        size_t sLen = 0;
        const char* s = JS_ToCStringLen(ctx, &sLen, argv[0]);
        if (!s) return JS_EXCEPTION;
        stringBuf.assign(s, sLen);
        JS_FreeCString(ctx, s);
        data = reinterpret_cast<const uint8_t*>(stringBuf.data());
        dataLen = stringBuf.size();
        isBinary = false;
    }

    // Allocate buf with LWS_PRE front padding.
    std::vector<uint8_t> buf(LWS_PRE + dataLen);
    std::memcpy(buf.data() + LWS_PRE, data, dataLen);
    c->txQueue.emplace_back(std::move(buf), isBinary);
    lws_callback_on_writable(c->wsi);
    return JS_UNDEFINED;
}

static JSValue jsWsConnClose(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    WsConnection* c = jsWsConnGet(ctx, this_val);
    if (!c || c->closed || !c->wsi) return JS_UNDEFINED;
    // Request close. lws will call LWS_CALLBACK_CLOSED which handles cleanup.
    lws_set_timeout(c->wsi, PENDING_TIMEOUT_CLOSE_SEND, 1);
    return JS_UNDEFINED;
}

static JSValue jsWsConnAddEventListener(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_UNDEFINED;
    const char* eventName = JS_ToCString(ctx, argv[0]);
    if (!eventName) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, eventName);
        return JS_UNDEFINED;
    }
    std::string prop = std::string("__evt_") + eventName;
    JS_FreeCString(ctx, eventName);

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

static const JSCFunctionListEntry jsWsConnFuncs[] = {
    JS_CFUNC_DEF("send", 1, jsWsConnSend),
    JS_CFUNC_DEF("close", 0, jsWsConnClose),
    JS_CFUNC_DEF("addEventListener", 2, jsWsConnAddEventListener),
};

static JSValue jsWsConnNew(JSContext* ctx, WsConnection* c)
{
    JSValue obj = JS_NewObjectClass(ctx, jsWsConnClassId);
    JS_SetOpaque(obj, new JsWsConnData{c});
    JS_SetPropertyFunctionList(ctx, obj, jsWsConnFuncs,
                               sizeof(jsWsConnFuncs) / sizeof(jsWsConnFuncs[0]));
    return obj;
}

// ============================================================================
// JS class: WsServer
// ============================================================================

static JSClassID jsWsServerClassId = 0;

struct JsWsServerData {
    WsServer* server; // non-owning; nulled when C++ side is gone
};

static void jsWsServerFinalize(JSRuntime*, JSValue val)
{
    delete static_cast<JsWsServerData*>(JS_GetOpaque(val, jsWsServerClassId));
}

static JSClassDef jsWsServerClassDef = { "WsServer", jsWsServerFinalize };

static WsServer* jsWsServerGet(JSContext* ctx, JSValueConst this_val)
{
    auto* d = static_cast<JsWsServerData*>(JS_GetOpaque(this_val, jsWsServerClassId));
    return d ? d->server : nullptr;
}

static void closeWsServer(ModuleState* mod, WsServer* srv);

static JSValue jsWsServerClose(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
    WsServer* s = jsWsServerGet(ctx, this_val);
    if (!s || s->closed) return JS_UNDEFINED;
    ModuleState* mod = getModuleState(engineFromJsCtx(ctx));
    if (!mod) return JS_UNDEFINED;
    closeWsServer(mod, s);
    return JS_UNDEFINED;
}

static JSValue jsWsServerGetPort(JSContext* ctx, JSValueConst this_val, int /*magic*/)
{
    WsServer* s = jsWsServerGet(ctx, this_val);
    if (!s) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, s->boundPort);
}

static JSValue jsWsServerAddEventListener(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_UNDEFINED;
    const char* eventName = JS_ToCString(ctx, argv[0]);
    if (!eventName) return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, eventName);
        return JS_UNDEFINED;
    }
    std::string prop = std::string("__evt_") + eventName;
    JS_FreeCString(ctx, eventName);

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

static const JSCFunctionListEntry jsWsServerFuncs[] = {
    JS_CGETSET_MAGIC_DEF("port", jsWsServerGetPort, nullptr, 0),
    JS_CFUNC_DEF("close", 0, jsWsServerClose),
    JS_CFUNC_DEF("addEventListener", 2, jsWsServerAddEventListener),
};

// ============================================================================
// Event dispatch helpers — enqueue JS listener calls as microtasks.
// ============================================================================

static void fireEvent(JSContext* ctx, JSValue target, const char* evtName,
                      int argc, JSValueConst* argv)
{
    std::string prop = std::string("__evt_") + evtName;
    JSValue arr = JS_GetPropertyStr(ctx, target, prop.c_str());
    if (JS_IsUndefined(arr)) { JS_FreeValue(ctx, arr); return; }
    uint32_t n = 0;
    {
        JSValue lenVal = JS_GetPropertyStr(ctx, arr, "length");
        JS_ToUint32(ctx, &n, lenVal);
        JS_FreeValue(ctx, lenVal);
    }
    for (uint32_t i = 0; i < n; ++i) {
        JSValue fn = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsFunction(ctx, fn)) {
            JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(ctx);
                const char* s = JS_ToCString(ctx, exc);
                spdlog::error("mb:ws: listener '{}' threw: {}", evtName, s ? s : "(null)");
                if (s) JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, exc);
            }
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, fn);
    }
    JS_FreeValue(ctx, arr);
}

// ============================================================================
// Server lifecycle
// ============================================================================

static void destroyConnectionJs(JSContext* ctx, WsConnection* c, bool fireCloseEvent)
{
    if (JS_IsUndefined(c->jsObj)) return;
    if (fireCloseEvent)
        fireEvent(ctx, c->jsObj, "close", 0, nullptr);
    // Detach opaque pointer so JS references become inert.
    auto* d = static_cast<JsWsConnData*>(JS_GetOpaque(c->jsObj, jsWsConnClassId));
    if (d) d->conn = nullptr;
    JS_FreeValue(ctx, c->jsObj);
    c->jsObj = JS_UNDEFINED;
}

static void closeWsServer(ModuleState* mod, WsServer* srv)
{
    if (srv->closed) return;

    // Fire close events + release JS-side connection objects *before* we mark
    // the server closed and tear down the vhost. The subsequent vhost_destroy
    // will fire LWS_CALLBACK_CLOSED for each wsi, but by then srv->closed is
    // true and JsWsConnData::conn has been nulled, so the handler is inert.
    if (srv->ctx) {
        for (auto& conn : srv->connections) {
            if (!conn->closed)
                destroyConnectionJs(srv->ctx, conn.get(), /*fireCloseEvent=*/true);
        }
    }

    srv->closed = true;
    for (auto& conn : srv->connections) conn->closed = true;

    if (srv->vhost) {
        lws_vhost_destroy(srv->vhost);
        srv->vhost = nullptr;
    }

    // vhost destruction above has fired CLOSED for every connection and those
    // handlers nulled psd->conn. Drop the C++ state for all of them now.
    srv->connections.clear();

    // Detach JS server object.
    if (srv->ctx && !JS_IsUndefined(srv->jsObj)) {
        auto* d = static_cast<JsWsServerData*>(JS_GetOpaque(srv->jsObj, jsWsServerClassId));
        if (d) d->server = nullptr;
        JS_FreeValue(srv->ctx, srv->jsObj);
        srv->jsObj = JS_UNDEFINED;
    }

    // Remove from module state (owning unique_ptr dies here).
    for (auto it = mod->servers.begin(); it != mod->servers.end(); ++it) {
        if (it->get() == srv) { mod->servers.erase(it); break; }
    }
}

// ============================================================================
// lws callback implementation
// ============================================================================

static int wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                      void* user, void* in, size_t len)
{
    lws_context* lwsCtx = lws_get_context(wsi);
    auto* mod = static_cast<ModuleState*>(lws_context_user(lwsCtx));
    if (!mod) return 0;

    // Most callbacks are per-vhost. Get the owning WsServer via vhost user data.
    WsServer* srv = nullptr;
    if (lws_vhost* vh = lws_get_vhost(wsi))
        srv = static_cast<WsServer*>(lws_vhost_user(vh));

    auto* psd = static_cast<PerSessionData*>(user);

    switch (reason) {

    // ---- Per-connection protocol events ----
    case LWS_CALLBACK_ESTABLISHED: {
        if (!srv || srv->closed) return -1;
        auto conn = std::make_unique<WsConnection>();
        conn->wsi = wsi;
        conn->server = srv;
        psd->conn = conn.get();
        WsConnection* raw = conn.get();
        srv->connections.push_back(std::move(conn));

        if (srv->ctx && !JS_IsUndefined(srv->jsObj)) {
            raw->jsObj = jsWsConnNew(srv->ctx, raw);
            // JS ref held by C++ (raw->jsObj) — listeners may add more.
            JSValue dup = JS_DupValue(srv->ctx, raw->jsObj);
            fireEvent(srv->ctx, srv->jsObj, "connection", 1, &dup);
            JS_FreeValue(srv->ctx, dup);
        }
        break;
    }

    case LWS_CALLBACK_RECEIVE: {
        if (!srv || !psd || !psd->conn) break;
        WsConnection* c = psd->conn;
        if (c->closed) break;

        if (c->rxBuffer.empty())
            c->rxBinary = lws_frame_is_binary(wsi) != 0;
        const uint8_t* p = static_cast<const uint8_t*>(in);
        c->rxBuffer.insert(c->rxBuffer.end(), p, p + len);

        if (lws_is_final_fragment(wsi) && srv->ctx && !JS_IsUndefined(c->jsObj)) {
            JSValue arg;
            if (c->rxBinary) {
                arg = JS_NewArrayBufferCopy(srv->ctx, c->rxBuffer.data(), c->rxBuffer.size());
            } else {
                arg = JS_NewStringLen(srv->ctx,
                    reinterpret_cast<const char*>(c->rxBuffer.data()), c->rxBuffer.size());
            }
            fireEvent(srv->ctx, c->jsObj, "message", 1, &arg);
            JS_FreeValue(srv->ctx, arg);
            c->rxBuffer.clear();
        }
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (!srv || !psd || !psd->conn) break;
        WsConnection* c = psd->conn;
        if (c->closed || c->txQueue.empty()) break;
        auto& front = c->txQueue.front();
        size_t payloadLen = front.first.size() - LWS_PRE;
        enum lws_write_protocol wp = front.second ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
        int wrote = lws_write(wsi, front.first.data() + LWS_PRE, payloadLen, wp);
        c->txQueue.pop_front();
        if (wrote < static_cast<int>(payloadLen)) {
            // Short write — lws will request another writable callback; drop the
            // failed frame to avoid a half-sent message being followed by a new one.
            spdlog::warn("mb:ws: short lws_write ({}/{})", wrote, payloadLen);
        }
        if (!c->txQueue.empty())
            lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_CLOSED: {
        if (!srv || !psd || !psd->conn) break;
        WsConnection* c = psd->conn;
        if (c->closed) break;
        c->closed = true;
        if (srv->ctx && !srv->closed)
            destroyConnectionJs(srv->ctx, c, true);
        // Remove from server.connections
        for (auto it = srv->connections.begin(); it != srv->connections.end(); ++it) {
            if (it->get() == c) { srv->connections.erase(it); break; }
        }
        psd->conn = nullptr;
        break;
    }

    // ---- Foreign event loop fd management ----
    case LWS_CALLBACK_ADD_POLL_FD: {
        if (!mod->engine || !mod->engine->loop()) break;
        auto* pa = static_cast<struct lws_pollargs*>(in);
        EventLoop::FdEvents events = static_cast<EventLoop::FdEvents>(0);
        if (pa->events & POLLIN)  events = events | EventLoop::FdEvents::Readable;
        if (pa->events & POLLOUT) events = events | EventLoop::FdEvents::Writable;
        if (!static_cast<uint8_t>(events)) events = EventLoop::FdEvents::Readable;
        int fd = pa->fd;
        lws_context* ctxCopy = lwsCtx;
        mod->engine->loop()->watchFd(fd, events, [ctxCopy, fd](EventLoop::FdEvents fired) {
            struct lws_pollfd pfd{};
            pfd.fd = fd;
            pfd.events  = POLLIN | POLLOUT;
            pfd.revents = 0;
            if (fired & EventLoop::FdEvents::Readable) pfd.revents |= POLLIN;
            if (fired & EventLoop::FdEvents::Writable) pfd.revents |= POLLOUT;
            lws_service_fd(ctxCopy, &pfd);
        });
        break;
    }
    case LWS_CALLBACK_DEL_POLL_FD: {
        if (!mod->engine || !mod->engine->loop()) break;
        auto* pa = static_cast<struct lws_pollargs*>(in);
        mod->engine->loop()->removeFd(pa->fd);
        break;
    }
    case LWS_CALLBACK_CHANGE_MODE_POLL_FD: {
        if (!mod->engine || !mod->engine->loop()) break;
        auto* pa = static_cast<struct lws_pollargs*>(in);
        EventLoop::FdEvents events = static_cast<EventLoop::FdEvents>(0);
        if (pa->events & POLLIN)  events = events | EventLoop::FdEvents::Readable;
        if (pa->events & POLLOUT) events = events | EventLoop::FdEvents::Writable;
        if (!static_cast<uint8_t>(events)) events = EventLoop::FdEvents::Readable;
        mod->engine->loop()->updateFd(pa->fd, events);
        break;
    }

    default:
        break;
    }
    return 0;
}

// ============================================================================
// ws.createServer(opts)
// ============================================================================

static bool ensureLwsContext(ModuleState* mod)
{
    if (mod->ctx) return true;
    struct lws_context_creation_info info = {};
    info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
    info.user = mod;
    // Default vhost has no protocols; real protocols attach per-vhost at createServer time.
    lws_set_log_level(0, nullptr);
    mod->ctx = lws_create_context(&info);
    if (!mod->ctx) {
        spdlog::error("mb:ws: failed to create lws context");
        return false;
    }
    return true;
}

static JSValue jsWsCreateServer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    auto* eng  = engineFromJsCtx(ctx);
    auto* inst = eng ? eng->findInstanceByCtx(ctx) : nullptr;
    if (!inst) return JS_ThrowTypeError(ctx, "mb:ws: no script context");

    if (!(inst->permissions & Script::Perm::NetListenLocal))
        return JS_ThrowTypeError(ctx, "mb:ws: net.listen.local permission required");

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "mb:ws: createServer requires an options object");

    // Extract opts
    std::string host = "127.0.0.1";
    int port = 0;
    std::string token;

    JSValue hv = JS_GetPropertyStr(ctx, argv[0], "host");
    if (JS_IsString(hv)) {
        const char* s = JS_ToCString(ctx, hv); if (s) { host = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, hv);
    JSValue pv = JS_GetPropertyStr(ctx, argv[0], "port");
    if (JS_IsNumber(pv)) { int32_t n; if (JS_ToInt32(ctx, &n, pv) == 0 && n >= 0) port = n; }
    JS_FreeValue(ctx, pv);
    JSValue tv = JS_GetPropertyStr(ctx, argv[0], "token");
    if (JS_IsString(tv)) {
        const char* s = JS_ToCString(ctx, tv); if (s) { token = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, tv);

    if (token.empty())
        return JS_ThrowTypeError(ctx, "mb:ws: opts.token is required");
    if (host != "127.0.0.1" && host != "localhost")
        return JS_ThrowTypeError(ctx, "mb:ws: only loopback host (127.0.0.1) supported");

    auto* mod = ensureModuleState(eng);
    if (!ensureLwsContext(mod))
        return JS_ThrowTypeError(ctx, "mb:ws: lws context init failed");

    // Build server state
    auto srv = std::make_unique<WsServer>();
    srv->owner = inst->id;
    srv->ctx = ctx;
    srv->host = host;
    srv->token = token;
    srv->protocolName = std::string("mb-shell.") + token;

    // Protocols: [our-protocol, terminator]. lws requires the array to outlive the vhost.
    srv->protocols.resize(2);
    srv->protocols[0] = {};
    srv->protocols[0].name = srv->protocolName.c_str();
    srv->protocols[0].callback = wsCallback;
    srv->protocols[0].per_session_data_size = sizeof(PerSessionData);
    srv->protocols[0].rx_buffer_size = 65536;
    srv->protocols[1] = LWS_PROTOCOL_LIST_TERM;

    struct lws_context_creation_info vinfo = {};
    vinfo.port = port;
    vinfo.iface = srv->host.c_str();
    vinfo.protocols = srv->protocols.data();
    vinfo.user = srv.get();
    vinfo.vhost_name = srv->protocolName.c_str();

    srv->vhost = lws_create_vhost(mod->ctx, &vinfo);
    if (!srv->vhost)
        return JS_ThrowTypeError(ctx, "mb:ws: failed to bind on %s:%d",
                                  srv->host.c_str(), port);

    srv->boundPort = lws_get_vhost_listen_port(srv->vhost);

    // Build JS server object.
    JSValue obj = JS_NewObjectClass(ctx, jsWsServerClassId);
    JS_SetOpaque(obj, new JsWsServerData{srv.get()});
    JS_SetPropertyFunctionList(ctx, obj, jsWsServerFuncs,
                               sizeof(jsWsServerFuncs) / sizeof(jsWsServerFuncs[0]));
    srv->jsObj = JS_DupValue(ctx, obj);

    mod->servers.push_back(std::move(srv));
    return obj;
}

// ============================================================================
// Module registration
// ============================================================================

static int jsWsModuleInit(JSContext* ctx, JSModuleDef* m)
{
    JSValue wsObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, wsObj, "createServer",
        JS_NewCFunction(ctx, jsWsCreateServer, "createServer", 1));
    JS_SetModuleExport(ctx, m, "default", wsObj);
    JS_SetModuleExport(ctx, m, "createServer",
        JS_NewCFunction(ctx, jsWsCreateServer, "createServer", 1));
    return 0;
}

} // anonymous namespace

JSModuleDef* createWsNativeModule(JSContext* ctx, Script::Engine* /*eng*/)
{
    // Allocate class IDs + register classes once per runtime.
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (jsWsServerClassId == 0) {
        JS_NewClassID(rt, &jsWsServerClassId);
        JS_NewClass(rt, jsWsServerClassId, &jsWsServerClassDef);
    }
    if (jsWsConnClassId == 0) {
        JS_NewClassID(rt, &jsWsConnClassId);
        JS_NewClass(rt, jsWsConnClassId, &jsWsConnClassDef);
    }
    JSModuleDef* m = JS_NewCModule(ctx, "mb:ws", jsWsModuleInit);
    if (!m) return nullptr;
    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "createServer");
    return m;
}

void wsUnloadInstance(Script::Engine* eng, Script::InstanceId id)
{
    ModuleState* mod = getModuleState(eng);
    if (!mod) return;
    // Snapshot pointers; closeWsServer mutates mod->servers.
    std::vector<WsServer*> toClose;
    for (auto& s : mod->servers)
        if (s->owner == id) toClose.push_back(s.get());
    for (WsServer* s : toClose)
        closeWsServer(mod, s);
}

void wsDestroyEngine(Script::Engine* eng)
{
    auto it = g_modules.find(eng);
    if (it == g_modules.end()) return;
    ModuleState* mod = it->second.get();

    // Close all remaining servers (detaches vhosts).
    while (!mod->servers.empty())
        closeWsServer(mod, mod->servers.back().get());

    if (mod->ctx) {
        lws_context_destroy(mod->ctx);
        mod->ctx = nullptr;
    }
    g_modules.erase(it);
}
