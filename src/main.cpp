#include "CLIClient.h"
#include "Config.h"
#include "PlatformDawn.h"
#include "Resources.h"
#include "Terminfo.h"
#include "Version.h"
#include <cstdio>
#include <cstring>
#include <cxxopts.hpp>
#include <execinfo.h>
#include <pwd.h>
#include <signal.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <unistd.h>

// Sticky flag flipped by main() once it commits to the headless/IPC code
// path. The signal handlers consult it to decide whether they also need
// to remove the IPC socket file before re-raising. Marked volatile +
// sig_atomic_t because it's read from a signal handler.
static volatile sig_atomic_t g_socketCleanupOnExit = 0;

// Async-signal-safe removal of the per-pid IPC socket. snprintf is not
// formally on POSIX's signal-safe list, but every libc implementation
// we target uses a stack buffer for `%d` formatting with no locks, and
// the existing handler has used it without issue.
static void removeIpcSocket()
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/mb-%d.sock", getpid());
    unlink(path);
}

// Async-signal-safe write of a NUL-terminated string. Used by the crash
// handler in place of fprintf / spdlog, neither of which is safe to
// call from a signal handler.
static void writeStrSig(int fd, const char *s)
{
    size_t len = 0;
    while (s[len]) {
        ++len;
    }
    [[maybe_unused]] auto n = write(fd, s, len);
}

// Fatal-signal handler: dumps a backtrace to stderr, cleans up the IPC
// socket if applicable, then re-raises with the default handler to get
// the correct exit status (and a core file if rlimits allow).
//
// Uses backtrace_symbols_fd, not backtrace_symbols, because the latter
// calls malloc — not async-signal-safe. After a SIGSEGV the allocator
// state can be arbitrarily corrupt, and calling malloc would risk
// deadlock or a second crash that swallows the original trace.
static void crashSignalHandler(int sig)
{
    writeStrSig(STDERR_FILENO, "\nmb: fatal signal ");
    char numbuf[32];
    int n = snprintf(numbuf, sizeof(numbuf), "%d\nstack trace:\n", sig);
    if (n > 0) {
        [[maybe_unused]] auto w = write(STDERR_FILENO, numbuf, static_cast<size_t>(n));
    }

    constexpr int kMaxFrames = 128;
    void *frames[kMaxFrames];
    int count = backtrace(frames, kMaxFrames);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);

    if (g_socketCleanupOnExit) {
        removeIpcSocket();
    }

    // Re-raise under the default handler so the exit status reflects
    // the real signal (and a core file gets written if ulimit permits).
    signal(sig, SIG_DFL);
    raise(sig);
}

// Graceful-shutdown handler for SIGTERM/SIGINT in headless/IPC mode.
// Performs socket cleanup and re-raises; no backtrace because these
// aren't crashes.
static void cleanupSocketAndExit(int sig)
{
    if (g_socketCleanupOnExit) {
        removeIpcSocket();
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashSignalHandlers()
{
    // Install for every signal that indicates a programming error or
    // hardware fault. SIGABRT covers assert() / abort() / std::terminate
    // and so is one of the most useful in practice.
    signal(SIGSEGV, crashSignalHandler);
    signal(SIGBUS, crashSignalHandler);
    signal(SIGABRT, crashSignalHandler);
    signal(SIGILL, crashSignalHandler);
    signal(SIGFPE, crashSignalHandler);
}

static std::string defaultShell(const std::string &user)
{
    if (const char *shell = getenv("SHELL")) {
        return shell;
    }
    if (struct passwd *pw = getpwnam(user.c_str())) {
        if (pw->pw_shell && pw->pw_shell[0]) {
            return pw->pw_shell;
        }
    }
    return "/bin/sh";
}

int main(int argc, char **argv)
{
    // Install before anything else (including the --ctl branch and any
    // cxxopts work) so even early-startup crashes get a backtrace.
    installCrashSignalHandlers();

    // Check for --ctl flag: if present, run as CLI client (before cxxopts,
    // because the CLI client has its own arg parsing)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctl") == 0) {
            return runCLI(argc, argv);
        }
    }

    cxxopts::Options opts("mb", "MasterBandit Terminal");
    opts.add_options()("v,verbose", "Increase verbosity (repeatable)")("log", "Set subsystem log level: name=level[,name=level] (subsystems: script,js,render,terminal,input,font)", cxxopts::value<std::string>())("s,shell", "Shell to use", cxxopts::value<std::string>())("test", "Headless test mode (no window, no config)")("ipc", "Enable debug IPC socket")("font", "Font path (test mode)", cxxopts::value<std::string>())("emoji-font", "Emoji font path (test mode)", cxxopts::value<std::string>())("fallback-font", "Additional fallback font path (test mode)", cxxopts::value<std::string>())("cols", "Terminal columns (test mode)", cxxopts::value<int>()->default_value("80"))("rows", "Terminal rows (test mode)", cxxopts::value<int>()->default_value("24"))("font-size", "Font size (test mode)", cxxopts::value<float>()->default_value("16"))("emit-terminfo", "Emit terminfo source for xterm-mb to stdout and exit")("V,version", "Print version and exit")("h,help", "Print usage");
    opts.allow_unrecognised_options();

    cxxopts::ParseResult result;
    try {
        result = opts.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        fprintf(stderr, "%s\n", e.what());
        fprintf(stderr, "%s\n", opts.help().c_str());
        return 2;
    }

    if (result.count("help")) {
        printf("%s\n", opts.help().c_str());
        return 0;
    }

    if (result.count("version")) {
        printf("mb %s\n", mb::kVersion);
        return 0;
    }

    if (result.count("emit-terminfo")) {
        // Pure data emission from the live XTGETTCAP table — no platform,
        // config, or logger needed.
        std::string s = emitTerminfoSource();
        fwrite(s.data(), 1, s.size(), stdout);
        return 0;
    }

    bool testMode          = result.count("test") > 0;
    uint32_t platformFlags = PlatformDawn::FlagNone;
    if (testMode) {
        platformFlags |= PlatformDawn::FlagHeadless;
    }
    if (result.count("ipc")) {
        platformFlags |= PlatformDawn::FlagIPC;
    }

    // Configure logging before anything else so no messages slip through at wrong level.
    static constexpr spdlog::level::level_enum kLevelMap[] = {
        spdlog::level::trace,    // 0 (-v -v -v -v)
        spdlog::level::debug,    // 1 (-v -v -v)
        spdlog::level::info,     // 2 (-v -v)
        spdlog::level::warn,     // 3 (-v)
        spdlog::level::err,      // 4 (default)
        spdlog::level::critical, // 5
        spdlog::level::off,      // 6
    };
    static auto parseSpdLevel = [](const std::string &s) -> spdlog::level::level_enum
    {
        if (s == "trace" || s == "verbose") {
            return spdlog::level::trace;
        }
        if (s == "debug") {
            return spdlog::level::debug;
        }
        if (s == "info") {
            return spdlog::level::info;
        }
        if (s == "warn" || s == "warning") {
            return spdlog::level::warn;
        }
        if (s == "error" || s == "err") {
            return spdlog::level::err;
        }
        if (s == "critical" || s == "fatal") {
            return spdlog::level::critical;
        }
        if (s == "off" || s == "silent") {
            return spdlog::level::off;
        }
        return spdlog::level::err;
    };

    int logLevel  = 4; // Error
    int verbosity = static_cast<int>(result.count("verbose"));
    for (int i = 0; i < verbosity; i++) {
        if (logLevel > 0) {
            --logLevel;
        }
    }
    const auto globalLevel = kLevelMap[logLevel];

    static const char *kSubsystems[] = { "script", "render", "terminal", "input", "font", nullptr };

    std::vector<spdlog::sink_ptr> sharedSinks;
    try {
        sharedSinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/mb.log", true));
    } catch (...) {
    }
    sharedSinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

    auto makeLogger = [&](const char *name, spdlog::level::level_enum lvl)
    {
        auto l = std::make_shared<spdlog::logger>(name, sharedSinks.begin(), sharedSinks.end());
        l->set_level(lvl);
        l->flush_on(spdlog::level::trace);
        spdlog::register_logger(l);
        return l;
    };

    spdlog::set_level(globalLevel);
    spdlog::set_default_logger(makeLogger("mb", globalLevel));
    for (int i = 0; kSubsystems[i]; ++i) {
        makeLogger(kSubsystems[i], globalLevel);
    }
    makeLogger("js", globalLevel < spdlog::level::info ? globalLevel : spdlog::level::info);

    spdlog::info("mb {}", mb::kVersion);

    if (result.count("log")) {
        std::string spec = result["log"].as<std::string>();
        std::istringstream ss(spec);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string name  = token.substr(0, eq);
            std::string level = token.substr(eq + 1);
            if (auto logger = spdlog::get(name)) {
                logger->set_level(parseSpdLevel(level));
            } else {
                fprintf(stderr, "warning: unknown log subsystem '%s'\n", name.c_str());
            }
        }
    }

    auto platform = createPlatform(argc, argv, platformFlags);
    if (!platform) {
        fprintf(stderr, "Failed to create platform\n");
        return 1;
    }

    if (testMode) {
        platform->setTestConfig(
            result.count("font") ? result["font"].as<std::string>() : std::string {},
            result["cols"].as<int>(),
            result["rows"].as<int>(),
            result["font-size"].as<float>(),
            result.count("emoji-font") ? result["emoji-font"].as<std::string>() : std::string {},
            result.count("fallback-font") ? result["fallback-font"].as<std::string>() : std::string {});
    }

    TerminalOptions options;
    if (!testMode) {
        Config config                    = loadConfig();
        options.font                     = config.font;
        options.fontSize                 = config.font_size;
        options.boldStrength             = config.bold_strength;
        options.scrollbackLines          = config.scrollback_lines < 0 ? std::nullopt : std::optional<int>(config.scrollback_lines);
        options.tabBar                   = config.tab_bar;
        options.keybindings              = config.keybindings;
        options.mousebindings            = config.mousebindings;
        options.dividerColor             = config.divider_color;
        options.dividerWidth             = config.divider_width;
        options.inactivePaneTint         = config.inactive_pane_tint;
        options.inactivePaneTintAlpha    = config.inactive_pane_tint_alpha;
        options.activePaneTint           = config.active_pane_tint;
        options.activePaneTintAlpha      = config.active_pane_tint_alpha;
        options.replacementChar          = config.replacement_char;
        options.padding                  = config.padding;
        options.cursor                   = config.cursor;
        options.colors                   = config.colors;
        options.shellIntegration         = config.shell_integration;
        options.shellIntegrationDir      = Resources::path("shell-integration").string();
        options.shellIntegrationFeatures = config.shell_integration_features;
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

    if (result.count("shell")) {
        options.shell = result["shell"].as<std::string>();
    }

    if (platformFlags & (PlatformDawn::FlagHeadless | PlatformDawn::FlagIPC)) {
        // SIGSEGV/SIGBUS/SIGABRT/SIGILL/SIGFPE are already handled by
        // crashSignalHandler (installed at the top of main); this flag
        // tells both handlers to also remove the per-pid IPC socket.
        g_socketCleanupOnExit = 1;
        signal(SIGTERM, cleanupSocketAndExit);
        signal(SIGINT, cleanupSocketAndExit);
    }

    platform->createTerminal(options);
    return platform->exec();
}
