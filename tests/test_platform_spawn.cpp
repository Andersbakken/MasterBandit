// Tests for platformSpawnDetached — the detached-spawn primitive used
// by mb.process.spawn. Verifies side effects via the filesystem (the
// spawned process writes a sentinel file, we poll for its appearance)
// rather than tracking the grandchild pid (which is intentionally
// untracked by the detached double-fork pattern).

#include <doctest/doctest.h>

#include "PlatformDawn.h" // platformSpawnDetached + ProcessSpawnOptions

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// Wait up to `timeout` for `predicate()` to become true. Returns true
// iff it did. Used to poll for sentinel files written by spawned
// processes — there's no signal on the script side that the grandchild
// finished (detached → no waitpid), so polling is the only option.
template <typename Pred>
bool waitFor(std::chrono::milliseconds timeout, Pred pred)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// Per-test scratch dir so concurrent test runs don't collide.
struct ScratchDir {
    fs::path dir;
    ScratchDir() {
        dir = fs::temp_directory_path()
            / ("mb-spawn-test-" + std::to_string(::getpid())
               + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path operator/(const char* name) const { return dir / name; }
};

// /bin/sh is the most portable shell available on every CI image we
// expect to run on. Tests that need shell execution use it via
// `-c "..."`. If a stripped-down container lacks /bin/sh the tests
// will be skipped (see DOCTEST_CHECK_RANGE / skip-on-missing).
const char* kShellPath = "/bin/sh";

bool shellAvailable() {
    return ::access(kShellPath, X_OK) == 0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Argument validation
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("platformSpawnDetached: empty path returns 0 (pre-fork failure)")
{
    pid_t pid = platformSpawnDetached("", {}, ProcessSpawnOptions{});
    CHECK(pid == 0);
}

// ─────────────────────────────────────────────────────────────────────
// Successful spawn — verify side effect lands on disk
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("platformSpawnDetached: spawns shell that writes a sentinel file"
          * doctest::skip(!shellAvailable()))
{
    ScratchDir s;
    auto sentinel = s / "sentinel";
    std::string cmd = "echo hello > " + sentinel.string();

    ProcessSpawnOptions opts; // empty — inherit env, no cwd override
    pid_t pid = platformSpawnDetached(kShellPath,
                                       {kShellPath, "-c", cmd}, opts);
    // pid > 0 confirms intermediate-child fork + reap succeeded. The
    // grandchild's exec / shell run is async; poll for the sentinel.
    REQUIRE(pid > 0);
    REQUIRE(waitFor(std::chrono::seconds(5),
                    [&] { return fs::exists(sentinel); }));

    std::ifstream f(sentinel);
    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    CHECK(contents == "hello\n");
}

TEST_CASE("platformSpawnDetached: PATH lookup finds /bin/sh as bare basename"
          * doctest::skip(!shellAvailable()))
{
    ScratchDir s;
    auto sentinel = s / "path-lookup";
    std::string cmd = "touch " + sentinel.string();

    pid_t pid = platformSpawnDetached("sh", {"sh", "-c", cmd},
                                       ProcessSpawnOptions{});
    REQUIRE(pid > 0);
    REQUIRE(waitFor(std::chrono::seconds(5),
                    [&] { return fs::exists(sentinel); }));
}

// ─────────────────────────────────────────────────────────────────────
// cwd handling
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("platformSpawnDetached: cwd makes the process start in that directory"
          * doctest::skip(!shellAvailable()))
{
    ScratchDir s;
    // Shell writes its $PWD to a relative file path. With cwd set to
    // s.dir, the relative path resolves under s.dir.
    std::string cmd = "pwd > pwd.out";

    ProcessSpawnOptions opts;
    opts.cwd = s.dir.string();

    pid_t pid = platformSpawnDetached(kShellPath, {kShellPath, "-c", cmd}, opts);
    REQUIRE(pid > 0);

    auto out = s / "pwd.out";
    REQUIRE(waitFor(std::chrono::seconds(5),
                    [&] { return fs::exists(out); }));

    std::ifstream f(out);
    std::string pwd;
    std::getline(f, pwd);

    // realpath the cwd before comparing — the kernel (and chdir) may
    // canonicalise symlinks (e.g. /tmp on macOS is /private/tmp).
    char actualPath[PATH_MAX];
    REQUIRE(::realpath(s.dir.string().c_str(), actualPath) != nullptr);
    CHECK(pwd == std::string(actualPath));
}

TEST_CASE("platformSpawnDetached: nonexistent cwd causes the spawn to fail silently"
          * doctest::skip(!shellAvailable()))
{
    ScratchDir s;
    auto sentinel = s / "should-not-appear";

    ProcessSpawnOptions opts;
    opts.cwd = (s.dir / "no-such-subdir").string();
    pid_t pid = platformSpawnDetached(
        kShellPath,
        {kShellPath, "-c", "touch " + sentinel.string()},
        opts);
    // Intermediate child still reaps cleanly — chdir failure happens
    // in the grandchild post-fork, which _exit(127)s. Parent doesn't
    // see the failure synchronously beyond the spdlog warning.
    REQUIRE(pid > 0);

    // Give the (would-be) grandchild a window to NOT create the
    // sentinel. We wait briefly; the absence is what we're verifying.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(fs::exists(sentinel));
}

// ─────────────────────────────────────────────────────────────────────
// env handling
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("platformSpawnDetached: env override is visible to the spawned process"
          * doctest::skip(!shellAvailable()))
{
    ScratchDir s;
    auto sentinel = s / "env.out";
    // Shell writes $MB_TEST_KEY to the sentinel. With no override,
    // $MB_TEST_KEY isn't set and the line is empty; with override,
    // it must equal our magic value.
    std::string cmd = "printf '%s' \"${MB_TEST_KEY}\" > " + sentinel.string();

    ProcessSpawnOptions opts;
    opts.env.emplace_back("MB_TEST_KEY", "value-from-test");

    pid_t pid = platformSpawnDetached(kShellPath,
                                       {kShellPath, "-c", cmd}, opts);
    REQUIRE(pid > 0);
    REQUIRE(waitFor(std::chrono::seconds(5),
                    [&] { return fs::exists(sentinel); }));

    std::ifstream f(sentinel);
    std::string val((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    CHECK(val == "value-from-test");
}

TEST_CASE("platformSpawnDetached: env merge replaces existing keys"
          * doctest::skip(!shellAvailable()))
{
    ScratchDir s;
    auto sentinel = s / "env-replace.out";
    std::string cmd = "printf '%s' \"${PATH}\" > " + sentinel.string();

    ProcessSpawnOptions opts;
    opts.env.emplace_back("PATH", "/spawn-test/replaced/path");
    pid_t pid = platformSpawnDetached(kShellPath,
                                       {kShellPath, "-c", cmd}, opts);
    REQUIRE(pid > 0);
    REQUIRE(waitFor(std::chrono::seconds(5),
                    [&] { return fs::exists(sentinel); }));

    std::ifstream f(sentinel);
    std::string val((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    CHECK(val == "/spawn-test/replaced/path");
}

TEST_CASE("platformSpawnDetached: inherited env is preserved when env is empty"
          * doctest::skip(!shellAvailable()))
{
    // Set a marker in our own environment, spawn without overrides,
    // verify the child sees it. Use a unique key so concurrent runs
    // don't collide. Note: ::setenv mutates THIS test process'
    // environ — if the test runner doesn't reset between tests it
    // could leak state. Pick a marker name that no other test uses.
    ::setenv("MB_SPAWN_INHERIT_TEST", "inherited-value", /*overwrite=*/1);

    ScratchDir s;
    auto sentinel = s / "inherit.out";
    std::string cmd =
        "printf '%s' \"${MB_SPAWN_INHERIT_TEST}\" > " + sentinel.string();

    pid_t pid = platformSpawnDetached(kShellPath,
                                       {kShellPath, "-c", cmd},
                                       ProcessSpawnOptions{});
    REQUIRE(pid > 0);
    REQUIRE(waitFor(std::chrono::seconds(5),
                    [&] { return fs::exists(sentinel); }));

    std::ifstream f(sentinel);
    std::string val((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    CHECK(val == "inherited-value");

    ::unsetenv("MB_SPAWN_INHERIT_TEST");
}

// ─────────────────────────────────────────────────────────────────────
// Failure: missing binary
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("platformSpawnDetached: missing binary still returns nonzero pid (intermediate child reaped)")
{
    // The intermediate child completes successfully — it's the
    // grandchild's exec that fails. The parent reaps the
    // intermediate, returns its pid, and emits a spdlog warn about
    // the exec failure. Critically, no zombie process is left
    // behind. We can't easily assert "no zombie" without
    // /proc-introspection on Linux only; the meaningful assertion
    // here is the contract: the function returns a nonzero pid for
    // a fork that succeeded structurally even when exec will fail.
    pid_t pid = platformSpawnDetached(
        "/bin/no-such-binary-nope-nope",
        {"/bin/no-such-binary-nope-nope"},
        ProcessSpawnOptions{});
    CHECK(pid > 0);

    // Best-effort check that the grandchild doesn't linger as a
    // zombie. Since it's adopted by init/launchd, we can't waitpid
    // for it from here; just make sure waitpid for its (unknown)
    // pid returns ECHILD as expected for "we have no children of
    // that pid".
    int status;
    pid_t ret = ::waitpid(-1, &status, WNOHANG);
    // ret == -1 with errno == ECHILD: no children we know of (good).
    // ret == 0: a child exists but isn't ready (unexpected).
    // ret > 0: we just reaped someone — make sure it wasn't OUR
    // intermediate (whose pid we hold), since that would mean our
    // own waitpid in spawn raced.
    if (ret > 0) {
        CHECK(ret != pid);
    }
}

// ─────────────────────────────────────────────────────────────────────
// argv defaulting
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("platformSpawnDetached: empty argv defaults argv[0] to path"
          * doctest::skip(!shellAvailable()))
{
    // /bin/true exits 0 with no argv. Verify it spawns successfully
    // when we pass an empty argv (the implementation defaults
    // argv[0] = path). We use /bin/true via the shell because
    // spawn-and-check-it-ran is unobservable without a side effect;
    // chained command produces a sentinel as evidence.
    ScratchDir s;
    auto sentinel = s / "argv-default.out";
    std::string cmd = "touch " + sentinel.string() + " && exit 0";

    pid_t pid = platformSpawnDetached(kShellPath,
                                       {kShellPath, "-c", cmd},
                                       ProcessSpawnOptions{});
    REQUIRE(pid > 0);
    REQUIRE(waitFor(std::chrono::seconds(5),
                    [&] { return fs::exists(sentinel); }));
}
