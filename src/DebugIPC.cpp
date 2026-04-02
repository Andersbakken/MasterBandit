#include "DebugIPC.h"
#include "Terminal.h"
#include "Log.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <unistd.h>
#include <sys/stat.h>

using json = nlohmann::json;

// ============================================================================
// Per-connection helpers
// ============================================================================

DebugIPC::PerConnection* DebugIPC::findConnection(struct lws* wsi)
{
    for (auto& p : connections_) {
        if (p.first == wsi) return &p.second;
    }
    return nullptr;
}

DebugIPC::PerConnection* DebugIPC::addConnection(struct lws* wsi)
{
    connections_.push_back({wsi, PerConnection{}});
    return &connections_.back().second;
}

void DebugIPC::removeConnection(struct lws* wsi)
{
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (it->first == wsi) {
            connections_.erase(it);
            return;
        }
    }
}

// ============================================================================
// WebSocket callback
// ============================================================================

int DebugIPC::wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                         void* user, void* in, size_t len)
{
    struct lws_context* ctx = lws_get_context(wsi);
    DebugIPC* self = static_cast<DebugIPC*>(lws_context_user(ctx));
    if (!self) return 0;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        spdlog::debug("DebugIPC: client connected");
        self->addConnection(wsi);
        break;
    }
    case LWS_CALLBACK_CLOSED: {
        spdlog::debug("DebugIPC: client disconnected");
        if (self->pngWsi_ == wsi) {
            self->pngPending_ = false;
            self->pngWsi_ = nullptr;
        }
        self->removeConnection(wsi);
        break;
    }
    case LWS_CALLBACK_RECEIVE: {
        auto* conn = self->findConnection(wsi);
        if (!conn) break;
        conn->rxBuffer.append(static_cast<const char*>(in), len);
        if (lws_is_final_fragment(wsi)) {
            self->handleMessage(wsi, conn->rxBuffer);
            conn->rxBuffer.clear();
        }
        break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        auto* conn = self->findConnection(wsi);
        if (!conn || conn->txQueue.empty()) break;
        std::string& msg = conn->txQueue.front();
        std::vector<unsigned char> buf(LWS_PRE + msg.size());
        memcpy(buf.data() + LWS_PRE, msg.data(), msg.size());
        lws_write(wsi, buf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
        conn->txQueue.erase(conn->txQueue.begin());
        if (!conn->txQueue.empty()) {
            lws_callback_on_writable(wsi);
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

static const struct lws_protocols sProtocols[] = {
    { "mb-debug", DebugIPC::wsCallback, 0, 65536 },
    LWS_PROTOCOL_LIST_TERM
};

DebugIPC::DebugIPC(uv_loop_t* loop, Terminal* terminal, GridCallback gridCb)
    : terminal_(terminal)
    , gridCb_(std::move(gridCb))
{
    socketPath_ = "/tmp/mb-" + std::to_string(getpid()) + ".sock";

    // Remove stale socket if it exists
    unlink(socketPath_.c_str());

    struct lws_context_creation_info info = {};
    info.options = LWS_SERVER_OPTION_LIBUV | LWS_SERVER_OPTION_UNIX_SOCK;
    info.iface = socketPath_.c_str();
    info.port = 0;
    info.protocols = sProtocols;
    info.user = this;
    info.foreign_loops = reinterpret_cast<void**>(&loop);

    ctx_ = lws_create_context(&info);
    if (!ctx_) {
        spdlog::error("DebugIPC: failed to create lws context");
        return;
    }

    // Set up async handle for thread-safe log forwarding
    uv_async_init(loop, &logAsync_, [](uv_async_t* handle) {
        auto* self = static_cast<DebugIPC*>(handle->data);
        std::deque<std::string> msgs;
        {
            std::lock_guard<std::mutex> lock(self->logMutex_);
            msgs.swap(self->logQueue_);
        }
        for (const auto& msg : msgs) {
            json j;
            j["type"] = "log";
            j["msg"] = msg;
            std::string payload = j.dump();
            for (auto& p : self->connections_) {
                if (p.second.subscribedToLogs) {
                    p.second.txQueue.push_back(payload);
                    lws_callback_on_writable(p.first);
                }
            }
        }
    });
    logAsync_.data = this;

    spdlog::info("DebugIPC: listening on {}", socketPath_);
}

DebugIPC::~DebugIPC()
{
    if (ctx_) {
        lws_context_destroy(ctx_);
    }
    unlink(socketPath_.c_str());

    uv_close(reinterpret_cast<uv_handle_t*>(&logAsync_), nullptr);
}

// ============================================================================
// Message handling
// ============================================================================

void DebugIPC::handleMessage(struct lws* wsi, const std::string& msg)
{
    json j;
    try {
        j = json::parse(msg);
    } catch (const json::parse_error&) {
        json err;
        err["type"] = "error";
        err["msg"] = "invalid JSON";
        sendResponse(wsi, err.dump());
        return;
    }

    std::string cmd = j.value("cmd", "");
    int id = j.value("id", 0);

    if (cmd == "screenshot") {
        std::string format = j.value("format", "grid");
        if (format == "png") {
            cmdScreenshotPng(wsi, id);
        } else {
            cmdScreenshotGrid(wsi, id);
        }
    } else if (cmd == "key") {
        std::string text = j.value("text", "");
        std::string key = j.value("key", "");
        std::vector<std::string> mods;
        if (j.contains("mods") && j["mods"].is_array()) {
            for (const auto& m : j["mods"]) {
                if (m.is_string()) mods.push_back(m.get<std::string>());
            }
        }
        cmdSendKey(wsi, id, text, key, mods);
    } else if (cmd == "subscribe") {
        std::string channel = j.value("channel", "");
        if (channel == "logs") {
            cmdSubscribeLogs(wsi, id);
        } else {
            json err;
            err["type"] = "error";
            err["id"] = id;
            err["msg"] = "unknown channel";
            sendResponse(wsi, err.dump());
        }
    } else if (cmd == "unsubscribe") {
        std::string channel = j.value("channel", "");
        if (channel == "logs") {
            cmdUnsubscribeLogs(wsi, id);
        } else {
            json err;
            err["type"] = "error";
            err["id"] = id;
            err["msg"] = "unknown channel";
            sendResponse(wsi, err.dump());
        }
    } else {
        json err;
        err["type"] = "error";
        err["id"] = id;
        err["msg"] = "unknown command: " + cmd;
        sendResponse(wsi, err.dump());
    }
}

void DebugIPC::sendResponse(struct lws* wsi, const std::string& jsonStr)
{
    auto* conn = findConnection(wsi);
    if (!conn) return;
    conn->txQueue.push_back(jsonStr);
    lws_callback_on_writable(wsi);
}

// ============================================================================
// Command: screenshot (grid)
// ============================================================================

void DebugIPC::cmdScreenshotGrid(struct lws* wsi, int id)
{
    if (gridCb_) {
        sendResponse(wsi, gridCb_(id));
    } else {
        json err;
        err["type"] = "error";
        err["id"] = id;
        err["msg"] = "grid screenshot not available";
        sendResponse(wsi, err.dump());
    }
}

// ============================================================================
// Command: screenshot (png)
// ============================================================================

void DebugIPC::cmdScreenshotPng(struct lws* wsi, int id)
{
    if (pngPending_) {
        json err;
        err["type"] = "error";
        err["id"] = id;
        err["msg"] = "screenshot already pending";
        sendResponse(wsi, err.dump());
        return;
    }

    pngPending_ = true;
    pngId_ = id;
    pngWsi_ = wsi;
    // Readback happens in TerminalWindow::renderTerminal() on next frame
}

void DebugIPC::onPngReady(const std::string& base64Png)
{
    if (!pngPending_ || !pngWsi_) return;

    json resp;
    resp["type"] = "screenshot";
    resp["format"] = "png";
    if (pngId_) resp["id"] = pngId_;
    resp["data"] = base64Png;

    sendResponse(pngWsi_, resp.dump());

    pngPending_ = false;
    pngWsi_ = nullptr;
    pngId_ = 0;
}

// ============================================================================
// Command: key
// ============================================================================

static Key nameToKey(const std::string& name)
{
    if (name == "Return" || name == "Enter") return Key_Return;
    if (name == "Backspace") return Key_Backspace;
    if (name == "Tab") return Key_Tab;
    if (name == "Escape") return Key_Escape;
    if (name == "Delete") return Key_Delete;
    if (name == "Left") return Key_Left;
    if (name == "Right") return Key_Right;
    if (name == "Up") return Key_Up;
    if (name == "Down") return Key_Down;
    if (name == "Home") return Key_Home;
    if (name == "End") return Key_End;
    if (name == "PageUp") return Key_PageUp;
    if (name == "PageDown") return Key_PageDown;
    if (name == "Insert") return Key_Insert;
    if (name == "F1") return Key_F1;
    if (name == "F2") return Key_F2;
    if (name == "F3") return Key_F3;
    if (name == "F4") return Key_F4;
    if (name == "F5") return Key_F5;
    if (name == "F6") return Key_F6;
    if (name == "F7") return Key_F7;
    if (name == "F8") return Key_F8;
    if (name == "F9") return Key_F9;
    if (name == "F10") return Key_F10;
    if (name == "F11") return Key_F11;
    if (name == "F12") return Key_F12;
    if (name == "Space") return Key_Space;
    return Key_unknown;
}

void DebugIPC::cmdSendKey(struct lws* wsi, int id, const std::string& text,
                          const std::string& key, const std::vector<std::string>& mods)
{
    if (!text.empty()) {
        for (char c : text) {
            KeyEvent ev;
            ev.key = Key_unknown;
            ev.text = std::string(1, c);
            ev.count = 1;
            ev.autoRepeat = false;
            terminal_->keyPressEvent(&ev);
        }
    } else if (!key.empty()) {
        Key k = nameToKey(key);
        if (k == Key_unknown && key.size() == 1) {
            KeyEvent ev;
            ev.key = Key_unknown;
            ev.text = key;
            ev.count = 1;
            ev.autoRepeat = false;

            bool ctrl = false;
            for (const auto& m : mods) {
                if (m == "ctrl" || m == "control") ctrl = true;
            }
            if (ctrl && key[0] >= 'a' && key[0] <= 'z') {
                ev.text = std::string(1, static_cast<char>(key[0] - 'a' + 1));
            } else if (ctrl && key[0] >= 'A' && key[0] <= 'Z') {
                ev.text = std::string(1, static_cast<char>(key[0] - 'A' + 1));
            }

            terminal_->keyPressEvent(&ev);
        } else if (k != Key_unknown) {
            KeyEvent ev;
            ev.key = k;
            ev.count = 1;
            ev.autoRepeat = false;
            terminal_->keyPressEvent(&ev);
        }
    }

    json resp;
    resp["type"] = "ok";
    if (id) resp["id"] = id;
    sendResponse(wsi, resp.dump());
}

// ============================================================================
// Command: subscribe/unsubscribe logs
// ============================================================================

void DebugIPC::cmdSubscribeLogs(struct lws* wsi, int id)
{
    auto* conn = findConnection(wsi);
    if (conn) conn->subscribedToLogs = true;

    json resp;
    resp["type"] = "ok";
    if (id) resp["id"] = id;
    sendResponse(wsi, resp.dump());
}

void DebugIPC::cmdUnsubscribeLogs(struct lws* wsi, int id)
{
    auto* conn = findConnection(wsi);
    if (conn) conn->subscribedToLogs = false;

    json resp;
    resp["type"] = "ok";
    if (id) resp["id"] = id;
    sendResponse(wsi, resp.dump());
}

// ============================================================================
// Log broadcasting (called from any thread)
// ============================================================================

void DebugIPC::broadcastLog(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        logQueue_.push_back(msg);
    }
    uv_async_send(&logAsync_);
}
