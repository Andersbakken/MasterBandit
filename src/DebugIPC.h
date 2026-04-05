#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>

#include <uv.h>
#include <libwebsockets.h>
#include <glaze/glaze.hpp>

class Terminal;

class DebugIPC {
public:
    using GridCallback     = std::function<std::string(int id)>;
    using StatsCallback    = std::function<std::string(int id)>;
    using TerminalCallback = std::function<Terminal*()>;
    using ActionCallback   = std::function<bool(const std::string& action, const std::vector<std::string>& args)>;

    DebugIPC(uv_loop_t* loop, TerminalCallback termCb, GridCallback gridCb,
             StatsCallback statsCb = {}, ActionCallback actionCb = {});
    ~DebugIPC();

    // Issue uv_close for owned handles. Must call before final uv_run.
    void closeHandles();

    void broadcastLog(const std::string& msg);
    const std::string& socketPath() const { return socketPath_; }

    // Called by TerminalWindow when a PNG readback completes
    void onPngReady(const std::string& base64Png);

    // Returns true if a PNG screenshot was requested and readback hasn't started
    bool pngScreenshotPending() const { return pngPending_ && !pngReadbackInProgress_; }
    int pngScreenshotId() const { return pngId_; }
    void markReadbackInProgress() { pngReadbackInProgress_ = true; }

    // Target and cell-rect for targeted screenshots
    const std::string& pngTarget() const { return pngTarget_; }
    struct CellRect { int x = 0, y = 0, w = 0, h = 0; bool valid = false; };
    const CellRect& pngCellRect() const { return pngCellRect_; }

    // Public because it's referenced from the static protocol table
    static int wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                          void* user, void* in, size_t len);

private:
    void handleMessage(struct lws* wsi, const std::string& msg);
    void sendResponse(struct lws* wsi, const std::string& json);

    // Command handlers
    void cmdScreenshotPng(struct lws* wsi, int id, const glz::generic& j);
    void cmdScreenshotGrid(struct lws* wsi, int id);
    void cmdSendKey(struct lws* wsi, int id, const std::string& text,
                    const std::string& key, const std::vector<std::string>& mods);
    void cmdStats(struct lws* wsi, int id);
    void cmdAction(struct lws* wsi, int id, const std::string& action,
                   const std::vector<std::string>& args);
    void cmdSubscribeLogs(struct lws* wsi, int id);
    void cmdUnsubscribeLogs(struct lws* wsi, int id);

    struct lws_context* ctx_ = nullptr;
    std::string socketPath_;
    TerminalCallback termCb_;
    GridCallback gridCb_;
    StatsCallback statsCb_;
    ActionCallback actionCb_;

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
    bool pngReadbackInProgress_ = false;
    int pngId_ = 0;
    struct lws* pngWsi_ = nullptr;
    std::string pngTarget_;
    CellRect pngCellRect_;

    // Thread-safe log forwarding
    uv_async_t logAsync_;
    std::mutex logMutex_;
    std::deque<std::string> logQueue_;
};
