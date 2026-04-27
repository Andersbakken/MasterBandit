#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <libproc.h>
#include <spdlog/spdlog.h>

// Foreground-banner toggle. Default: present banners even when mb is the
// active app — this is what a terminal user wants since long-running tasks
// are usually launched from inside mb (build complete, tests done, etc.).
static std::atomic<bool> g_showWhenForeground{true};

API_AVAILABLE(macos(10.14))
@interface MBNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation MBNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    if (!g_showWhenForeground.load()) {
        completionHandler(UNNotificationPresentationOptionNone);
        return;
    }
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound);
}
@end

static MBNotificationDelegate* g_notifDelegate = nil;

// macOS doesn't need a global init/shutdown step: notifications are wired
// up in platformInitNotifications, and appearance observing uses Cocoa's
// own NSDistributedNotificationCenter. These exist to match the
// cross-platform signature.
class EventLoop;
void platformInit(EventLoop& /*loop*/) {}
void platformShutdown() {}

void platformCloseNotification(const std::string& /*sourceTag*/,
                               const std::string& /*clientId*/)
{
    // TODO macOS: [UNUserNotificationCenter removeDeliveredNotificationsWithIdentifiers:].
}

std::vector<std::string> platformActiveNotifications(const std::string& /*sourceTag*/)
{
    // TODO macOS: query via getDeliveredNotificationsWithCompletionHandler:.
    // For now report none active; OSC 99 p=alive will reply with an empty list.
    return {};
}

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
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
        completionHandler:^(BOOL granted, NSError* error) {
            if (granted) {
                spdlog::info("Notifications: authorization granted");
            } else {
                const char* err = error ? [[error localizedDescription] UTF8String] : "(no error info)";
                spdlog::warn("Notifications: authorization not granted ({})", err);
            }
        }];
}

void platformSetNotificationsShowWhenForeground(bool show)
{
    g_showWhenForeground.store(show);
}

void platformSendNotification(const std::string& /*sourceTag*/,
                              const std::string& /*clientId*/,
                              const std::string& title, const std::string& body,
                              uint8_t /*urgency*/,
                              bool /*closeResponseRequested*/,
                              std::function<void(const std::string&)> /*onClosed*/)
{
    // TODO macOS: map urgency to UNNotificationContent.interruptionLevel
    // (passive/active/timeSensitive/critical), and use clientId as the
    // UNNotificationRequest identifier so re-issues replace in place.
    //
    // closeResponseRequested + onClosed currently dropped on macOS — the
    // delegate fires only on user-action invocations, not on swipe-away or
    // expiry, so the wire-back is at best partial. Pending macOS pass.
    //
    // Note for the macOS pass: when closeResponseRequested is set, fire
    // onClosed *immediately at send time* with reason "untracked" (kitty's
    // notifications.py:991-992 path). UNUserNotificationCenter doesn't
    // emit a callback on swipe-away/auto-expire, so we can't honor c=1
    // properly — telling the program up front that the close-response is
    // untracked matches kitty's MacOSIntegration (supports_close_events =
    // False). Wire form: \e]99;i=<id>:p=close;untracked\a — written via
    // term->writeText from inside Platform_Tabs.cpp's onClosed lambda.
    if (![[NSBundle mainBundle] bundleIdentifier]) return;
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    // Convert to NSString before any async hop. Block-copy semantics
    // retain captured Objective-C objects, so the NSStrings outlive the
    // caller's std::string parameters; capturing those C++ references
    // directly would be a use-after-free when the block fires later.
    NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
    NSString* nsBody = [NSString stringWithUTF8String:body.c_str()];
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = nsTitle;
    content.body = nsBody;
    content.sound = [UNNotificationSound defaultSound];
    UNNotificationRequest* request = [UNNotificationRequest
        requestWithIdentifier:[[NSUUID UUID] UUIDString]
        content:content
        trigger:nil];
    [center addNotificationRequest:request withCompletionHandler:^(NSError* postErr) {
        if (postErr) {
            spdlog::warn("Notifications: post failed: {}",
                         [[postErr localizedDescription] UTF8String]);
        }
    }];
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
