#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dbus/dbus.h>

// Single-connection D-Bus session-bus bridge running on a private worker
// thread. Other threads submit work via sendAsync / addSignalMatch; reply
// and signal callbacks fire on the worker thread, never on the caller's
// thread. Callers that need main-thread fan-out should marshal via
// EventLoop::post inside their callback.
//
// libdbus's watch/timeout/dispatch hooks are wired into a private epoll
// owned by this bridge. The worker thread is the only thread that touches
// libdbus state (after construction); cross-thread work is funnelled
// through a small outbox + an eventfd wake.
class DBusBridge {
public:
    // Reply callback. `reply` is the incoming message (caller borrows;
    // libdbus owns it for the duration of the callback). `ok` is false on
    // any error reply, transport failure, or pending-call cancellation.
    using ReplyCb = std::function<void(DBusMessage* reply, bool ok)>;

    // Signal callback. `msg` is the matched signal message (caller
    // borrows). Multiple matches on the same rule get distinct callbacks;
    // the bridge dispatches in registration order.
    using SignalCb = std::function<void(DBusMessage* msg)>;

    DBusBridge();
    ~DBusBridge();

    DBusBridge(const DBusBridge&) = delete;
    DBusBridge& operator=(const DBusBridge&) = delete;

    // Whether the session bus connection opened. If false, sendAsync /
    // addSignalMatch are silent no-ops.
    bool connected() const { return conn_ != nullptr; }

    // Steals the message reference (this call calls dbus_message_unref on
    // it after sending). cb may be empty for fire-and-forget.
    void sendAsync(DBusMessage* msg, ReplyCb cb);

    // Add a signal-match rule (e.g.
    // "type='signal',interface='org.freedesktop.portal.Settings',"
    // "member='SettingChanged'"). Posts an AddMatch to the bus. The
    // SignalCb receives any signal message that matches *any* installed
    // rule — it is the caller's job to filter further by interface/member.
    void addSignalMatch(const std::string& matchRule, SignalCb cb);

private:
    void threadMain();
    void drainOutbox();
    void runReadyTimeouts();
    int  msUntilNextTimeout();
    void dispatchAll();
    void rebuildFdMask(int fd);

    // libdbus hooks (static trampolines)
    static dbus_bool_t s_add_watch(DBusWatch*, void*);
    static void        s_remove_watch(DBusWatch*, void*);
    static void        s_toggle_watch(DBusWatch*, void*);
    static dbus_bool_t s_add_timeout(DBusTimeout*, void*);
    static void        s_remove_timeout(DBusTimeout*, void*);
    static void        s_toggle_timeout(DBusTimeout*, void*);
    static void        s_wakeup_main(void*);
    static DBusHandlerResult s_filter(DBusConnection*, DBusMessage*, void*);
    static void        s_pending_notify(DBusPendingCall*, void*);

    DBusConnection*    conn_   = nullptr;
    int                epollFd_ = -1;
    int                wakeFd_  = -1;
    std::atomic<bool>  stop_{false};
    std::thread        thread_;

    std::mutex                          outMu_;
    std::vector<std::function<void()>>  outbox_;

    // Watch tables (worker-thread only after construction).
    // Multiple DBusWatch* may share an fd (one for read, one for write).
    std::unordered_set<DBusWatch*>             allWatches_;
    std::unordered_map<DBusWatch*, int>        watchFd_;
    std::unordered_map<int, std::unordered_set<DBusWatch*>> fdWatches_;
    std::unordered_map<int, uint32_t>          fdEpollMask_;

    struct TimeoutInfo {
        uint64_t deadlineNs;   // monotonic, when next to fire
        bool     enabled;
    };
    std::unordered_map<DBusTimeout*, TimeoutInfo> timeouts_;

    std::vector<std::pair<std::string, SignalCb>> signalCbs_;
};
