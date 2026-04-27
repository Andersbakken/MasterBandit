#include "DBusBridge.h"
#include "PlatformDawn.h"
#include "WaitableValue.h"

#include <eventloop/EventLoop.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// All file-static state below is owned by platformInit / platformShutdown.
// Worker-thread callbacks (signal filter, pending-reply notify) update
// g_darkMode and post to g_eventLoop; everything user-facing runs on the
// main thread.
namespace {

EventLoop*                       g_eventLoop = nullptr;
std::unique_ptr<DBusBridge>      g_bridge;
WaitableValue<bool>              g_darkMode;

// Appearance observer is set / read on the main thread only, but we still
// take the mutex when invoking it via post() because the assignment in
// platformObserveAppearanceChanges and the read in the posted lambda
// happen on the same thread but at different points in the loop iteration.
std::mutex                       g_appearanceMu;
std::function<void(bool)>        g_appearanceCallback;

// Notification-tracking state. Mutated on both the main thread (sends,
// queries) and the DBus worker thread (Notify reply, NotificationClosed
// signal); g_notifyMu serializes both.
//
// Per (sourceTag, clientId): one Entry. The Entry tracks the live daemon
// id (after the first reply lands) and an in-flight flag covering the
// gap between Notify send and reply. A second send arriving while
// inFlight overwrites Queued — the latest payload wins, replacing the
// oldest queued, so the daemon never sees more than one duplicate per
// rapid burst regardless of how many we receive.
//
// Composite key uses \x1f (US) as the separator. sourceTag is a Uuid
// stringification (hex + dashes); clientId is an OSC 99 i= which kitty
// passes through sanitize_id (alphanumeric + dash + underscore, plus a
// few separators). Neither contains \x1f, so the encoding is unambiguous.
struct NotificationQueued {
    std::string title;
    std::string body;
    uint8_t     urgency = 1;
    bool        closeResponseRequested = false;
    std::function<void(const std::string& reason)> onClosed;
    std::vector<std::string> buttons;
    std::function<void(const std::string& buttonId)> onActivated;
};

struct NotificationEntry {
    std::string sourceTag;
    std::string clientId;
    uint32_t    daemonId = 0;     // 0 until first successful Notify reply
    bool        inFlight = false; // a Notify is awaiting reply
    bool        closeResponseRequested = false;  // for the currently-active notification
    std::function<void(const std::string& reason)> onClosed;  // active onClosed
    std::function<void(const std::string& buttonId)> onActivated;  // body/button click
    std::unique_ptr<NotificationQueued> queued;  // payload waiting for reply
};

std::mutex                                          g_notifyMu;
std::unordered_map<std::string, NotificationEntry>  g_entries;     // key → Entry
std::unordered_map<uint32_t, std::string>           g_daemonToKey; // daemonId → key

// Daemon capabilities, populated on init by GetCapabilities. False until
// the reply lands (so the very first send may go out without buttons even
// if the daemon supports them — same race as wezterm/kitty have).
std::atomic<bool> g_supportsActions{false};

constexpr char kNotifyKeySep = '\x1f';

std::string makeNotifyKey(const std::string& sourceTag, const std::string& clientId)
{
    std::string out;
    out.reserve(sourceTag.size() + 1 + clientId.size());
    out.append(sourceTag);
    out.push_back(kNotifyKeySep);
    out.append(clientId);
    return out;
}

const char* freedesktopReasonToText(uint32_t reason)
{
    // org.freedesktop.Notifications.NotificationClosed reason codes:
    //   1 — expired
    //   2 — dismissed by user
    //   3 — closed via CloseNotification
    //   4 — undefined / reserved
    // Maps to the OSC 99 close-response reason strings used by kitty's
    // send_closed_response (notifications.py:1043-1045).
    switch (reason) {
    case 1: return "expired";
    case 2: return "dismissed-by-user";
    case 3: return "closed";
    default: return "";
    }
}

void publishDarkMode(bool isDark)
{
    g_darkMode.set(isDark);
    if (!g_eventLoop) return;
    g_eventLoop->post([isDark] {
        std::function<void(bool)> cb;
        {
            std::lock_guard<std::mutex> lk(g_appearanceMu);
            cb = g_appearanceCallback;
        }
        if (cb) cb(isDark);
    });
}

// Worker-thread.
void handleSettingChanged(DBusMessage* msg)
{
    if (!dbus_message_is_signal(msg, "org.freedesktop.portal.Settings",
                                "SettingChanged")) return;
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args)) return;

    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) return;
    const char* ns = nullptr;
    dbus_message_iter_get_basic(&args, &ns);
    if (!ns || strcmp(ns, "org.freedesktop.appearance") != 0) return;

    if (!dbus_message_iter_next(&args)) return;
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) return;
    const char* key = nullptr;
    dbus_message_iter_get_basic(&args, &key);
    if (!key || strcmp(key, "color-scheme") != 0) return;

    if (!dbus_message_iter_next(&args)) return;
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_VARIANT) return;
    DBusMessageIter v;
    dbus_message_iter_recurse(&args, &v);
    if (dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_UINT32) return;
    dbus_uint32_t scheme = 0;
    dbus_message_iter_get_basic(&v, &scheme);
    publishDarkMode(scheme == 1);
}

// Worker-thread. Reply payload for Settings.Read is a single VARIANT
// containing the actual value's variant: signature "v" outside, with the
// inner type being whatever the setting holds (UINT32 here). Any failure
// path (no portal on the bus, malformed reply, unexpected type) latches
// the cache to false so callers stop blocking on the timeout.
void handleColorSchemeReadReply(DBusMessage* reply, bool ok)
{
    auto fail = [&](const char* why, const char* detail = nullptr) {
        spdlog::warn("DBusBridge: portal Settings.Read color-scheme {}{}{}",
                     why,
                     detail ? ": " : "",
                     detail ? detail : "");
        publishDarkMode(false);
    };

    if (!ok) {
        const char* errName = reply ? dbus_message_get_error_name(reply) : nullptr;
        const char* errMsg  = nullptr;
        if (reply && errName) {
            DBusMessageIter it;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&it, &errMsg);
            }
        }
        spdlog::warn("DBusBridge: portal Settings.Read color-scheme failed "
                     "({}{}{}); dark mode disabled",
                     errName ? errName : "no reply",
                     errMsg ? ": " : "",
                     errMsg ? errMsg : "");
        publishDarkMode(false);
        return;
    }
    if (!reply) { fail("no reply"); return; }

    DBusMessageIter args;
    if (!dbus_message_iter_init(reply, &args)) { fail("empty reply"); return; }
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_VARIANT) {
        fail("reply not a variant"); return;
    }

    DBusMessageIter inner;
    dbus_message_iter_recurse(&args, &inner);

    // Some portal versions wrap in a second variant; tolerate both.
    if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner2;
        dbus_message_iter_recurse(&inner, &inner2);
        inner = inner2;
    }

    if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_UINT32) {
        fail("inner variant is not UINT32"); return;
    }
    dbus_uint32_t scheme = 0;
    dbus_message_iter_get_basic(&inner, &scheme);
    spdlog::info("DBusBridge: portal color-scheme = {} (raw={})",
                 scheme == 1 ? "dark" : "light", scheme);
    publishDarkMode(scheme == 1);
}

// Worker-thread. ActionInvoked payload is "us" — (id, action_key).
void handleActionInvoked(DBusMessage* msg)
{
    if (!dbus_message_is_signal(msg, "org.freedesktop.Notifications",
                                "ActionInvoked")) return;
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args)) return;
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32) return;
    dbus_uint32_t nid = 0;
    dbus_message_iter_get_basic(&args, &nid);

    if (!dbus_message_iter_next(&args)) return;
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) return;
    const char* key = nullptr;
    dbus_message_iter_get_basic(&args, &key);
    if (!key) return;

    // Map our wire form: "default" → "" (body click), numeric "1".."N"
    // → button index passed through verbatim. Anything else gets passed
    // through as-is — kitty doesn't define non-numeric custom actions
    // for OSC 99 today.
    std::string buttonId = (strcmp(key, "default") == 0) ? std::string{}
                                                         : std::string(key);

    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lk(g_notifyMu);
        auto dit = g_daemonToKey.find(nid);
        if (dit == g_daemonToKey.end()) return;
        auto eit = g_entries.find(dit->second);
        if (eit == g_entries.end()) return;
        NotificationEntry& e = eit->second;
        cb = e.onActivated;
        // Match kitty notifications.py:902-903: if close-response wasn't
        // requested, the notification is single-shot — purge after first
        // activation so subsequent ActionInvoked from the same daemon id
        // don't refire the callback. If c=1 was set, leave the entry
        // intact so onClosed can still fire when the user eventually
        // dismisses (and to keep multiple-click reports working under
        // kitty's spec).
        if (!e.closeResponseRequested) {
            e.onActivated = nullptr;
            g_daemonToKey.erase(dit);
            // Drop the entry entirely if nothing else is keeping it
            // alive (no close-response, no in-flight, no queued).
            if (!e.inFlight && !e.queued && !e.onClosed) {
                g_entries.erase(eit);
            } else {
                e.daemonId = 0;
            }
        }
    }
    if (!cb || !g_eventLoop) return;
    g_eventLoop->post([cb = std::move(cb), buttonId = std::move(buttonId)]() mutable {
        cb(buttonId);
    });
}

// Worker-thread. NotificationClosed payload is "uu" — (id, reason).
void handleNotificationClosed(DBusMessage* msg)
{
    if (!dbus_message_is_signal(msg, "org.freedesktop.Notifications",
                                "NotificationClosed")) return;
    DBusMessageIter args;
    if (!dbus_message_iter_init(msg, &args)) return;
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32) return;
    dbus_uint32_t nid = 0;
    dbus_message_iter_get_basic(&args, &nid);

    dbus_uint32_t reason = 4;  // undefined if absent
    if (dbus_message_iter_next(&args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(&args, &reason);
    }

    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lk(g_notifyMu);
        auto dit = g_daemonToKey.find(nid);
        if (dit == g_daemonToKey.end()) return;
        std::string key = std::move(dit->second);
        g_daemonToKey.erase(dit);

        auto eit = g_entries.find(key);
        if (eit == g_entries.end()) return;
        NotificationEntry& e = eit->second;
        if (e.daemonId != nid) {
            // The daemon id we just got a close for is not the entry's
            // current id — already replaced. Nothing to fire.
            return;
        }
        cb = std::move(e.onClosed);
        e.onClosed = nullptr;
        e.closeResponseRequested = false;
        e.daemonId = 0;
        // If nothing is in-flight or queued, drop the entry entirely.
        if (!e.inFlight && !e.queued) {
            g_entries.erase(eit);
        }
    }
    if (!cb || !g_eventLoop) return;
    std::string reasonText = freedesktopReasonToText(reason);
    g_eventLoop->post([cb = std::move(cb),
                       reasonText = std::move(reasonText)]() mutable {
        cb(reasonText);
    });
}

DBusMessage* buildSettingsReadCall()
{
    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings",
        "Read");
    if (!call) return nullptr;
    const char* ns  = "org.freedesktop.appearance";
    const char* key = "color-scheme";
    if (!dbus_message_append_args(call,
            DBUS_TYPE_STRING, &ns,
            DBUS_TYPE_STRING, &key,
            DBUS_TYPE_INVALID)) {
        dbus_message_unref(call);
        return nullptr;
    }
    return call;
}

void spawnDetached(const char* path, char* const argv[])
{
    // Errno-reporting pipe. Both ends are O_CLOEXEC so a successful
    // execvp in the grandchild closes the write end automatically; the
    // parent's read returns EOF and we know exec succeeded. If exec
    // fails (or the inner fork fails), the failing process writes the
    // errno to the pipe before _exit, and the parent's read picks it
    // up. Pattern is well-known; see e.g. APUE §10.18.
    int errPipe[2];
    if (pipe2(errPipe, O_CLOEXEC) < 0) {
        spdlog::warn("spawnDetached({}): pipe2: {}", path, strerror(errno));
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(errPipe[0]);
        close(errPipe[1]);
        spdlog::warn("spawnDetached({}): fork: {}", path, strerror(errno));
        return;
    }
    if (pid == 0) {
        // Child: only the grandchild needs the write end. Drop the
        // read end so we can't accidentally read our own errno.
        close(errPipe[0]);
        pid_t inner = fork();
        if (inner == 0) {
            // Grandchild. setsid detaches from our process group so
            // the launched program isn't killed when mb's controlling
            // terminal goes away.
            setsid();
            execvp(path, argv);
            int err = errno;
            ssize_t n = write(errPipe[1], &err, sizeof(err));
            (void)n;
            _exit(127);
        }
        if (inner < 0) {
            int err = errno;
            ssize_t n = write(errPipe[1], &err, sizeof(err));
            (void)n;
        }
        close(errPipe[1]);
        _exit(0);
    }

    // Parent: close write end so EOF is detectable on read once the
    // grandchild's copy goes away (either via exec or _exit).
    close(errPipe[1]);
    int status;
    waitpid(pid, &status, 0);

    int err = 0;
    ssize_t n;
    do {
        n = read(errPipe[0], &err, sizeof(err));
    } while (n < 0 && errno == EINTR);
    close(errPipe[0]);

    if (n == static_cast<ssize_t>(sizeof(err))) {
        spdlog::warn("spawnDetached({}): execvp failed: {}",
                     path, strerror(err));
    }
    // n == 0  → exec succeeded (write end closed by O_CLOEXEC); no log.
    // n == -1 → read error (rare, e.g. EBADF if pipe was closed twice);
    //          treat as "unknown outcome" and stay silent.
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// Public API (declared in PlatformDawn.h)
// ─────────────────────────────────────────────────────────────────────────

void platformInit(EventLoop& loop)
{
    g_eventLoop = &loop;
    g_bridge = std::make_unique<DBusBridge>();
    if (!g_bridge->connected()) {
        // Permanent failure: populate the cache with a default so callers
        // don't block waiting for a reply that will never arrive.
        g_darkMode.set(false);
        return;
    }

    // Subscribe before reading so a setting change between the Read send
    // and the AddMatch register isn't missed.
    g_bridge->addSignalMatch(
        "type='signal',"
        "interface='org.freedesktop.portal.Settings',"
        "member='SettingChanged',"
        "path='/org/freedesktop/portal/desktop'",
        &handleSettingChanged);

    // Subscribe to NotificationClosed so we can drop entries from the
    // tracking map when the daemon dismisses them, and propagate
    // close-response back to terminals that asked for it (OSC 99 c=1).
    g_bridge->addSignalMatch(
        "type='signal',"
        "interface='org.freedesktop.Notifications',"
        "member='NotificationClosed',"
        "path='/org/freedesktop/Notifications'",
        &handleNotificationClosed);

    // ActionInvoked carries (id, action_key). We map "default" to the
    // body-click report (empty button id); numeric keys "1".."N" map to
    // button indices passed through to OSC 99 a=report.
    g_bridge->addSignalMatch(
        "type='signal',"
        "interface='org.freedesktop.Notifications',"
        "member='ActionInvoked',"
        "path='/org/freedesktop/Notifications'",
        &handleActionInvoked);

    if (DBusMessage* call = buildSettingsReadCall()) {
        g_bridge->sendAsync(call, &handleColorSchemeReadReply);
    }

    // Query daemon capabilities. We currently only care about 'actions'
    // for gating button payloads — daemons that don't advertise it would
    // silently drop the buttons array, but we drop them ourselves to
    // avoid sending dead bytes (matches kitty notifications.py:733).
    if (DBusMessage* call = dbus_message_new_method_call(
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "GetCapabilities")) {
        g_bridge->sendAsync(call, [](DBusMessage* reply, bool ok) {
            if (!ok || !reply) return;
            DBusMessageIter args, sub;
            if (!dbus_message_iter_init(reply, &args)) return;
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) return;
            dbus_message_iter_recurse(&args, &sub);
            while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
                const char* cap = nullptr;
                dbus_message_iter_get_basic(&sub, &cap);
                if (cap && strcmp(cap, "actions") == 0) {
                    g_supportsActions.store(true, std::memory_order_release);
                    break;
                }
                if (!dbus_message_iter_next(&sub)) break;
            }
        });
    }
}

void platformShutdown()
{
    // Tear the bridge down first; its dtor joins the worker thread, so any
    // signal callback or Notify-reply that was in flight is fully drained
    // before we touch the maps. After this point no posted lambdas can land
    // either (the bridge's worker is the source of those for notifications).
    g_bridge.reset();
    g_eventLoop = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_appearanceMu);
        g_appearanceCallback = nullptr;
    }
    g_darkMode.unset();
    {
        std::lock_guard<std::mutex> lk(g_notifyMu);
        g_entries.clear();
        g_daemonToKey.clear();
    }
}

bool platformIsDarkMode()
{
    // Block up to 1 s if the initial Read reply hasn't landed yet. Once
    // populated (by reply or by SettingChanged), subsequent calls are
    // non-blocking. On bridge-failure we set(false) at init, also
    // non-blocking from the first call.
    return g_darkMode.getOrWait(std::chrono::seconds(1)).value_or(false);
}

void platformObserveAppearanceChanges(std::function<void(bool isDark)> callback)
{
    std::lock_guard<std::mutex> lk(g_appearanceMu);
    g_appearanceCallback = std::move(callback);
}

void platformInitNotifications()
{
    // Notifications on Linux flow through DBusBridge. Bridge construction
    // and bus connection happens in platformInit(); this entry point
    // exists only to match the macOS API.
}

void platformSetNotificationsShowWhenForeground(bool /*show*/)
{
    // Linux notification daemons (libnotify-style) do not differentiate.
}

// Forward decl — the reply handler may chain into another dispatch.
void dispatchNotify(const std::string& key,
                    const std::string& title,
                    const std::string& body,
                    uint8_t urgency,
                    dbus_uint32_t replacesId,
                    const std::vector<std::string>& buttons);

void onNotifyReply(const std::string& key, dbus_uint32_t replacesAtSend,
                   const std::string& titleForLog,
                   DBusMessage* reply, bool ok)
{
    dbus_uint32_t newDaemonId = 0;
    if (ok && reply) {
        DBusMessageIter it;
        if (dbus_message_iter_init(reply, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&it, &newDaemonId);
        }
    }
    if (!ok) {
        const char* errName = reply ? dbus_message_get_error_name(reply) : nullptr;
        const char* errMsg  = nullptr;
        if (reply && errName) {
            DBusMessageIter it;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&it, &errMsg);
            }
        }
        spdlog::warn("Notify({}) failed: {} {}",
                     titleForLog,
                     errName ? errName : "(no reply)",
                     errMsg ? errMsg : "");
    }

    std::unique_ptr<NotificationQueued> queued;
    dbus_uint32_t replacesForQueued = 0;
    {
        std::lock_guard<std::mutex> lk(g_notifyMu);
        auto eit = g_entries.find(key);
        if (eit == g_entries.end()) return;
        NotificationEntry& e = eit->second;

        if (ok) {
            // Refresh daemon-id mappings. If we replaced an existing
            // daemonId and the daemon allocated a fresh one, drop the old
            // mapping. Same for replacesAtSend captured at send time
            // (covers the unusual case where it differed from e.daemonId).
            if (e.daemonId != 0 && e.daemonId != newDaemonId)
                g_daemonToKey.erase(e.daemonId);
            if (replacesAtSend != 0 && replacesAtSend != newDaemonId &&
                replacesAtSend != e.daemonId)
                g_daemonToKey.erase(replacesAtSend);
            e.daemonId = newDaemonId;
            g_daemonToKey[newDaemonId] = key;
        }

        if (e.queued) {
            // Promote the queued payload to the active in-flight slot.
            queued = std::move(e.queued);
            e.closeResponseRequested = queued->closeResponseRequested;
            e.onClosed = std::move(queued->onClosed);
            e.onActivated = std::move(queued->onActivated);
            replacesForQueued = e.daemonId;  // 0 if previous send failed
            // inFlight stays true.
        } else {
            e.inFlight = false;
            // Failed send with no daemon id and nothing queued: drop the
            // entry entirely so we don't leak a permanent placeholder.
            if (!ok && e.daemonId == 0) {
                g_entries.erase(eit);
            }
        }
    }

    if (queued) {
        dispatchNotify(key, queued->title, queued->body, queued->urgency,
                       replacesForQueued, queued->buttons);
    }
}

void dispatchNotify(const std::string& key,
                    const std::string& title,
                    const std::string& body,
                    uint8_t urgency,
                    dbus_uint32_t replacesId,
                    const std::vector<std::string>& buttons)
{
    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify");
    if (!call) {
        spdlog::warn("dispatchNotify: dbus_message_new_method_call failed");
        // Mark not in-flight and drop entry if needed; otherwise we'd
        // leak the in-flight state forever.
        std::lock_guard<std::mutex> lk(g_notifyMu);
        auto it = g_entries.find(key);
        if (it != g_entries.end()) {
            it->second.inFlight = false;
            if (it->second.daemonId == 0 && !it->second.queued)
                g_entries.erase(it);
        }
        return;
    }

    const char*  app     = "mb";
    const char*  icon    = "it.masterband.mb";
    const char*  summary = title.c_str();
    const char*  bodyP   = body.c_str();
    dbus_int32_t expire  = -1;

    DBusMessageIter args;
    dbus_message_iter_init_append(call, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replacesId);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &summary);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &bodyP);

    // Actions array: alternating (action_key, label) string pairs.
    // We always include "default" with a single space (dbus rejects empty
    // strings for action labels) — that makes the body clickable on
    // daemons that support actions; daemons that don't will ignore it.
    // Buttons are added 1-based only if the daemon advertised 'actions'
    // via GetCapabilities; otherwise we skip the byte cost.
    DBusMessageIter actionsArr;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actionsArr);
    {
        const char* defKey = "default";
        const char* defLbl = " ";
        dbus_message_iter_append_basic(&actionsArr, DBUS_TYPE_STRING, &defKey);
        dbus_message_iter_append_basic(&actionsArr, DBUS_TYPE_STRING, &defLbl);
    }
    if (g_supportsActions.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < buttons.size() && i < 8; ++i) {
            char keyBuf[8];
            int kn = snprintf(keyBuf, sizeof(keyBuf), "%zu", i + 1);
            if (kn <= 0) continue;
            const char* keyP = keyBuf;
            const char* lblP = buttons[i].c_str();
            dbus_message_iter_append_basic(&actionsArr, DBUS_TYPE_STRING, &keyP);
            dbus_message_iter_append_basic(&actionsArr, DBUS_TYPE_STRING, &lblP);
        }
    }
    dbus_message_iter_close_container(&args, &actionsArr);

    DBusMessageIter hintsArr;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hintsArr);
    {
        DBusMessageIter entry, variant;
        const char* hk = "desktop-entry";
        const char* hv = "it.masterband.mb";
        dbus_message_iter_open_container(&hintsArr, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &hk);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &hv);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&hintsArr, &entry);
    }
    {
        DBusMessageIter entry, variant;
        const char* hk = "urgency";
        uint8_t     hv = (urgency > 2) ? 1 : urgency;
        dbus_message_iter_open_container(&hintsArr, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &hk);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "y", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &hv);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&hintsArr, &entry);
    }
    dbus_message_iter_close_container(&args, &hintsArr);

    dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &expire);

    // Capture buttons by value into the reply lambda so it can re-dispatch
    // a chained queued send while preserving the original button labels.
    std::vector<std::string> buttonsCopy;  // unused at this point — buttons
                                           // travel through entry.queued for
                                           // chains. Reply handler doesn't
                                           // need them.
    (void)buttonsCopy;

    g_bridge->sendAsync(call, [keyCopy = key, replacesCopy = replacesId,
                               titleCopy = title]
                              (DBusMessage* reply, bool ok) {
        onNotifyReply(keyCopy, replacesCopy, titleCopy, reply, ok);
    });
}

void platformSendNotification(const std::string& sourceTag,
                              const std::string& clientId,
                              const std::string& title, const std::string& body,
                              uint8_t urgency,
                              bool closeResponseRequested,
                              std::function<void(const std::string& reason)> onClosed,
                              const std::vector<std::string>& buttons,
                              std::function<void(const std::string& buttonId)> onActivated,
                              const std::string& /*onlyWhen*/)
{
    // TODO Linux: honor o= (only_when) gate per kitty notifications.py:955-962.
    // macOS impl in PlatformUtils_macOS.mm is the reference. Suppression must
    // still fire onClosed("untracked") if c=1 to keep the wire side honest.
    if (!g_bridge || !g_bridge->connected()) {
        spdlog::warn("platformSendNotification: D-Bus session bus unavailable; "
                     "dropping notification: {}", title);
        return;
    }

    // Untracked path: no replace bookkeeping, just send. Suitable for
    // notifications that don't carry an OSC i= identifier. We still
    // install onActivated if provided — body clicks fire ActionInvoked
    // and we want the focus action to work even without an id.
    if (sourceTag.empty() || clientId.empty()) {
        std::string key = makeNotifyKey(sourceTag, clientId);
        {
            std::lock_guard<std::mutex> lk(g_notifyMu);
            NotificationEntry& e = g_entries[key];
            e.sourceTag = sourceTag;
            e.clientId = clientId;
            e.inFlight = true;
            e.closeResponseRequested = false;
            e.onClosed = nullptr;
            e.onActivated = std::move(onActivated);
        }
        dispatchNotify(key, title, body, urgency, 0, buttons);
        return;
    }

    std::string key = makeNotifyKey(sourceTag, clientId);
    bool sendNow = false;
    dbus_uint32_t replacesId = 0;

    {
        std::lock_guard<std::mutex> lk(g_notifyMu);
        NotificationEntry& e = g_entries[key];
        e.sourceTag = sourceTag;
        e.clientId  = clientId;

        if (e.inFlight) {
            // A Notify is already in flight for this key. Stash the new
            // payload — when the in-flight reply lands, it'll be promoted
            // and dispatched with the freshly-known daemonId. If a queued
            // payload was already pending, it gets replaced (latest wins).
            auto q = std::make_unique<NotificationQueued>();
            q->title    = title;
            q->body     = body;
            q->urgency  = urgency;
            q->closeResponseRequested = closeResponseRequested;
            q->onClosed = std::move(onClosed);
            q->buttons = buttons;
            q->onActivated = std::move(onActivated);
            e.queued = std::move(q);
        } else {
            e.inFlight = true;
            e.closeResponseRequested = closeResponseRequested;
            e.onClosed = closeResponseRequested ? std::move(onClosed) : nullptr;
            e.onActivated = std::move(onActivated);
            replacesId = e.daemonId;
            sendNow = true;
        }
    }

    if (sendNow) {
        dispatchNotify(key, title, body, urgency, replacesId, buttons);
    }
}

void platformCloseNotification(const std::string& sourceTag,
                               const std::string& clientId)
{
    if (!g_bridge || !g_bridge->connected()) return;
    if (sourceTag.empty() || clientId.empty()) return;

    dbus_uint32_t daemonId = 0;
    std::function<void(const std::string&)> immediateOnClosed;
    {
        std::lock_guard<std::mutex> lk(g_notifyMu);
        auto it = g_entries.find(makeNotifyKey(sourceTag, clientId));
        if (it == g_entries.end()) return;  // never sent or already cleaned up
        NotificationEntry& e = it->second;
        if (e.daemonId != 0) {
            daemonId = e.daemonId;
        } else if (e.closeResponseRequested) {
            // In-flight: no daemonId yet, so we can't dispatch
            // CloseNotification to the daemon. Match kitty
            // notifications.py:1031 — fire close-response immediately with
            // empty reason. Move onClosed out so the eventual
            // NotificationClosed signal won't fire it again. The
            // notification will still display when the Notify reply lands;
            // user must dismiss it manually (kitty has the same gap).
            immediateOnClosed = std::move(e.onClosed);
            e.closeResponseRequested = false;
        }
    }

    if (daemonId != 0) {
        DBusMessage* call = dbus_message_new_method_call(
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "CloseNotification");
        if (!call) {
            spdlog::warn("platformCloseNotification: dbus_message_new_method_call failed");
            return;
        }
        if (!dbus_message_append_args(call,
                DBUS_TYPE_UINT32, &daemonId,
                DBUS_TYPE_INVALID)) {
            dbus_message_unref(call);
            return;
        }
        // Don't drop the entry locally — wait for NotificationClosed to
        // fire so the close-response wireback reflects the daemon's reason
        // code (3 = closed).
        g_bridge->sendAsync(call, nullptr);
        return;
    }

    if (immediateOnClosed && g_eventLoop) {
        g_eventLoop->post([cb = std::move(immediateOnClosed)]() mutable {
            cb("");  // empty reason, matches kitty's send_closed_response default
        });
    }
}

std::vector<std::string> platformActiveNotifications(const std::string& sourceTag)
{
    std::vector<std::string> out;
    if (sourceTag.empty()) return out;
    std::lock_guard<std::mutex> lk(g_notifyMu);
    out.reserve(g_entries.size());
    for (auto& kv : g_entries) {
        const NotificationEntry& e = kv.second;
        // Include any entry that's been sent for this source — whether
        // it's still in-flight (no daemonId yet) or has landed and isn't
        // closed (daemonId != 0). Matches kitty's "alive from send time"
        // semantic in notifications.py.
        if (e.sourceTag != sourceTag) continue;
        if (e.clientId.empty()) continue;
        if (!e.inFlight && e.daemonId == 0) continue;  // post-error remnant
        out.push_back(e.clientId);
    }
    return out;
}

void platformOpenURL(const std::string& url)
{
    char* argv[] = {
        const_cast<char*>("xdg-open"),
        const_cast<char*>(url.c_str()),
        nullptr
    };
    spawnDetached("xdg-open", argv);
}

std::string platformProcessCWD(pid_t pid)
{
    char link[64];
    char path[4096];
    snprintf(link, sizeof(link), "/proc/%d/cwd", static_cast<int>(pid));
    ssize_t len = readlink(link, path, sizeof(path) - 1);
    if (len < 0) return {};
    path[len] = '\0';
    return path;
}
