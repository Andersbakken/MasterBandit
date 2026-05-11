// Smoke test for the zsh shell-integration assets.
//
// Spawns a real zsh under forkpty with ZDOTDIR pointed at our shipped
// integration scripts (the same path Terminal::init injects in
// production). Drives `false<CR>exit<CR>` and asserts that the integration
// emitted the expected OSC 133 markers — including 133;D;1 for the failed
// command, which proves the precmd hook correctly captured $? before any
// other code clobbered it.
//
// Skipped when zsh is not installed (defensive: most dev hosts have it,
// but mb-tests should not hard-fail on a stripped-down container).
//
// MB_TEST_SHELL_INTEGRATION_DIR is the absolute path to the asset root
// inside the build's resource tree, set by the test target's CMake.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__linux__)
#include <util.h> // openpty on macOS
#endif

#ifdef __linux__
#include <pty.h>
#endif

namespace {

const char *findZsh()
{
    static const char *candidates[] = { "/bin/zsh", "/usr/bin/zsh", "/usr/local/bin/zsh" };
    struct stat st;
    for (const char *p : candidates) {
        if (stat(p, &st) == 0 && (st.st_mode & S_IXUSR)) {
            return p;
        }
    }
    return nullptr;
}

// Read from fd until EOF, child exits, or deadline elapses.
std::string drain(int fd, std::chrono::milliseconds budget)
{
    std::string out;
    auto deadline = std::chrono::steady_clock::now() + budget;
    char buf[4096];
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec  = remaining.count() / 1000000;
        tv.tv_usec = remaining.count() % 1000000;

        const int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            break;
        }

        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, buf + n);
        } else {
            // n == 0  -> EOF (child closed PTY); n < 0 -> error (PTY hangup
            // is reported as EIO on Linux). Either way we're done.
            break;
        }
    }
    return out;
}

} // namespace

namespace {

struct ShellRun
{
    std::string output;
    std::string tmpHome;
};

// Spawn a real interactive zsh under forkpty against the shipped assets,
// drive `false<CR>exit<CR>`, and return the captured bytes plus the temp
// HOME (so callers can anchor path assertions on the unique mkdtemp
// suffix). `mbShellIntegrationEnv == nullptr` means leave it unset; "" is
// distinct (sets the var to empty, which the script treats as "all on").
ShellRun runZshUnderIntegration(const char *zsh,
                                const std::filesystem::path &assetDir,
                                const char *mbShellIntegrationEnv)
{
    namespace fs = std::filesystem;

    char tmpl[] = "/tmp/mb-shellint-XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    // Canonicalize: macOS /tmp is a symlink to /private/tmp, so the path
    // mkdtemp returns and the path zsh's $PWD resolves to differ. HOME
    // and PWD must agree byte-for-byte for the tilde-abbreviation in the
    // OSC 0 title check to land.
    ShellRun run;
    run.tmpHome = fs::canonical(tmpl).string();
    {
        FILE *f = fopen((run.tmpHome + "/.zshrc").c_str(), "w");
        REQUIRE(f != nullptr);
        fputs("PS1='[mb-test]%# '\n", f);
        fclose(f);
    }

    int masterFd = -1;
    pid_t pid    = forkpty(&masterFd, nullptr, nullptr, nullptr);
    REQUIRE(pid >= 0);

    if (pid == 0) {
        setenv("HOME", run.tmpHome.c_str(), 1);
        setenv("ZDOTDIR", assetDir.c_str(), 1);
        unsetenv("MB_ORIG_ZDOTDIR");
        if (mbShellIntegrationEnv) {
            setenv("MB_SHELL_INTEGRATION", mbShellIntegrationEnv, 1);
        } else {
            unsetenv("MB_SHELL_INTEGRATION");
        }
        setenv("TERM", "xterm-256color", 1);
        if (chdir(run.tmpHome.c_str()) != 0) {
            _exit(126);
        }

        // -i forces interactive so precmd_functions / preexec_functions
        // actually fire. -d skips global startup so /etc/zshenv quirks
        // (Apple's `path_helper`, etc.) don't change PWD/path mid-run.
        const char *argv[] = { zsh, "-i", "-d", nullptr };
        execv(zsh, const_cast<char *const *>(argv));
        _exit(127);
    }

    // Use \r (terminal Enter) not \n — zle in interactive mode keys off
    // CR, and \n alone gets buffered as literal input until the
    // bracketed-paste timeout elapses.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::string driver = "false\rexit\r";
    REQUIRE(write(masterFd, driver.data(), driver.size()) == static_cast<ssize_t>(driver.size()));

    run.output = drain(masterFd, std::chrono::milliseconds(3000));
    close(masterFd);

    int status = 0;
    waitpid(pid, &status, 0);
    fs::remove_all(run.tmpHome);
    return run;
}

} // namespace

TEST_CASE("shell-integration: zsh emits OSC 133 A/C/D, OSC 7 cwd, OSC 0 title")
{
    const char *zsh = findZsh();
    if (!zsh) {
        MESSAGE("zsh not installed, skipping");
        return;
    }

    namespace fs            = std::filesystem;
    const fs::path assetDir = fs::path(MB_TEST_SHELL_INTEGRATION_DIR) / "zsh";
    REQUIRE(fs::exists(assetDir / ".zshenv"));
    REQUIRE(fs::exists(assetDir / "mb-integration.zsh"));

    const ShellRun run    = runZshUnderIntegration(zsh, assetDir, nullptr);
    const std::string &out = run.output;

    // Assertions. OSC sequences may use BEL or ST; integration uses BEL.
    CHECK_MESSAGE(out.find("\x1b]133;A\x07") != std::string::npos,
                  "expected OSC 133;A in output: ", out);
    CHECK_MESSAGE(out.find("\x1b]133;C\x07") != std::string::npos,
                  "expected OSC 133;C in output: ", out);
    CHECK_MESSAGE(out.find("\x1b]133;D;1\x07") != std::string::npos,
                  "expected OSC 133;D;1 (false's exit code) in output: ", out);
    CHECK_MESSAGE(out.find("\x1b]7;file://") != std::string::npos,
                  "expected OSC 7 cwd in output: ", out);
    // The cwd path itself must reach the receiver intact. A previous bug
    // shadowed zsh's reserved `path` array with `local path=$PWD`, which
    // collapsed the for-loop to a single iteration and emitted just the
    // URL-encoded leading slash. Anchor on the temp HOME we set up so
    // we'd catch that regression — the basename is the unique mkdtemp suffix.
    const std::string mark = "/" + fs::path(run.tmpHome).filename().string();
    CHECK_MESSAGE(out.find(mark) != std::string::npos,
                  "expected OSC 7 path to contain ", mark, " in: ", out);
    // OSC 0 title is re-emitted every precmd so leaked titles from TUI
    // apps self-heal on the next prompt. Default format is the tilde-
    // abbreviated cwd; with HOME==tmpHome the first prompt emits "~".
    CHECK_MESSAGE(out.find("\x1b]0;~\x07") != std::string::npos,
                  "expected OSC 0 title (~) in output: ", out);
}

TEST_CASE("shell-integration: MB_SHELL_INTEGRATION opts out per feature")
{
    const char *zsh = findZsh();
    if (!zsh) {
        MESSAGE("zsh not installed, skipping");
        return;
    }

    namespace fs            = std::filesystem;
    const fs::path assetDir = fs::path(MB_TEST_SHELL_INTEGRATION_DIR) / "zsh";

    // mb sets MB_SHELL_INTEGRATION pre-fork from `shell_integration_features`
    // in TOML. Verify the script-side flag parsing actually suppresses
    // emission for each named feature, while leaving the un-named ones on.
    const ShellRun run    = runZshUnderIntegration(zsh, assetDir, "no-cwd no-title");
    const std::string &out = run.output;

    // prompt-mark stays on (not opted out).
    CHECK_MESSAGE(out.find("\x1b]133;A\x07") != std::string::npos,
                  "expected OSC 133;A still emitted: ", out);
    CHECK_MESSAGE(out.find("\x1b]133;D;1\x07") != std::string::npos,
                  "expected OSC 133;D;1 still emitted: ", out);
    // cwd + title suppressed.
    CHECK_MESSAGE(out.find("\x1b]7;") == std::string::npos,
                  "expected NO OSC 7 cwd in output: ", out);
    CHECK_MESSAGE(out.find("\x1b]0;") == std::string::npos,
                  "expected NO OSC 0 title in output: ", out);
}
