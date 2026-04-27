#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/types.h>
#include <libproc.h>
#include <spdlog/spdlog.h>

// Foreground-banner toggle. Default: present banners even when mb is the
// active app — this is what a terminal user wants since long-running tasks
// are usually launched from inside mb (build complete, tests done, etc.).
// Independent of the per-notification OSC 99 o= gate (handled at send time).
static std::atomic<bool> g_showWhenForeground{true};

// Provided by Window_cocoa.mm. Returns the active CocoaWindow's NSWindow*
// (cast to void*) or nullptr if no window is alive yet (headless / pre-create).
extern "C" void* macActiveNSWindow();

// Composite identifier byte (matches Linux's `<sourceTag>\x1f<clientId>`).
static constexpr char kIdSep = '\x1f';

namespace {

struct InFlight {
    std::string sourceTag;
    std::string clientId;
    std::function<void(const std::string&)> onClosed;
    std::function<void(const std::string&)> onActivated;
};

// Routing tables — single-threaded by construction. Every read/write either
// happens on the main thread directly (platformSendNotification /
// platformCloseNotification / platformActiveNotifications) or is hopped via
// dispatch_async(dispatch_get_main_queue(), ...) before touching state
// (delegate callbacks). No mutex.
std::unordered_map<std::string /*identifier*/, InFlight> g_inflight;
// Insertion-order log for FIFO eviction (defensive cap — pathological wire
// bursts that never trigger reconcile shouldn't grow unbounded).
std::deque<std::string> g_inflightOrder;
constexpr size_t kInFlightCap = 256;

// Untracked-send disambiguator — when clientId is empty, append a counter
// suffix to the identifier so multiple untracked sends don't collide on the
// same key. Linux has the same gap (TODO.md:250) but we get it free here.
std::atomic<uint64_t> g_untrackedCounter{0};

// Button-set category cache. UNUserNotificationCenter's
// setNotificationCategories: is replace-all, so we keep the union and
// re-register on every send that produces a fresh hash.
std::unordered_map<uint64_t /*hash*/, std::string /*categoryId*/> g_categoryByHash;
NSMutableSet<UNNotificationCategory*>* g_categories = nil;

uint64_t hashButtons(const std::vector<std::string>& buttons)
{
    // FNV-1a over UTF-8 bytes joined by U+001E (record separator).
    uint64_t h = 14695981039346656037ull;
    auto mix = [&](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    for (size_t i = 0; i < buttons.size(); ++i) {
        if (i) mix(0x1e);
        for (unsigned char c : buttons[i]) mix(c);
    }
    return h;
}

std::string buildIdentifier(const std::string& sourceTag, const std::string& clientId)
{
    if (clientId.empty()) {
        // Untracked send. Append a unique counter so concurrent untracked
        // notifications don't share a UN identifier (which would
        // accidentally trigger replace-in-place between unrelated sends).
        uint64_t n = g_untrackedCounter.fetch_add(1, std::memory_order_relaxed);
        return sourceTag + kIdSep + "__untracked_" + std::to_string(n);
    }
    return sourceTag + kIdSep + clientId;
}

void rememberInFlight(const std::string& identifier, InFlight entry)
{
    auto it = g_inflight.find(identifier);
    if (it == g_inflight.end()) {
        g_inflightOrder.push_back(identifier);
        if (g_inflightOrder.size() > kInFlightCap) {
            const std::string& victim = g_inflightOrder.front();
            g_inflight.erase(victim);
            g_inflightOrder.pop_front();
        }
    }
    g_inflight[identifier] = std::move(entry);
}

void forgetInFlight(const std::string& identifier)
{
    if (g_inflight.erase(identifier) == 0) return;
    for (auto it = g_inflightOrder.begin(); it != g_inflightOrder.end(); ++it) {
        if (*it == identifier) { g_inflightOrder.erase(it); break; }
    }
}

// kitty notifications.py:955-962 parity. Returns true if the notification
// should be shown given onlyWhen and the current window state. Empty/unknown
// onlyWhen → always allow.
bool isAllowedByOnlyWhen(const std::string& onlyWhen)
{
    if (onlyWhen.empty() || onlyWhen == "always") return true;

    NSWindow* win = (__bridge NSWindow*)macActiveNSWindow();
    bool hasFocus = [NSApp isActive] && win && [win isKeyWindow];
    bool isVisible = false;
    if (win) {
        // occlusionState's `Visible` bit is true when *any* part of the
        // window is on-screen and not occluded; matches kitty's
        // os_window_is_invisible inversion.
        isVisible = [win isVisible] && (([win occlusionState] & NSWindowOcclusionStateVisible) != 0);
    }
    if (hasFocus) return false;
    if (onlyWhen == "invisible" && isVisible) return false;
    return true;
}

} // namespace

API_AVAILABLE(macos(10.14))
@interface MBNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation MBNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center; (void)notification;
    // OSC 99 o= gating already happened at send time — anything that lands
    // here is allowed by the per-notification spec. Only honor the runtime
    // foreground toggle.
    if (!g_showWhenForeground.load()) {
        completionHandler(UNNotificationPresentationOptionNone);
        return;
    }
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    (void)center;
    // Apple does not document the queue this fires on. Defensively hop to
    // main before touching g_inflight (kitty cocoa_window.m:422-440 does the
    // same; we share a no-mutex single-threaded model so this is required).
    NSString* identifier = response.notification.request.identifier;
    NSString* actionId = response.actionIdentifier;
    std::string idCpp = identifier ? [identifier UTF8String] : "";
    std::string actionCpp;
    if (actionId && ![actionId isEqualToString:UNNotificationDefaultActionIdentifier]) {
        actionCpp = [actionId UTF8String];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        auto it = g_inflight.find(idCpp);
        if (it == g_inflight.end()) return;
        // Copy the callback before invoking — onActivated may itself send a
        // new notification under the same identifier (replace-in-place),
        // which would invalidate the iterator/InFlight entry.
        auto cb = it->second.onActivated;
        if (cb) cb(actionCpp);
    });
    completionHandler();
}
@end

static MBNotificationDelegate* g_notifDelegate = nil;

class EventLoop;
void platformInit(EventLoop& /*loop*/) {}
void platformShutdown() {}

extern "C" const char* macResourcePathOrNull()
{
    if (![[NSBundle mainBundle] bundleIdentifier]) return nullptr;
    NSString* p = [[NSBundle mainBundle] resourcePath];
    return p ? [p UTF8String] : nullptr;
}

bool platformIsDarkMode()
{
    NSAppearance* appearance = [NSApp effectiveAppearance];
    NSAppearanceName best = [appearance bestMatchFromAppearancesWithNames:
        @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    return [best isEqualToString:NSAppearanceNameDarkAqua];
}

static std::function<void(bool)> g_appearanceCallback;
static id g_appearanceObserver = nil;

void platformInitNotifications()
{
    // UNUserNotificationCenter asserts on construction when there is no main
    // bundle (running the bare binary, not a .app). Bail before touching it.
    if (![[NSBundle mainBundle] bundleIdentifier]) {
        spdlog::warn("Notifications disabled: no bundle identifier "
                     "(running bare binary, not a .app)");
        return;
    }
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    if (!g_notifDelegate) g_notifDelegate = [[MBNotificationDelegate alloc] init];
    center.delegate = g_notifDelegate;
    if (!g_categories) g_categories = [NSMutableSet set];

    // iTerm2-style: only prompt when status is NotDetermined. Avoids the
    // re-prompt-on-every-launch behavior the original always-request had,
    // and avoids triggering the system dialog at all when the user
    // previously declined.
    [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* s) {
        if (s.authorizationStatus == UNAuthorizationStatusNotDetermined) {
            UNAuthorizationOptions opts = UNAuthorizationOptionAlert | UNAuthorizationOptionSound;
            [center requestAuthorizationWithOptions:opts
                completionHandler:^(BOOL granted, NSError* error) {
                    if (granted) {
                        spdlog::info("Notifications: authorization granted");
                    } else {
                        const char* err = error ? [[error localizedDescription] UTF8String] : "(no error info)";
                        spdlog::warn("Notifications: authorization not granted ({})", err);
                    }
                }];
        } else if (s.authorizationStatus == UNAuthorizationStatusDenied) {
            spdlog::info("Notifications: previously denied; not re-prompting");
        }
    }];
}

void platformSetNotificationsShowWhenForeground(bool show)
{
    g_showWhenForeground.store(show);
}

void platformSendNotification(const std::string& sourceTag,
                              const std::string& clientId,
                              const std::string& title, const std::string& body,
                              uint8_t urgency,
                              bool closeResponseRequested,
                              std::function<void(const std::string&)> onClosed,
                              const std::vector<std::string>& buttons,
                              std::function<void(const std::string&)> onActivated,
                              const std::string& onlyWhen)
{
    // Bundle-bail: same path as platformInitNotifications. Even when we
    // can't deliver, c=1 must still fire onClosed("untracked") synchronously
    // so the wire side doesn't wait forever for a close event that never
    // arrives (kitty notifications.py:991-992 pattern).
    if (![[NSBundle mainBundle] bundleIdentifier]) {
        if (closeResponseRequested && onClosed) onClosed("untracked");
        return;
    }

    // OSC 99 o= gating (kitty notifications.py:955-962). Suppression drops
    // the notification entirely — no banner, no Notification Center entry,
    // no in-flight tracking. c=1 still fires untracked-at-send.
    if (!isAllowedByOnlyWhen(onlyWhen)) {
        if (closeResponseRequested && onClosed) onClosed("untracked");
        return;
    }

    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];

    // Convert to NSString before any async hop — block-copy semantics retain
    // captured Objective-C objects, so the NSStrings outlive the caller's
    // std::string parameters; capturing those by reference would be a
    // use-after-free when the block fires later.
    NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
    NSString* nsBody  = [NSString stringWithUTF8String:body.c_str()];
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = nsTitle ?: @"";
    content.body  = nsBody  ?: @"";

    // Urgency → interruptionLevel + sound (kitty cocoa_window.m:513-526).
    // Deployment target is 12.0 so interruptionLevel is unconditionally
    // available.
    switch (urgency) {
        case 0:
            content.interruptionLevel = UNNotificationInterruptionLevelPassive;
            content.sound = nil;
            break;
        case 2:
            content.interruptionLevel = UNNotificationInterruptionLevelCritical;
            content.sound = [UNNotificationSound defaultCriticalSound];
            break;
        case 1:
        default:
            content.interruptionLevel = UNNotificationInterruptionLevelActive;
            content.sound = [UNNotificationSound defaultSound];
            break;
    }

    // Button-set category. Register before addNotificationRequest: so the
    // categoryIdentifier resolves on first delivery — UN docs require the
    // category to be live at delivery time.
    if (!buttons.empty()) {
        uint64_t h = hashButtons(buttons);
        std::string catId;
        auto cit = g_categoryByHash.find(h);
        if (cit == g_categoryByHash.end()) {
            // Hex-encode the hash — stable, ASCII, fits the
            // categoryIdentifier expectations.
            char buf[17];
            std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
            catId.assign(buf);
            NSMutableArray<UNNotificationAction*>* acts = [NSMutableArray array];
            for (size_t i = 0; i < buttons.size(); ++i) {
                NSString* aid   = [NSString stringWithFormat:@"%zu", i + 1]; // "1".."N"
                NSString* label = [NSString stringWithUTF8String:buttons[i].c_str()] ?: @"";
                UNNotificationAction* a =
                    [UNNotificationAction actionWithIdentifier:aid
                                                          title:label
                                                        options:0];
                [acts addObject:a];
            }
            // Do NOT pass UNNotificationCategoryOptionCustomDismissAction:
            // we already fire the c=1 untracked close at send time, and an
            // explicit dismiss callback would double-fire onClosed.
            UNNotificationCategory* cat =
                [UNNotificationCategory
                    categoryWithIdentifier:[NSString stringWithUTF8String:catId.c_str()]
                                   actions:acts
                         intentIdentifiers:@[]
                                   options:0];
            [g_categories addObject:cat];
            g_categoryByHash.emplace(h, catId);
        } else {
            catId = cit->second;
        }
        // setNotificationCategories: is replace-all — register the union
        // every time so previously-registered categories don't disappear.
        [center setNotificationCategories:g_categories];
        content.categoryIdentifier = [NSString stringWithUTF8String:catId.c_str()];
    }

    std::string identifier = buildIdentifier(sourceTag, clientId);

    // Fire c=1 close-response immediately — UN doesn't deliver swipe-away
    // or auto-expiry callbacks, so we follow kitty's "supports_close_events
    // = False" pattern (notifications.py:521, 991-992) and tell the program
    // up front that the close-response is unreliable on this platform. Wire
    // form is built by the caller's onClosed lambda in Platform_Tabs.cpp.
    // Reentrancy is fine: platformSendNotification is invoked from
    // eventLoop_->post(...), not from inside the OSC parser.
    if (closeResponseRequested && onClosed) onClosed("untracked");

    // Track for activation routing + active-notification queries even when
    // c=1 is not set: the body click / button click still needs to find the
    // onActivated callback. Untracked-send (clientId empty) gets a counter
    // suffix in the identifier so it can't collide.
    InFlight entry;
    entry.sourceTag    = sourceTag;
    entry.clientId     = clientId;
    entry.onClosed     = std::move(onClosed);
    entry.onActivated  = std::move(onActivated);
    rememberInFlight(identifier, std::move(entry));

    UNNotificationRequest* request = [UNNotificationRequest
        requestWithIdentifier:[NSString stringWithUTF8String:identifier.c_str()]
                      content:content
                      trigger:nil];
    [center addNotificationRequest:request withCompletionHandler:^(NSError* postErr) {
        if (postErr) {
            spdlog::warn("Notifications: post failed: {}",
                         [[postErr localizedDescription] UTF8String]);
        }
    }];
}

void platformCloseNotification(const std::string& sourceTag,
                               const std::string& clientId)
{
    if (![[NSBundle mainBundle] bundleIdentifier]) return;
    if (clientId.empty()) return; // untracked sends can't be addressed by id
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    std::string identifier = sourceTag + kIdSep + clientId;
    NSString* nsId = [NSString stringWithUTF8String:identifier.c_str()];
    if (!nsId) return;
    NSArray<NSString*>* arr = @[nsId];
    [center removeDeliveredNotificationsWithIdentifiers:arr];
    [center removePendingNotificationRequestsWithIdentifiers:arr];
    // Drop our routing entry. Don't synthesize onActivated/onClosed —
    // c=1's untracked already fired at send time, and synthesizing
    // onActivated would lie about user intent.
    forgetInFlight(identifier);
}

std::vector<std::string> platformActiveNotifications(const std::string& sourceTag)
{
    if (![[NSBundle mainBundle] bundleIdentifier]) return {};
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];

    // Reconcile the cached in-flight set against UN's delivered set: handles
    // swipe-away (which fires no callback), and acts as the GC point for
    // notifications that left without a removeDelivered call.
    __block NSArray<UNNotification*>* delivered = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [center getDeliveredNotificationsWithCompletionHandler:^(NSArray<UNNotification*>* arr) {
        delivered = arr;
        dispatch_semaphore_signal(sem);
    }];
    // 50 ms cap — UN normally responds in microseconds; a forever wait
    // under system pressure would stall the main thread. On timeout we
    // fall back to the cached set (over-reports, acceptable).
    long timedOut = dispatch_semaphore_wait(
        sem, dispatch_time(DISPATCH_TIME_NOW, 50LL * NSEC_PER_MSEC));

    std::vector<std::string> out;
    if (timedOut != 0) {
        // Fallback: report the cached set unmodified.
        for (auto& kv : g_inflight) {
            if (kv.second.sourceTag == sourceTag && !kv.second.clientId.empty())
                out.push_back(kv.second.clientId);
        }
        return out;
    }

    std::unordered_set<std::string> live;
    live.reserve(delivered.count);
    for (UNNotification* n in delivered) {
        NSString* idn = n.request.identifier;
        if (idn) live.emplace([idn UTF8String]);
    }

    // GC: drop entries from g_inflight that aren't in the delivered set.
    // Iterate by copying the keys first to avoid invalidating the deque.
    std::vector<std::string> toDrop;
    for (auto& kv : g_inflight) {
        if (live.find(kv.first) == live.end()) toDrop.push_back(kv.first);
    }
    for (const auto& id : toDrop) forgetInFlight(id);

    // Build response from the now-reconciled in-flight set.
    for (auto& kv : g_inflight) {
        if (kv.second.sourceTag == sourceTag && !kv.second.clientId.empty())
            out.push_back(kv.second.clientId);
    }
    return out;
}

void platformOpenURL(const std::string& url)
{
    NSString* nsUrl = [NSString stringWithUTF8String:url.c_str()];
    if (!nsUrl) {
        spdlog::warn("openURL: input is not valid UTF-8");
        return;
    }
    NSURL* parsed = [NSURL URLWithString:nsUrl];
    if (!parsed) {
        spdlog::warn("openURL: malformed URL: {}", url);
        return;
    }
    NSWorkspaceOpenConfiguration* cfg = [NSWorkspaceOpenConfiguration configuration];
    [[NSWorkspace sharedWorkspace] openURL:parsed
                             configuration:cfg
                         completionHandler:^(NSRunningApplication* app, NSError* err) {
        (void)app;
        if (err) {
            spdlog::warn("openURL failed: {}",
                         [[err localizedDescription] UTF8String]);
        }
    }];
}

std::string platformProcessCWD(pid_t pid)
{
    // Works without entitlements for child processes of the same UID, which
    // is the only normal call site (PTY descendants). Cross-UID PIDs and
    // other-team-id processes silently return empty under the hardened
    // runtime — there's no error indicator beyond the empty result.
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) > 0)
        return vpi.pvi_cdir.vip_path;
    return {};
}

void platformObserveAppearanceChanges(std::function<void(bool isDark)> callback)
{
    g_appearanceCallback = std::move(callback);

    if (g_appearanceObserver) {
        [[NSDistributedNotificationCenter defaultCenter] removeObserver:g_appearanceObserver];
        g_appearanceObserver = nil;
    }

    g_appearanceObserver = [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"AppleInterfaceThemeChangedNotification"
        object:nil
        queue:[NSOperationQueue mainQueue]
        usingBlock:^(NSNotification*) {
            if (g_appearanceCallback) {
                g_appearanceCallback(platformIsDarkMode());
            }
        }];
}
