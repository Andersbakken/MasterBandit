#include "MBConnection.h"
#include "Utils.h"

#include <glaze/glaze.hpp>

#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <chrono>
#include <thread>



// ============================================================================
// WebSocket callback
// ============================================================================

static const struct lws_protocols sProtos[] = {
    { "mb-debug", MBConnection::wsCallback, 0, 65536 },
    LWS_PROTOCOL_LIST_TERM
};

int MBConnection::wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                           void* /*user*/, void* in, size_t len)
{
    struct lws_context* ctx = lws_get_context(wsi);
    MBConnection* self = static_cast<MBConnection*>(lws_context_user(ctx));
    if (!self) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        self->connected_ = true;
        self->wsi_ = wsi;
        if (self->txPending_)
            lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (self->txPending_ && !self->pendingTx_.empty()) {
            std::vector<unsigned char> buf(LWS_PRE + self->pendingTx_.size());
            memcpy(buf.data() + LWS_PRE, self->pendingTx_.data(), self->pendingTx_.size());
            lws_write(wsi, buf.data() + LWS_PRE, self->pendingTx_.size(), LWS_WRITE_TEXT);
            self->pendingTx_.clear();
            self->txPending_ = false;
        }
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        self->rxBuffer_.append(static_cast<const char*>(in), len);
        if (lws_is_final_fragment(wsi)) {
            self->lastResponse_ = std::move(self->rxBuffer_);
            self->rxBuffer_.clear();
            self->responseReady_ = true;
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
        self->wsi_ = nullptr;
        self->connected_ = false;
        break;

    default:
        break;
    }
    return 0;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

MBConnection::MBConnection() : MBConnection(Options{}) {}

MBConnection::MBConnection(const Options& opts)
{
    std::string fontPath = opts.fontPath.empty() ? MB_TEST_FONT : opts.fontPath;
    std::string mbBinary = MB_BINARY;

    std::string colsStr = std::to_string(opts.cols);
    std::string rowsStr = std::to_string(opts.rows);
    std::string fontSizeStr = std::to_string(opts.fontSize);

    pid_ = fork();
    if (pid_ == 0) {
        // Child: exec mb --test
        execl(mbBinary.c_str(), mbBinary.c_str(), "--test",
              "--font", fontPath.c_str(),
              "--cols", colsStr.c_str(),
              "--rows", rowsStr.c_str(),
              "--font-size", fontSizeStr.c_str(),
              nullptr);
        _exit(127);
    }

    socketPath_ = "/tmp/mb-" + std::to_string(pid_) + ".sock";
}

MBConnection::~MBConnection()
{
    // Disconnect WebSocket
    if (ctx_) {
        lws_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    uv_loop_close(&loop_);

    // Kill child and clean up its socket
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int status;
        waitpid(pid_, &status, 0);
    }
    unlink(socketPath_.c_str());
}

// ============================================================================
// Connect
// ============================================================================

bool MBConnection::connect(int timeoutMs)
{
    // Poll for socket file existence
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            struct stat st;
            if (stat(socketPath_.c_str(), &st) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        struct stat st;
        if (stat(socketPath_.c_str(), &st) != 0) return false;
    }

    // Create libuv loop and lws client context
    uv_loop_init(&loop_);

    uv_loop_t* loopPtr = &loop_;
    struct lws_context_creation_info info = {};
    info.options = LWS_SERVER_OPTION_LIBUV;
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = sProtos;
    info.user = this;
    info.foreign_loops = reinterpret_cast<void**>(&loopPtr);

    lws_set_log_level(0, nullptr);
    ctx_ = lws_create_context(&info);
    if (!ctx_) return false;

    // Connect to Unix socket
    std::string unixAddr = "+" + socketPath_;
    struct lws_client_connect_info ccinfo = {};
    ccinfo.context = ctx_;
    ccinfo.address = unixAddr.c_str();
    ccinfo.port = 0;
    ccinfo.path = "/";
    ccinfo.host = "localhost";
    ccinfo.origin = "localhost";
    ccinfo.protocol = "mb-debug";
    ccinfo.local_protocol_name = "mb-debug";
    ccinfo.ssl_connection = 0;

    struct lws* wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) return false;

    // Poll the loop until connected or timeout (NOWAIT keeps handles alive between calls)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!connected_ && std::chrono::steady_clock::now() < deadline) {
        uv_run(&loop_, UV_RUN_NOWAIT);
        if (!connected_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return connected_;
}

// ============================================================================
// Send request and wait for response
// ============================================================================

std::string MBConnection::sendRequest(const std::string& json, int timeoutMs)
{
    if (!connected_ || !wsi_) return {};

    responseReady_ = false;
    lastResponse_.clear();
    pendingTx_ = json;
    txPending_ = true;
    lws_callback_on_writable(wsi_);

    // Poll until response or timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!responseReady_ && connected_ && std::chrono::steady_clock::now() < deadline) {
        uv_run(&loop_, UV_RUN_NOWAIT);
        if (!responseReady_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!responseReady_ && pid_ > 0) {
        // Check if child crashed
        int status = 0;
        pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r > 0) pid_ = -1;
    }
    return responseReady_ ? lastResponse_ : std::string{};
}

// ============================================================================
// Public API
// ============================================================================

bool MBConnection::sendAction(const std::string& action, const std::vector<std::string>& args, int timeoutMs)
{
    glz::generic::object_t req;
    req["cmd"] = "action";
    req["action"] = action;
    req["id"] = static_cast<double>(nextId_++);

    if (!args.empty()) {
        glz::generic::array_t argsArr;
        for (const auto& a : args) argsArr.emplace_back(a);
        req["args"] = std::move(argsArr);
    }

    std::string json;
    (void)glz::write_json(req, json);
    auto resp = sendRequest(json, timeoutMs);
    if (resp.empty()) return false;

    // Check "ok" field in response
    glz::generic j;
    if (glz::read_json(j, resp)) return false;
    if (auto* obj = std::get_if<glz::generic::object_t>(&j.data)) {
        auto it = obj->find("ok");
        if (it != obj->end()) {
            if (auto* b = std::get_if<bool>(&it->second.data)) {
                return *b;
            }
        }
    }
    return false;
}

bool MBConnection::injectData(const std::string& data, int timeoutMs)
{
    // Base64-encode to avoid JSON control character issues
    std::string b64 = base64::encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());

    glz::generic::object_t req;
    req["cmd"] = "inject";
    req["data"] = b64;
    req["id"] = static_cast<double>(nextId_++);

    std::string json;
    (void)glz::write_json(req, json);
    auto resp = sendRequest(json, timeoutMs);
    return !resp.empty();
}

std::string MBConnection::queryStats(int timeoutMs)
{
    glz::generic::object_t req;
    req["cmd"] = "stats";
    req["id"] = static_cast<double>(nextId_++);

    std::string json;
    (void)glz::write_json(req, json);
    return sendRequest(json, timeoutMs);
}

void MBConnection::wait(int ms)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (connected_ && std::chrono::steady_clock::now() < deadline) {
        uv_run(&loop_, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool MBConnection::reset(int timeoutMs)
{
    // Send RIS (Reset Initial State) escape sequence via the PTY
    return sendText("\033c", timeoutMs);
}

MBConnection& MBConnection::shared()
{
    static MBConnection instance({.cols = 40, .rows = 10});
    static bool initialized = false;
    if (!initialized) {
        initialized = instance.connect();
    }
    return instance;
}

bool MBConnection::sendText(const std::string& text, int timeoutMs)
{
    glz::generic::object_t req;
    req["cmd"] = "key";
    req["text"] = text;
    req["id"] = static_cast<double>(nextId_++);

    std::string json;
    (void)glz::write_json(req, json);
    auto resp = sendRequest(json, timeoutMs);
    return !resp.empty();
}

bool MBConnection::sendKey(const std::string& key, const std::vector<std::string>& mods, int timeoutMs)
{
    glz::generic::object_t req;
    req["cmd"] = "key";
    req["key"] = key;
    req["id"] = static_cast<double>(nextId_++);

    if (!mods.empty()) {
        glz::generic::array_t modsArr;
        for (const auto& m : mods) modsArr.emplace_back(m);
        req["mods"] = std::move(modsArr);
    }

    std::string json;
    (void)glz::write_json(req, json);
    auto resp = sendRequest(json, timeoutMs);
    return !resp.empty();
}

std::vector<uint8_t> MBConnection::doScreenshot(const std::string& target,
                                               int x, int y, int w, int h,
                                               bool hasRect, int timeoutMs)
{
    glz::generic::object_t req;
    req["cmd"] = "screenshot";
    req["format"] = "png";
    req["id"] = static_cast<double>(nextId_++);

    if (!target.empty())
        req["target"] = target;

    if (hasRect) {
        glz::generic::object_t rect;
        rect["x"] = static_cast<double>(x);
        rect["y"] = static_cast<double>(y);
        rect["w"] = static_cast<double>(w);
        rect["h"] = static_cast<double>(h);
        req["rect"] = std::move(rect);
    }

    std::string json;
    (void)glz::write_json(req, json);
    auto resp = sendRequest(json, timeoutMs);
    if (resp.empty()) return {};

    // Parse response and extract base64 data
    glz::generic j;
    if (glz::read_json(j, resp)) return {};

    if (auto* obj = std::get_if<glz::generic::object_t>(&j.data)) {
        auto it = obj->find("data");
        if (it != obj->end()) {
            if (auto* s = std::get_if<std::string>(&it->second.data)) {
                if (s->empty()) return {};
                return base64::decode(*s);
            }
        }
    }
    return {};
}

std::vector<uint8_t> MBConnection::screenshotPng(int timeoutMs)
{
    return doScreenshot("", 0, 0, 0, 0, false, timeoutMs);
}

std::vector<uint8_t> MBConnection::screenshotPane(int paneId, int timeoutMs)
{
    return doScreenshot("pane:" + std::to_string(paneId), 0, 0, 0, 0, false, timeoutMs);
}

std::vector<uint8_t> MBConnection::screenshotRect(int x, int y, int w, int h, int timeoutMs)
{
    return doScreenshot("", x, y, w, h, true, timeoutMs);
}

std::vector<uint8_t> MBConnection::screenshotPaneRect(int paneId, int x, int y, int w, int h, int timeoutMs)
{
    return doScreenshot("pane:" + std::to_string(paneId), x, y, w, h, true, timeoutMs);
}

// ============================================================================
// PNG comparison
// ============================================================================

// stb_image for decoding — STB_IMAGE_IMPLEMENTATION is defined in terminal/OSC.cpp
#include <stb_image.h>

int MBConnection::comparePng(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int wa, ha, ca, wb, hb, cb;
    uint8_t* pixA = stbi_load_from_memory(a.data(), static_cast<int>(a.size()), &wa, &ha, &ca, 4);
    uint8_t* pixB = stbi_load_from_memory(b.data(), static_cast<int>(b.size()), &wb, &hb, &cb, 4);

    if (!pixA || !pixB || wa != wb || ha != hb) {
        stbi_image_free(pixA);
        stbi_image_free(pixB);
        return 255; // max difference if sizes mismatch
    }

    int maxDiff = 0;
    size_t total = static_cast<size_t>(wa) * ha * 4;
    for (size_t i = 0; i < total; i++) {
        int d = std::abs(static_cast<int>(pixA[i]) - static_cast<int>(pixB[i]));
        if (d > maxDiff) maxDiff = d;
    }

    stbi_image_free(pixA);
    stbi_image_free(pixB);
    return maxDiff;
}

bool MBConnection::matchesReference(const std::vector<uint8_t>& png, const std::string& refName, int tolerance)
{
    std::string refPath = std::string(MB_REF_DIR) + "/" + refName;

    // If MB_UPDATE_REFS=1, save the actual as the new reference
    if (const char* env = getenv("MB_UPDATE_REFS")) {
        if (std::string(env) == "1") {
            savePng(png, refPath);
            return true;
        }
    }

    auto ref = loadPng(refPath);
    if (ref.empty()) return false;

    return comparePng(png, ref) <= tolerance;
}

void MBConnection::savePng(const std::vector<uint8_t>& png, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), png.size());
}

std::vector<uint8_t> MBConnection::loadPng(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}
