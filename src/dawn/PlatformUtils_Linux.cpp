#include <cstdlib>
#include <functional>
#include <string>

bool platformIsDarkMode()
{
    // TODO: query xdg-desktop-portal or gsettings for dark mode preference
    return true;
}

void platformObserveAppearanceChanges(std::function<void(bool isDark)> /*callback*/)
{
    // TODO: monitor org.freedesktop.appearance.color-scheme via D-Bus
}

void platformSendNotification(const std::string& title, const std::string& body)
{
    // TODO: use libnotify or D-Bus org.freedesktop.Notifications
    std::string cmd = "notify-send ";
    cmd += "'" + title + "' '" + body + "' 2>/dev/null &";
    (void)system(cmd.c_str());
}

void platformOpenURL(const std::string& url)
{
    std::string cmd = "xdg-open '" + url + "' 2>/dev/null &";
    (void)system(cmd.c_str());
}
