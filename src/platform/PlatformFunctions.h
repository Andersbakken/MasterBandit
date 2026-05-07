#pragma once

// Free-function platform API. Split out of PlatformDawn.h so platform-
// specific TUs (notably PlatformUtils_macOS.mm, which #imports Cocoa)
// can pick up just these declarations without dragging in DebugIPC.h ->
// libwebsockets.h. libwebsockets' lws-http.h enum HTTP_STATUS_* collides
// with cups http.h (transitive via Cocoa) on the macOS 26.4 SDK.

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

class EventLoop;

// Implemented in platform-specific files (PlatformUtils_macOS.mm / PlatformUtils_Linux.cpp)
//
// platformInit must run before any of the other platform calls. On Linux
// it opens the session-bus connection used for dark-mode discovery and
// notifications; on macOS it is currently a no-op. platformShutdown is the
// counterpart, called from PlatformDawn's destructor before the EventLoop
// itself is torn down.
void platformInit(EventLoop &loop);
void platformShutdown();
bool platformIsDarkMode();
void platformObserveAppearanceChanges(std::function<void(bool isDark)> callback);
void platformInitNotifications();
void platformSetNotificationsShowWhenForeground(bool show);
// Push the current window focus + visibility into the notification layer so
// platformSendNotification can apply OSC 99 `o=` (only_when) gating without
// querying the windowing-toolkit at send time. PlatformDawn calls this from
// its onFocus / onVisibility hooks. macOS reads NSWindow state directly and
// implements this as a no-op.
void platformSetNotificationWindowState(bool focused, bool visible);
// urgency: 0=low, 1=normal, 2=critical (freedesktop Notifications "urgency"
// hint). Currently honored on Linux; macOS impl ignores the value pending a
// per-platform mapping.
//
// sourceTag + clientId form a composite key under which the platform tracks
// the live notification. If a previous notification with the same key is
// still live, the new send replaces it in place (Linux: replaces_id;
// macOS: pending). Empty sourceTag or empty clientId disables tracking —
// the notification is sent fresh and never replaceable.
//
// closeResponseRequested mirrors OSC 99 c=1: when set, the platform will
// invoke `onClosed` once the daemon dismisses the notification (any reason
// — expiry, user-dismissal, programmatic close). Linux maps freedesktop
// reasons 1/2/3/4 → "expired"/"dismissed-by-user"/"closed"/"". onClosed
// runs on the main thread.
//
// buttons + onActivated wire the OSC 99 `p=buttons` / `a=` action surface.
// Each label in `buttons` becomes a clickable button on the daemon side
// (capped at 8 by the OSC parser). onActivated fires on the main thread
// when the user clicks the body or one of the buttons; the buttonId is
// "" for the body/default click, or "1".."N" for a specific button (1-
// based). Empty buttons + null onActivated → notification has no
// interactive surface at all.
//
// onlyWhen is the OSC 99 `o=` gate (kitty notifications.py:955-962).
// Empty == always-allow. "unfocused" suppresses when our window has
// focus; "invisible" suppresses when focused or visible-but-unfocused;
// "always" allows. Suppression silently drops — no banner, no in-flight
// tracking, no `onClosed` fired even when c=1 (matches kitty
// notify_with_command at notifications.py:978-993 — c=1 clients are
// expected to use p=alive polling, not synchronous wait-for-close).
void platformSendNotification(const std::string &sourceTag,
                              const std::string &clientId,
                              const std::string &title,
                              const std::string &body,
                              uint8_t urgency,
                              bool closeResponseRequested,
                              std::function<void(const std::string &reason)> onClosed,
                              const std::vector<std::string> &buttons,
                              std::function<void(const std::string &buttonId)> onActivated,
                              const std::string &onlyWhen);

// Programmatically dismiss a previously-shown notification. No-op if the
// (sourceTag, clientId) pair isn't currently tracked (for example: the
// daemon already auto-expired it, or the original send never landed a
// reply yet).
void platformCloseNotification(const std::string &sourceTag,
                               const std::string &clientId);

// Return the clientIds of currently-active notifications belonging to
// sourceTag — i.e. those with a daemon id that hasn't been dropped via
// NotificationClosed yet. Used to satisfy OSC 99 p=alive queries.
std::vector<std::string> platformActiveNotifications(const std::string &sourceTag);
void platformOpenURL(const std::string &url);
std::string platformProcessCWD(pid_t pid);

// Launch an external (non-PTY) process detached from mb. Used by the
// `mb.process.spawn` script API and by anything else that wants to run
// a binary without attaching its stdio to a terminal pane.
//
// Semantics:
//   - PATH lookup via execvp(3) — `path` may be a basename or absolute.
//   - Argv-style. argv[0] is the program name as the child sees it; the
//     caller is expected to mirror `path` there unless they have a
//     reason not to. No shell, no quoting, no metacharacter expansion.
//   - Detached: double-forks, calls setsid(2), reaps the intermediate
//     child synchronously so mb leaves no zombie. The grandchild is
//     adopted by init/launchd. mb's controlling tty is dropped.
//   - stdio: reopened to /dev/null in the grandchild so the spawned
//     program can't write back into mb's terminal (we have no PTY for
//     it) and can't read from mb's stdin.
//   - cwd: empty string → inherit. Non-empty → chdir(2) before exec.
//     Failure to chdir aborts the spawn (errno surfaces via the same
//     errno-pipe path that catches execvp failures).
//   - env: empty vector → inherit mb's environment. Non-empty → merge
//     over the inherited env (key=value pairs replace, new keys add;
//     no key removal in v1). Use the helper rather than rolling your
//     own envp to keep the merge semantics consistent.
//   - cwd / env strings must outlive the call (they're consumed
//     synchronously, before fork returns).
//
// Returns:
//   pid > 0 : intermediate child reaped successfully. The grandchild
//             may still have failed exec — we log warn from the parent
//             when the errno pipe carries a value. pid is the
//             intermediate child's, NOT the grandchild's; v1 has no
//             API for tracking the grandchild's lifecycle (would need
//             SIGCHLD / pidfd plumbing — see "tracked" follow-up).
//   0       : fork failed pre-spawn. errno-class info logged; caller
//             should treat as a synchronous failure.
//
// Errors after fork but before exec (chdir, exec) are reported via
// spdlog::warn from the parent and cause the spawn to fail silently
// from the script's perspective. Returning a structured error code
// from the v1 detached API would require waiting for the grandchild,
// which defeats "detached" — the lossy log is the price.
//
// Implemented per-platform:
//   - PlatformUtils_Linux.cpp   (existing spawnDetached refactored)
//   - PlatformUtils_macOS.mm    (new; same fork-based approach)
struct ProcessSpawnOptions
{
    std::string cwd;                                      // empty == inherit
    std::vector<std::pair<std::string, std::string>> env; // empty == inherit
};

pid_t platformSpawnDetached(const std::string &path,
                            const std::vector<std::string> &argv,
                            const ProcessSpawnOptions &opts);
