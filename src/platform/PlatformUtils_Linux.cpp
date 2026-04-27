#include "DBusBridge.h"
#include "PlatformDawn.h"
#include "WaitableValue.h"

#include <eventloop/EventLoop.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

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
    pid_t pid = fork();
    if (pid == 0) {
        pid_t inner = fork();
        if (inner == 0) {
            setsid();
            execvp(path, argv);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
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

    if (DBusMessage* call = buildSettingsReadCall()) {
        g_bridge->sendAsync(call, &handleColorSchemeReadReply);
    }
}

void platformShutdown()
{
    g_bridge.reset();
    g_eventLoop = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_appearanceMu);
        g_appearanceCallback = nullptr;
    }
    g_darkMode.unset();
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

void platformSendNotification(const std::string& title, const std::string& body,
                              uint8_t urgency)
{
    if (!g_bridge || !g_bridge->connected()) {
        spdlog::warn("platformSendNotification: D-Bus session bus unavailable; "
                     "dropping notification: {}", title);
        return;
    }

    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify");
    if (!call) {
        spdlog::warn("platformSendNotification: dbus_message_new_method_call failed");
        return;
    }

    const char*     app      = "mb";
    dbus_uint32_t   replaces = 0;
    // Reverse-DNS identifier — GNOME Shell uses this as the source label
    // when no .desktop file is installed for the app. Other daemons
    // (dunst/mako/xfce4-notifyd) treat it as an icon name and fall back
    // to a generic icon if no matching theme entry exists.
    const char*     icon     = "it.masterband.mb";
    const char*     summary  = title.c_str();
    const char*     bodyP    = body.c_str();
    dbus_int32_t    expire   = -1;

    DBusMessageIter args;
    dbus_message_iter_init_append(call, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &app);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replaces);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &summary);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &bodyP);

    DBusMessageIter actionsArr;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actionsArr);
    dbus_message_iter_close_container(&args, &actionsArr);

    DBusMessageIter hintsArr;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hintsArr);
    {
        // desktop-entry hint: basename (no .desktop suffix) of the
        // installed .desktop file. Used by mako/dunst/xfce4-notifyd; GNOME
        // Shell ignores this and uses WM_CLASS instead, but we set it for
        // multi-daemon coverage.
        DBusMessageIter entry, variant;
        const char* key = "desktop-entry";
        const char* val = "it.masterband.mb";
        dbus_message_iter_open_container(&hintsArr, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&hintsArr, &entry);
    }
    {
        // urgency hint: byte 0=low, 1=normal, 2=critical. Spec section
        // "Notification Urgency Levels". Critical notifications are typically
        // not auto-expired by the daemon.
        DBusMessageIter entry, variant;
        const char* key = "urgency";
        uint8_t      val = (urgency > 2) ? 1 : urgency;
        dbus_message_iter_open_container(&hintsArr, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "y", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&hintsArr, &entry);
    }
    dbus_message_iter_close_container(&args, &hintsArr);

    dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &expire);

    g_bridge->sendAsync(call, [titleCopy = title](DBusMessage* reply, bool ok) {
        if (ok) return;
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
                     titleCopy,
                     errName ? errName : "(no reply)",
                     errMsg ? errMsg : "");
    });
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
