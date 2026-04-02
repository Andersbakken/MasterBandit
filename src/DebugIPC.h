#ifndef DEBUGIPC_H
#define DEBUGIPC_H

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>

#include <uv.h>
#include <libwebsockets.h>

class Terminal;

class DebugIPC {
public:
    // gridCallback: called with (id) → returns JSON string for grid screenshot
    using GridCallback = std::function<std::string(int id)>;

    DebugIPC(uv_loop_t* loop, Terminal* terminal, GridCallback gridCb);
    ~DebugIPC();

    void broadcastLog(const std::string& msg);
    const std::string& socketPath() const { return socketPath_; }

    // Called by TerminalWindow when a PNG readback completes
    void onPngReady(const std::string& base64Png);

    // Returns true if a PNG screenshot is pending
    bool pngScreenshotPending() const { return pngPending_; }
    int pngScreenshotId() const { return pngId_; }
    void clearPngPending() { pngPending_ = false; }

    // Public because it's referenced from the static protocol table
    static int wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                          void* user, void* in, size_t len);

private:
    void handleMessage(struct lws* wsi, const std::string& msg);
    void sendResponse(struct lws* wsi, const std::string& json);

    // Command handlers
    void cmdScreenshotPng(struct lws* wsi, int id);
    void cmdScreenshotGrid(struct lws* wsi, int id);
    void cmdSendKey(struct lws* wsi, int id, const std::string& text,
                    const std::string& key, const std::vector<std::string>& mods);
    void cmdSubscribeLogs(struct lws* wsi, int id);
    void cmdUnsubscribeLogs(struct lws* wsi, int id);

    struct lws_context* ctx_ = nullptr;
    std::string socketPath_;
    Terminal* terminal_;
    GridCallback gridCb_;

    struct PerConnection {
        std::string rxBuffer;
        bool subscribedToLogs = false;
        std::vector<std::string> txQueue;
    };

    // Map from lws* to per-connection data
    std::vector<std::pair<struct lws*, PerConnection>> connections_;

    PerConnection* findConnection(struct lws* wsi);
    PerConnection* addConnection(struct lws* wsi);
    void removeConnection(struct lws* wsi);

    // PNG screenshot state
    bool pngPending_ = false;
    int pngId_ = 0;
    struct lws* pngWsi_ = nullptr;

    // Thread-safe log forwarding
    uv_async_t logAsync_;
    std::mutex logMutex_;
    std::deque<std::string> logQueue_;
};

#endif /* DEBUGIPC_H */
