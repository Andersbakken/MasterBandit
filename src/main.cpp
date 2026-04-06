#include <unistd.h>
#include <signal.h>
#include "PlatformDawn.h"
#include "Config.h"
#include <cstring>
#include <pwd.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sstream>
#include "CLIClient.h"
#include <cxxopts.hpp>

static void cleanupSocketAndExit(int sig)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/mb-%d.sock", getpid());
    unlink(path);
    // Re-raise with default handler to get correct exit status
    signal(sig, SIG_DFL);
    raise(sig);
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
    // Check for --ctl flag: if present, run as CLI client (before cxxopts,
    // because the CLI client has its own arg parsing)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctl") == 0) {
            return runCLI(argc, argv);
        }
    }

    cxxopts::Options opts("mb", "MasterBandit Terminal");
    opts.add_options()
        ("v,verbose", "Increase verbosity (repeatable)")
        ("log", "Set subsystem log level: name=level[,name=level] (subsystems: script,js,render,terminal,input,font)", cxxopts::value<std::string>())
        ("s,shell", "Shell to use", cxxopts::value<std::string>())
        ("test", "Headless test mode (no window, no config)")
        ("font", "Font path (test mode)", cxxopts::value<std::string>())
        ("cols", "Terminal columns (test mode)", cxxopts::value<int>()->default_value("80"))
        ("rows", "Terminal rows (test mode)", cxxopts::value<int>()->default_value("24"))
        ("font-size", "Font size (test mode)", cxxopts::value<float>()->default_value("16"))
        ("h,help", "Print usage");
    opts.allow_unrecognised_options();

    cxxopts::ParseResult result;
    try {
        result = opts.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        fprintf(stderr, "%s\n", opts.help().c_str());
        return 2;
    }

    if (result.count("help")) {
        printf("%s\n", opts.help().c_str());
        return 0;
    }

    bool testMode = result.count("test") > 0;

    auto platform = createPlatform(argc, argv, testMode);
    if (!platform) {
        fprintf(stderr, "Failed to create platform\n");
        return 1;
    }

    if (testMode) {
        platform->setTestConfig(
            result.count("font") ? result["font"].as<std::string>() : std::string{},
            result["cols"].as<int>(),
            result["rows"].as<int>(),
            result["font-size"].as<float>());
    }

    TerminalOptions options;
    if (!testMode) {
        Config config = loadConfig();
        options.font = config.font;
        options.fontSize = config.font_size;
        options.boldStrength = config.bold_strength;
        options.scrollbackLines = config.scrollback_lines < 0 ? std::nullopt : std::optional<int>(config.scrollback_lines);
        options.tabBar = config.tab_bar;
        options.keybindings = config.keybindings;
        options.mousebindings = config.mousebindings;
        options.dividerColor          = config.divider_color;
        options.dividerWidth          = config.divider_width;
        options.inactivePaneTint      = config.inactive_pane_tint;
        options.inactivePaneTintAlpha = config.inactive_pane_tint_alpha;
        options.activePaneTint        = config.active_pane_tint;
        options.activePaneTintAlpha   = config.active_pane_tint_alpha;
        options.replacementChar       = config.replacement_char;
        options.padding               = config.padding;
        options.colors                = config.colors;
    }

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

    if (result.count("shell"))
        options.shell = result["shell"].as<std::string>();

    // -v decrements log level: Error(default) → Warn → Debug → Verbose(trace)
    static constexpr spdlog::level::level_enum kLevelMap[] = {
        spdlog::level::trace,    // 0 Verbose
        spdlog::level::debug,    // 1 Debug
        spdlog::level::warn,     // 2 Warn
        spdlog::level::err,      // 3 Error (default)
        spdlog::level::critical, // 4 Fatal
        spdlog::level::off,      // 5 Silent
    };
    static auto parseSpdLevel = [](const std::string& s) -> spdlog::level::level_enum {
        if (s == "trace" || s == "verbose") return spdlog::level::trace;
        if (s == "debug")                   return spdlog::level::debug;
        if (s == "info")                    return spdlog::level::info;
        if (s == "warn" || s == "warning")  return spdlog::level::warn;
        if (s == "error" || s == "err")     return spdlog::level::err;
        if (s == "critical" || s == "fatal")return spdlog::level::critical;
        if (s == "off" || s == "silent")    return spdlog::level::off;
        return spdlog::level::err;
    };

    int logLevel = 3; // Error
    int verbosity = static_cast<int>(result.count("verbose"));
    for (int i = 0; i < verbosity; i++)
        if (logLevel > 0) --logLevel;
    const auto globalLevel = kLevelMap[logLevel];

    // Register subsystem loggers.
    // js: console.log output from scripts — default info so it's always visible.
    // All others default to the global level.
    static const char* kSubsystems[] = { "script", "render", "terminal", "input", "font", nullptr };
    spdlog::set_level(globalLevel);
    for (int i = 0; kSubsystems[i]; ++i)
        spdlog::stdout_color_mt(kSubsystems[i])->set_level(globalLevel);
    // js logger: always at info unless -v lowers global below info
    spdlog::stdout_color_mt("js")->set_level(
        globalLevel < spdlog::level::info ? globalLevel : spdlog::level::info);

    // --log subsystem=level[,subsystem=level] overrides
    if (result.count("log")) {
        std::string spec = result["log"].as<std::string>();
        std::istringstream ss(spec);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            std::string name  = token.substr(0, eq);
            std::string level = token.substr(eq + 1);
            if (auto logger = spdlog::get(name))
                logger->set_level(parseSpdLevel(level));
            else
                fprintf(stderr, "warning: unknown log subsystem '%s'\n", name.c_str());
        }
    }

    signal(SIGTERM, cleanupSocketAndExit);
    signal(SIGINT, cleanupSocketAndExit);
    signal(SIGSEGV, cleanupSocketAndExit);
    signal(SIGBUS, cleanupSocketAndExit);
    signal(SIGABRT, cleanupSocketAndExit);

    platform->createTerminal(options);
    return platform->exec();
}
