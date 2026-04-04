#include <unistd.h>
#include "Terminal.h"
#include "Config.h"
#include <getopt.h>
#include <cstring>
#include <pwd.h>
#include "Log.h"
#include "CLIClient.h"

void usage(FILE *f)
{
    fprintf(f, "Usage: mb [-v] [-s shell]\n");
    fflush(f);
}

static std::string defaultShell(const std::string &user)
{
    if (const char *shell = getenv("SHELL")) {
        return shell;
    }
    if (struct passwd *pw = getpwnam(user.c_str())) {
        if (pw->pw_shell && pw->pw_shell[0])
            return pw->pw_shell;
    }
    return "/bin/sh";
}

int main(int argc, char **argv)
{
    // Check for --ctl flag: if present, run as CLI client
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctl") == 0) {
            return runCLI(argc, argv);
        }
    }

    std::unique_ptr<Platform> platform = createPlatform(argc, argv);
    if (!platform) {
        fprintf(stderr, "Failed to create platform\n");
        return 1;
    }
    Config config = loadConfig();

    TerminalOptions options;
    options.font = config.font;
    options.fontSize = config.font_size;
    options.boldStrength = config.bold_strength;
    options.scrollbackLines = config.scrollback_lines < 0 ? std::nullopt : std::optional<int>(config.scrollback_lines);
    options.tabBar = config.tab_bar;
    options.keybindings = config.keybindings;
    options.dividerColor          = config.divider_color;
    options.dividerWidth          = config.divider_width;
    options.inactivePaneTint      = config.inactive_pane_tint;
    options.inactivePaneTintAlpha = config.inactive_pane_tint_alpha;
    options.activePaneTint        = config.active_pane_tint;
    options.activePaneTintAlpha   = config.active_pane_tint_alpha;

    char buf[1024];
    if (!getlogin_r(buf, sizeof(buf))) {
        options.user = buf;
    } else if (const char *u = getenv("USER")) {
        options.user = u;
    } else if (const char *un = getenv("USERNAME")) {
        options.user = un;
    } else {
        fprintf(stderr, "Can't find user\n");
        return 1;
    }

    options.shell = defaultShell(options.user);
    int logLevel = Log::logLevel();
    int opt;
    while ((opt = getopt(argc, argv, ":vs:")) != -1) {
        switch (opt) {
        case 'v':
            if (logLevel > 0)
                --logLevel;
            break;
        case 's':
            options.shell = optarg;
            break;
        default:
            usage(stderr);
            return 2;
        }
    }
    Log::setLogLevel(static_cast<Log::Level>(logLevel));

    // Terminal is now owned by the platform's layout — createTerminal returns nullptr on success.
    if (!platform->createTerminal(options)) {
        // null is normal; a failed setup logs its own error and exec() will return early.
    }
    return platform->exec();
}
