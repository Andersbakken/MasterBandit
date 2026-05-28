#include "CLIClient.h"
#include "Config.h"
#include "PlatformDawn.h"
#include "Resources.h"
#include "Terminfo.h"
#include "Version.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cxxopts.hpp>
#include <execinfo.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <elf.h>
#include <link.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

// Sticky flag flipped by main() once it commits to the headless/IPC code
// path. The signal handlers consult it to decide whether they also need
// to remove the IPC socket file before re-raising. Marked volatile +
// sig_atomic_t because it's read from a signal handler.
static volatile sig_atomic_t g_socketCleanupOnExit = 0;

// Crash-log directory, populated by composeCrashLogDir at startup.
static char g_crashLogDir[512];
static bool g_crashLogDirReady = false;

// Build-id (GNU build-id on Linux, LC_UUID on macOS) as lowercase hex.
static char g_buildIdHex[80];
static bool g_buildIdReady = false;

// Alt-stack so the fatal-signal handler can run when the main stack is
// exhausted.
static constexpr size_t kAltStackSize = 128 * 1024;
static uint8_t g_altStack[kAltStackSize];

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

// Async-signal-safe write that retries on EINTR and short writes.
static void writeAllSig(int fd, const char *s, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t r = write(fd, s + written, len - written);
        if (r > 0) {
            written += static_cast<size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void writeStrSig(int fd, const char *s)
{
    if (!s) {
        return;
    }
    size_t len = 0;
    while (s[len]) {
        ++len;
    }
    writeAllSig(fd, s, len);
}

#ifdef __linux__
namespace {
struct BuildIdProbe
{
    int idx;
    char *out;
    size_t outSize;
    bool found;
};
} // namespace

static int buildIdPhdrCallback(struct dl_phdr_info *info, size_t, void *data)
{
    auto *probe = static_cast<BuildIdProbe *>(data);
    if (probe->idx++ != 0) {
        return 1; // only inspect the main image (index 0)
    }
    for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_NOTE) {
            continue;
        }
        const uint8_t *base = reinterpret_cast<const uint8_t *>(info->dlpi_addr + phdr->p_vaddr);
        size_t off          = 0;
        while (off + sizeof(ElfW(Nhdr)) <= phdr->p_memsz) {
            const ElfW(Nhdr) *nhdr = reinterpret_cast<const ElfW(Nhdr) *>(base + off);
            size_t nameOff         = off + sizeof(ElfW(Nhdr));
            size_t descOff         = nameOff + ((nhdr->n_namesz + 3) & ~3u);
            if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz == 4 && memcmp(base + nameOff, "GNU", 4) == 0 && descOff + nhdr->n_descsz <= phdr->p_memsz) {
                const uint8_t *desc = base + descOff;
                size_t descLen      = nhdr->n_descsz;
                if (descLen * 2 + 1 > probe->outSize) {
                    descLen = (probe->outSize - 1) / 2;
                }
                for (size_t k = 0; k < descLen; ++k) {
                    snprintf(probe->out + k * 2, 3, "%02x", desc[k]);
                }
                probe->out[descLen * 2] = '\0';
                probe->found            = true;
                return 1;
            }
            off = descOff + ((nhdr->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

static void captureBuildId()
{
    BuildIdProbe probe { 0, g_buildIdHex, sizeof(g_buildIdHex), false };
    dl_iterate_phdr(buildIdPhdrCallback, &probe);
    g_buildIdReady = probe.found;
}
#endif // __linux__

#ifdef __APPLE__
static void captureBuildId()
{
    const auto *hdr = reinterpret_cast<const struct mach_header_64 *>(_dyld_get_image_header(0));
    if (!hdr) {
        return;
    }
    const uint8_t *cmds = reinterpret_cast<const uint8_t *>(hdr + 1);
    size_t off          = 0;
    for (uint32_t i = 0; i < hdr->ncmds; ++i) {
        const auto *lc = reinterpret_cast<const struct load_command *>(cmds + off);
        if (lc->cmd == LC_UUID) {
            const auto *uc = reinterpret_cast<const struct uuid_command *>(lc);
            for (int k = 0; k < 16; ++k) {
                snprintf(g_buildIdHex + k * 2, 3, "%02x", uc->uuid[k]);
            }
            g_buildIdHex[32] = '\0';
            g_buildIdReady   = true;
            return;
        }
        off += lc->cmdsize;
    }
}
#endif // __APPLE__

static void dumpLoadMap(int fd)
{
#ifdef __linux__
    int mapsFd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (mapsFd < 0) {
        writeStrSig(fd, "(/proc/self/maps unavailable)\n");
        return;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(mapsFd, buf, sizeof(buf))) > 0) {
        writeAllSig(fd, buf, static_cast<size_t>(n));
    }
    close(mapsFd);
#elif defined(__APPLE__)
    writeStrSig(fd, "images:\n");
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        intptr_t slide   = _dyld_get_image_vmaddr_slide(i);
        const auto *hdr  = _dyld_get_image_header(i);
        char line[1024];
        int len = snprintf(line, sizeof(line), "  base=%p slide=0x%lx %s\n", reinterpret_cast<const void *>(hdr), static_cast<unsigned long>(slide), name ? name : "(unknown)");
        if (len > 0 && len < static_cast<int>(sizeof(line))) {
            writeAllSig(fd, line, static_cast<size_t>(len));
        }
    }
#else
    (void)fd;
#endif
}

static void writeCrashHeader(int fd, int sig)
{
    writeStrSig(fd, "=== mb crash ===\nversion: ");
    writeStrSig(fd, mb::kVersion);
    writeStrSig(fd, "\nbuild-id: ");
    writeStrSig(fd, g_buildIdReady ? g_buildIdHex : "(unavailable)");
    writeStrSig(fd, "\n");
    char numbuf[64];
    int n = snprintf(numbuf, sizeof(numbuf), "signal: %d\npid: %d\n", sig, getpid());
    if (n > 0) {
        writeAllSig(fd, numbuf, static_cast<size_t>(n));
    }
}

// Writes the crash payload (header, load map, backtrace) to stderr and
// to a crash-log file under g_crashLogDir, then re-raises.
static void crashSignalHandler(int sig, siginfo_t *, void *)
{
    int fd = -1;
    char path[768];
    if (g_crashLogDirReady) {
        int n = snprintf(path, sizeof(path), "%s/crash-%lld-%d.log", g_crashLogDir, static_cast<long long>(time(nullptr)), getpid());
        if (n > 0 && n < static_cast<int>(sizeof(path))) {
            fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
        }
    }

    int sinks[2] = { STDERR_FILENO, fd };
    for (int s = 0; s < 2; ++s) {
        int dst = sinks[s];
        if (dst < 0) {
            continue;
        }
        writeStrSig(dst, "\n");
        writeCrashHeader(dst, sig);
        dumpLoadMap(dst);
        writeStrSig(dst, "backtrace:\n");
        constexpr int kMaxFrames = 128;
        void *frames[kMaxFrames];
        int count = backtrace(frames, kMaxFrames);
        // _fd variant only: backtrace_symbols calls malloc.
        backtrace_symbols_fd(frames, count, dst);
    }

    if (fd >= 0) {
        writeStrSig(STDERR_FILENO, "mb: crash log written to ");
        writeStrSig(STDERR_FILENO, path);
        writeStrSig(STDERR_FILENO, "\n");
        close(fd);
    }

    if (g_socketCleanupOnExit) {
        removeIpcSocket();
    }

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

static void composeCrashLogDir()
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        if (struct passwd *pw = getpwuid(getuid())) {
            home = pw->pw_dir;
        }
    }
    if (!home || !home[0]) {
        g_crashLogDirReady = false;
        return;
    }

#ifdef __APPLE__
    int n = snprintf(g_crashLogDir, sizeof(g_crashLogDir), "%s/Library/Logs/mb", home);
#else
    int n = snprintf(g_crashLogDir, sizeof(g_crashLogDir), "%s/.cache/mb/crashes", home);
#endif
    if (n <= 0 || n >= static_cast<int>(sizeof(g_crashLogDir))) {
        g_crashLogDirReady = false;
        return;
    }

    char tmp[sizeof(g_crashLogDir)];
    snprintf(tmp, sizeof(tmp), "%s", g_crashLogDir);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);

    struct stat st;
    g_crashLogDirReady = (stat(g_crashLogDir, &st) == 0) && S_ISDIR(st.st_mode);
}

static void installCrashSignalHandlers()
{
    composeCrashLogDir();
    captureBuildId();

    stack_t ss {};
    ss.ss_sp    = g_altStack;
    ss.ss_size  = sizeof(g_altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa {};
    sa.sa_sigaction = crashSignalHandler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGSEGV);
    sigaddset(&sa.sa_mask, SIGBUS);
    sigaddset(&sa.sa_mask, SIGABRT);
    sigaddset(&sa.sa_mask, SIGILL);
    sigaddset(&sa.sa_mask, SIGFPE);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;

    // SIGSEGV/SIGBUS = memory faults; SIGABRT covers assert/abort/
    // std::terminate; SIGILL/SIGFPE catch the rest of the hardware traps.
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
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
    opts.add_options()("v,verbose", "Increase verbosity (repeatable)")("log", "Set subsystem log level: name=level[,name=level] (subsystems: script,js,render,terminal,input,font)", cxxopts::value<std::string>())("s,shell", "Shell to use", cxxopts::value<std::string>())("test", "Headless test mode (no window, no config)")("ipc", "Enable debug IPC socket")("x11", "Force the X11 window backend on Linux (overrides WAYLAND_DISPLAY auto-detect; ignored elsewhere)")("wayland", "Force the Wayland window backend on Linux (overrides DISPLAY auto-detect; ignored elsewhere)")("font", "Font path (test mode)", cxxopts::value<std::string>())("emoji-font", "Emoji font path (test mode)", cxxopts::value<std::string>())("fallback-font", "Additional fallback font path (test mode)", cxxopts::value<std::string>())("cols", "Terminal columns (test mode)", cxxopts::value<int>()->default_value("80"))("rows", "Terminal rows (test mode)", cxxopts::value<int>()->default_value("24"))("font-size", "Font size (test mode)", cxxopts::value<float>()->default_value("16"))("emit-terminfo", "Emit terminfo source for xterm-mb to stdout and exit")("V,version", "Print version and exit")("h,help", "Print usage");
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
    if (result.count("x11") && result.count("wayland")) {
        fprintf(stderr, "mb: --x11 and --wayland are mutually exclusive\n");
        return 2;
    }
    if (result.count("x11")) {
        platformFlags |= PlatformDawn::FlagForceX11;
    }
    if (result.count("wayland")) {
        platformFlags |= PlatformDawn::FlagForceWayland;
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
        options.backgroundOpacity        = std::clamp(config.colors.background_opacity, 0.0f, 1.0f);
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
