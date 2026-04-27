#include "DBusBridge.h"

#include <spdlog/spdlog.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <time.h>

#include <cerrno>
#include <cstring>

namespace {

uint64_t nowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

uint32_t epollMaskFromDbus(unsigned dbusFlags)
{
    uint32_t m = 0;
    if (dbusFlags & DBUS_WATCH_READABLE) m |= EPOLLIN;
    if (dbusFlags & DBUS_WATCH_WRITABLE) m |= EPOLLOUT;
    return m;
}

unsigned dbusFlagsFromEpoll(uint32_t epollEvents)
{
    unsigned f = 0;
    if (epollEvents & EPOLLIN)  f |= DBUS_WATCH_READABLE;
    if (epollEvents & EPOLLOUT) f |= DBUS_WATCH_WRITABLE;
    if (epollEvents & EPOLLERR) f |= DBUS_WATCH_ERROR;
    if (epollEvents & EPOLLHUP) f |= DBUS_WATCH_HANGUP;
    return f;
}

} // namespace

DBusBridge::DBusBridge()
{
    // Process-wide thread support for libdbus. Safe to call repeatedly.
    dbus_threads_init_default();

    DBusError err;
    dbus_error_init(&err);

    // Private connection so we own the lifetime; shared (dbus_bus_get) gets
    // tangled with arbitrary other consumers in the process.
    conn_ = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn_ || dbus_error_is_set(&err)) {
        spdlog::warn("DBusBridge: connect to session bus failed: {}",
                     err.message ? err.message : "(unknown)");
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        return;
    }

    // We drive the lifetime; don't let libdbus exit() on disconnect.
    dbus_connection_set_exit_on_disconnect(conn_, FALSE);

    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        spdlog::warn("DBusBridge: epoll_create1: {}", strerror(errno));
        dbus_connection_close(conn_);
        dbus_connection_unref(conn_);
        conn_ = nullptr;
        return;
    }

    wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeFd_ < 0) {
        spdlog::warn("DBusBridge: eventfd: {}", strerror(errno));
        close(epollFd_); epollFd_ = -1;
        dbus_connection_close(conn_);
        dbus_connection_unref(conn_);
        conn_ = nullptr;
        return;
    }

    epoll_event wev{};
    wev.events = EPOLLIN;
    wev.data.fd = wakeFd_;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &wev);

    // Install hooks. Watch/timeout add callbacks may fire synchronously
    // here as libdbus exposes its current set.
    if (!dbus_connection_set_watch_functions(conn_,
            &DBusBridge::s_add_watch,
            &DBusBridge::s_remove_watch,
            &DBusBridge::s_toggle_watch,
            this, nullptr)) {
        spdlog::warn("DBusBridge: set_watch_functions OOM");
    }
    if (!dbus_connection_set_timeout_functions(conn_,
            &DBusBridge::s_add_timeout,
            &DBusBridge::s_remove_timeout,
            &DBusBridge::s_toggle_timeout,
            this, nullptr)) {
        spdlog::warn("DBusBridge: set_timeout_functions OOM");
    }
    dbus_connection_set_wakeup_main_function(conn_,
        &DBusBridge::s_wakeup_main, this, nullptr);

    if (!dbus_connection_add_filter(conn_, &DBusBridge::s_filter, this, nullptr)) {
        spdlog::warn("DBusBridge: add_filter OOM");
    }

    thread_ = std::thread([this] { threadMain(); });
}

DBusBridge::~DBusBridge()
{
    stop_.store(true, std::memory_order_release);
    if (wakeFd_ >= 0) {
        uint64_t one = 1;
        [[maybe_unused]] auto n = write(wakeFd_, &one, sizeof(one));
    }
    if (thread_.joinable()) thread_.join();

    if (conn_) {
        dbus_connection_remove_filter(conn_, &DBusBridge::s_filter, this);
        dbus_connection_set_watch_functions(conn_, nullptr, nullptr, nullptr, nullptr, nullptr);
        dbus_connection_set_timeout_functions(conn_, nullptr, nullptr, nullptr, nullptr, nullptr);
        dbus_connection_set_wakeup_main_function(conn_, nullptr, nullptr, nullptr);
        dbus_connection_close(conn_);
        dbus_connection_unref(conn_);
        conn_ = nullptr;
    }
    if (wakeFd_ >= 0)  { close(wakeFd_);  wakeFd_  = -1; }
    if (epollFd_ >= 0) { close(epollFd_); epollFd_ = -1; }
}

// ---------- public API ----------

void DBusBridge::sendAsync(DBusMessage* msg, ReplyCb cb)
{
    if (!conn_) {
        if (msg) dbus_message_unref(msg);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(outMu_);
        outbox_.push_back([this, msg, cb = std::move(cb)]() mutable {
            DBusPendingCall* pending = nullptr;
            // -1 = libdbus default reply timeout.
            if (!dbus_connection_send_with_reply(conn_, msg, &pending, -1) || !pending) {
                spdlog::warn("DBusBridge: send_with_reply failed (out of memory or disconnected)");
                if (cb) cb(nullptr, false);
                dbus_message_unref(msg);
                return;
            }
            auto* heapCb = new ReplyCb(std::move(cb));
            if (!dbus_pending_call_set_notify(pending,
                    &DBusBridge::s_pending_notify, heapCb,
                    [](void* p) { delete static_cast<ReplyCb*>(p); })) {
                spdlog::warn("DBusBridge: pending_call_set_notify OOM");
                delete heapCb;
                if (cb) cb(nullptr, false);
            }
            dbus_pending_call_unref(pending);
            dbus_message_unref(msg);
        });
    }
    uint64_t one = 1;
    [[maybe_unused]] auto n = write(wakeFd_, &one, sizeof(one));
}

void DBusBridge::addSignalMatch(const std::string& matchRule, SignalCb cb)
{
    if (!conn_) return;
    {
        std::lock_guard<std::mutex> lk(outMu_);
        outbox_.push_back([this, matchRule, cb = std::move(cb)]() mutable {
            DBusError err;
            dbus_error_init(&err);
            dbus_bus_add_match(conn_, matchRule.c_str(), &err);
            if (dbus_error_is_set(&err)) {
                spdlog::warn("DBusBridge: add_match '{}' failed: {}",
                             matchRule, err.message);
                dbus_error_free(&err);
                return;
            }
            signalCbs_.emplace_back(matchRule, std::move(cb));
        });
    }
    uint64_t one = 1;
    [[maybe_unused]] auto n = write(wakeFd_, &one, sizeof(one));
}

// ---------- worker thread ----------

void DBusBridge::threadMain()
{
    constexpr int kMaxEvents = 16;
    epoll_event events[kMaxEvents];

    while (!stop_.load(std::memory_order_acquire)) {
        int waitMs = msUntilNextTimeout();
        int n = epoll_wait(epollFd_, events, kMaxEvents, waitMs);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("DBusBridge: epoll_wait: {}", strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ep = events[i].events;
            if (fd == wakeFd_) {
                uint64_t buf;
                [[maybe_unused]] auto _ = read(wakeFd_, &buf, sizeof(buf));
                continue;
            }
            // libdbus fd: dispatch to all DBusWatch* on this fd.
            auto it = fdWatches_.find(fd);
            if (it == fdWatches_.end()) continue;
            unsigned dflags = dbusFlagsFromEpoll(ep);
            // Snapshot — handle() may toggle/remove watches.
            std::vector<DBusWatch*> snap(it->second.begin(), it->second.end());
            for (DBusWatch* w : snap) {
                if (!dbus_watch_get_enabled(w)) continue;
                unsigned wantFlags = dbus_watch_get_flags(w);
                unsigned hit = dflags & (wantFlags | DBUS_WATCH_ERROR | DBUS_WATCH_HANGUP);
                if (hit) dbus_watch_handle(w, hit);
            }
        }

        runReadyTimeouts();
        drainOutbox();
        dispatchAll();
    }
}

void DBusBridge::drainOutbox()
{
    std::vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lk(outMu_);
        local.swap(outbox_);
    }
    for (auto& fn : local) fn();
}

int DBusBridge::msUntilNextTimeout()
{
    if (timeouts_.empty()) return -1;
    uint64_t now = nowNs();
    uint64_t soonest = UINT64_MAX;
    for (auto& kv : timeouts_) {
        if (!kv.second.enabled) continue;
        if (kv.second.deadlineNs < soonest) soonest = kv.second.deadlineNs;
    }
    if (soonest == UINT64_MAX) return -1;
    if (soonest <= now) return 0;
    uint64_t deltaNs = soonest - now;
    uint64_t deltaMs = (deltaNs + 999'999ULL) / 1'000'000ULL;
    if (deltaMs > 60'000ULL) deltaMs = 60'000ULL;
    return static_cast<int>(deltaMs);
}

void DBusBridge::runReadyTimeouts()
{
    if (timeouts_.empty()) return;
    uint64_t now = nowNs();
    // Snapshot to a local list — handle() may rearm/remove timeouts.
    std::vector<DBusTimeout*> ready;
    for (auto& kv : timeouts_) {
        if (kv.second.enabled && kv.second.deadlineNs <= now)
            ready.push_back(kv.first);
    }
    for (DBusTimeout* t : ready) {
        // libdbus rearms by calling toggle / re-setting the interval.
        // Reset deadline first so an immediate handle() that doesn't
        // rearm doesn't refire.
        auto it = timeouts_.find(t);
        if (it != timeouts_.end()) {
            int interval = dbus_timeout_get_interval(t);
            it->second.deadlineNs = now + static_cast<uint64_t>(interval) * 1'000'000ULL;
        }
        dbus_timeout_handle(t);
    }
}

void DBusBridge::dispatchAll()
{
    if (!conn_) return;
    while (dbus_connection_dispatch(conn_) == DBUS_DISPATCH_DATA_REMAINS) {}
}

void DBusBridge::rebuildFdMask(int fd)
{
    auto it = fdWatches_.find(fd);
    if (it == fdWatches_.end() || it->second.empty()) {
        // Drop the registration.
        if (fdEpollMask_.erase(fd)) {
            epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        if (it != fdWatches_.end()) fdWatches_.erase(it);
        return;
    }

    uint32_t mask = 0;
    for (DBusWatch* w : it->second) {
        if (!dbus_watch_get_enabled(w)) continue;
        mask |= epollMaskFromDbus(dbus_watch_get_flags(w));
    }
    if (mask == 0) {
        if (fdEpollMask_.erase(fd)) {
            epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        return;
    }

    epoll_event ev{};
    ev.events = mask;
    ev.data.fd = fd;
    auto curIt = fdEpollMask_.find(fd);
    if (curIt == fdEpollMask_.end()) {
        if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            spdlog::warn("DBusBridge: EPOLL_CTL_ADD fd={}: {}", fd, strerror(errno));
            return;
        }
        fdEpollMask_[fd] = mask;
    } else if (curIt->second != mask) {
        if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            spdlog::warn("DBusBridge: EPOLL_CTL_MOD fd={}: {}", fd, strerror(errno));
            return;
        }
        curIt->second = mask;
    }
}

// ---------- libdbus hook trampolines ----------

dbus_bool_t DBusBridge::s_add_watch(DBusWatch* w, void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    int fd = dbus_watch_get_unix_fd(w);
    if (fd < 0) fd = dbus_watch_get_socket(w);
    if (fd < 0) return TRUE;  // nothing to register, but not an OOM

    self->allWatches_.insert(w);
    self->watchFd_[w] = fd;
    self->fdWatches_[fd].insert(w);
    self->rebuildFdMask(fd);
    return TRUE;
}

void DBusBridge::s_remove_watch(DBusWatch* w, void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    auto it = self->watchFd_.find(w);
    if (it == self->watchFd_.end()) return;
    int fd = it->second;
    self->watchFd_.erase(it);
    self->allWatches_.erase(w);
    auto fdIt = self->fdWatches_.find(fd);
    if (fdIt != self->fdWatches_.end()) {
        fdIt->second.erase(w);
        self->rebuildFdMask(fd);  // drops registration if set went empty
    }
}

void DBusBridge::s_toggle_watch(DBusWatch* w, void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    auto it = self->watchFd_.find(w);
    if (it == self->watchFd_.end()) return;
    self->rebuildFdMask(it->second);
}

dbus_bool_t DBusBridge::s_add_timeout(DBusTimeout* t, void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    bool enabled = dbus_timeout_get_enabled(t);
    int interval = dbus_timeout_get_interval(t);
    self->timeouts_[t] = TimeoutInfo{
        nowNs() + static_cast<uint64_t>(interval) * 1'000'000ULL,
        enabled
    };
    return TRUE;
}

void DBusBridge::s_remove_timeout(DBusTimeout* t, void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    self->timeouts_.erase(t);
}

void DBusBridge::s_toggle_timeout(DBusTimeout* t, void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    auto it = self->timeouts_.find(t);
    if (it == self->timeouts_.end()) return;
    it->second.enabled = dbus_timeout_get_enabled(t);
    int interval = dbus_timeout_get_interval(t);
    it->second.deadlineNs = nowNs() + static_cast<uint64_t>(interval) * 1'000'000ULL;
}

void DBusBridge::s_wakeup_main(void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    if (self->wakeFd_ < 0) return;
    uint64_t one = 1;
    [[maybe_unused]] auto n = write(self->wakeFd_, &one, sizeof(one));
}

DBusHandlerResult DBusBridge::s_filter(DBusConnection*, DBusMessage* msg, void* data)
{
    auto* self = static_cast<DBusBridge*>(data);
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    // Hand to every registered signal callback. They are responsible for
    // their own interface/member filtering — the AddMatch rule already
    // narrows server-side.
    for (auto& kv : self->signalCbs_) {
        kv.second(msg);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void DBusBridge::s_pending_notify(DBusPendingCall* pending, void* data)
{
    auto* cb = static_cast<ReplyCb*>(data);
    DBusMessage* reply = dbus_pending_call_steal_reply(pending);
    bool ok = reply && dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_ERROR;
    if (*cb) (*cb)(reply, ok);
    if (reply) dbus_message_unref(reply);
    // `cb` is freed by libdbus via the free function passed to set_notify.
}
