#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>
#include <functional>
#include <string>

bool platformIsDarkMode()
{
    if (@available(macOS 10.14, *)) {
        NSAppearance* appearance = [NSApp effectiveAppearance];
        NSAppearanceName best = [appearance bestMatchFromAppearancesWithNames:
            @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        return [best isEqualToString:NSAppearanceNameDarkAqua];
    }
    return true;
}

static std::function<void(bool)> g_appearanceCallback;
static id g_appearanceObserver = nil;

void platformSendNotification(const std::string& title, const std::string& body)
{
    if (@available(macOS 10.14, *)) {
        UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
        [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
            completionHandler:^(BOOL granted, NSError* error) {
                if (!granted) return;
                UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
                content.title = [NSString stringWithUTF8String:title.c_str()];
                content.body = [NSString stringWithUTF8String:body.c_str()];
                content.sound = [UNNotificationSound defaultSound];

                UNNotificationRequest* request = [UNNotificationRequest
                    requestWithIdentifier:[[NSUUID UUID] UUIDString]
                    content:content
                    trigger:nil];
                [center addNotificationRequest:request withCompletionHandler:nil];
            }];
    }
}

void platformOpenURL(const std::string& url)
{
    NSString* nsUrl = [NSString stringWithUTF8String:url.c_str()];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:nsUrl]];
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
