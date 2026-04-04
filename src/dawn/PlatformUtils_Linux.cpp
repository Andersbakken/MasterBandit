#include <functional>

bool platformIsDarkMode()
{
    // TODO: query xdg-desktop-portal or gsettings for dark mode preference
    return true;
}

void platformObserveAppearanceChanges(std::function<void(bool isDark)> /*callback*/)
{
    // TODO: monitor org.freedesktop.appearance.color-scheme via D-Bus
}
