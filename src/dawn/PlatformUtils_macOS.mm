#import <Cocoa/Cocoa.h>
#include <functional>

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
