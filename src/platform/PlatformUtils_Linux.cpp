#include <functional>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>

bool platformIsDarkMode()
{
    // TODO: query xdg-desktop-portal or gsettings for dark mode preference
    return true;
}

void platformObserveAppearanceChanges(std::function<void(bool isDark)> /*callback*/)
{
    // TODO: monitor org.freedesktop.appearance.color-scheme via D-Bus
}

static void spawnDetached(const char* path, char* const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        // Double-fork to avoid zombie
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

void platformSendNotification(const std::string& title, const std::string& body)
{
    // TODO: use libnotify or D-Bus org.freedesktop.Notifications
    char* argv[] = {
        const_cast<char*>("notify-send"),
        const_cast<char*>(title.c_str()),
        const_cast<char*>(body.c_str()),
        nullptr
    };
    spawnDetached("notify-send", argv);
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
