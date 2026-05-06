// Detached process spawn — extracted from PlatformUtils_{Linux.cpp,macOS.mm}
// so it can be linked independently into mb-tests without dragging in the
// notification / DBus / Cocoa machinery the rest of those TUs depend on.
//
// One TU, two backends behind #ifdef. Both backends share the same
// double-fork + setsid + reopen-stdio pattern; the differences are:
//   - execvpe (Linux glibc) vs `environ = envp; execvp` (macOS, no execvpe).
//   - pipe2(O_CLOEXEC) (Linux) vs pipe + fcntl(F_SETFD, FD_CLOEXEC) (macOS).
//
// Header dependency: "PlatformDawn.h" is needed only for the
// `ProcessSpawnOptions` struct + the `platformSpawnDetached` declaration.
// It pulls in a fair amount of windowing-toolkit infrastructure
// transitively, but that's header-only includes — no link-time deps land
// here beyond libc and spdlog.

#include "PlatformDawn.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ; // POSIX: process environment for default inheritance.
                       // On macOS this is fine for executables (this TU is
                       // linked directly into the executable). Frameworks
                       // / dylibs would need _NSGetEnviron — n/a here.

// Build the merged envp for a spawn: start from `environ`, replace any
// keys present in `overrides`, append new keys at the end. Returns a
// vector of NUL-terminated owning strings plus a parallel char* vector
// terminated with nullptr (the layout execve(2) wants). Both must
// outlive the execve call. Async-signal-safety: this function runs
// PRE-fork, so it can use std::vector / std::string freely; the result
// is then handed to the grandchild which only does pointer-arithmetic
// reads.
namespace platform_spawn_detail {
struct MergedEnv {
    std::vector<std::string> storage; // owning "K=V" strings
    std::vector<char*>       envp;    // points into storage; nullptr-terminated
};
static MergedEnv buildEnv(const std::vector<std::pair<std::string, std::string>>& overrides)
{
    MergedEnv out;
    if (overrides.empty()) {
        // Inherit verbatim: copy `environ` pointer-by-pointer. The
        // child execve(2)s with this vector. Strings live in the
        // parent's environ until exec replaces the address space —
        // which happens in the grandchild before any of these
        // pointers could be invalidated.
        for (char** p = environ; *p; ++p)
            out.envp.push_back(*p);
        out.envp.push_back(nullptr);
        return out;
    }
    // Index existing env by key for fast replace.
    std::unordered_map<std::string, size_t> keyToIdx;
    for (char** p = environ; *p; ++p) {
        std::string entry = *p;
        auto eq = entry.find('=');
        std::string key = (eq == std::string::npos) ? entry : entry.substr(0, eq);
        keyToIdx[key] = out.storage.size();
        out.storage.push_back(std::move(entry));
    }
    for (const auto& kv : overrides) {
        std::string entry = kv.first + "=" + kv.second;
        auto it = keyToIdx.find(kv.first);
        if (it != keyToIdx.end()) {
            out.storage[it->second] = std::move(entry);
        } else {
            keyToIdx[kv.first] = out.storage.size();
            out.storage.push_back(std::move(entry));
        }
    }
    out.envp.reserve(out.storage.size() + 1);
    for (auto& s : out.storage) out.envp.push_back(s.data());
    out.envp.push_back(nullptr);
    return out;
}
} // namespace platform_spawn_detail

pid_t platformSpawnDetached(const std::string& path,
                            const std::vector<std::string>& argv,
                            const ProcessSpawnOptions& opts)
{
    if (path.empty()) {
        spdlog::warn("platformSpawnDetached: empty path");
        return 0;
    }

    // Build argv as a char* vector. Strings owned by `argv` outlive
    // this function (the parent waits for the intermediate child
    // before returning), so .data() pointers are stable.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    // If argv is empty, default argv[0] to the binary path so execvp
    // sees a non-empty argv[0]. Saves callers from boilerplate.
    if (cargv.size() == 1) {
        cargv.insert(cargv.begin(), const_cast<char*>(path.c_str()));
    }

    platform_spawn_detail::MergedEnv env =
        platform_spawn_detail::buildEnv(opts.env);

    // Errno-reporting pipe. Both ends O_CLOEXEC so a successful exec
    // closes the write end automatically; the parent's read returns
    // EOF and we know exec succeeded. If exec fails (or the inner
    // fork or chdir fails), the failing process writes the errno to
    // the pipe before _exit, and the parent's read picks it up.
    // Pattern is well-known; see e.g. APUE §10.18.
    int errPipe[2];
#ifdef __linux__
    if (pipe2(errPipe, O_CLOEXEC) < 0) {
        spdlog::warn("platformSpawnDetached({}): pipe2: {}", path, strerror(errno));
        return 0;
    }
#else
    // macOS lacks pipe2(2); fall back to pipe(2) + fcntl(F_SETFD,
    // FD_CLOEXEC) on each end. There's a pid_t-sized window between
    // pipe() and fcntl() but it's single-threaded here (no other
    // thread inherits these fds).
    if (pipe(errPipe) < 0) {
        spdlog::warn("platformSpawnDetached({}): pipe: {}", path, strerror(errno));
        return 0;
    }
    fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);
#endif

    pid_t pid = fork();
    if (pid < 0) {
        close(errPipe[0]);
        close(errPipe[1]);
        spdlog::warn("platformSpawnDetached({}): fork: {}", path, strerror(errno));
        return 0;
    }
    if (pid == 0) {
        // Intermediate child: only the grandchild needs the write end.
        // Drop the read end so we can't accidentally read our own errno.
        close(errPipe[0]);
        pid_t inner = fork();
        if (inner == 0) {
            // Grandchild. setsid detaches from our process group so
            // the launched program isn't killed when mb's controlling
            // terminal goes away (and so it can't steal our tty).
            setsid();

            // Reopen stdio to /dev/null. The grandchild has no PTY
            // (we're spawning detached), and any inherited descriptors
            // pointing at mb's terminal would let the child draw
            // garbage on top of our output. Stdin → /dev/null too so
            // `vim` etc. can't accidentally suck input from us.
            int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
            if (devnull >= 0) {
                // dup2 clears CLOEXEC on the new fd, which we want —
                // the child's stdio must survive exec.
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }

            // chdir before exec if requested. Errors here surface as
            // a failed spawn (errno propagates via the pipe path
            // below).
            if (!opts.cwd.empty()) {
                if (chdir(opts.cwd.c_str()) < 0) {
                    int err = errno;
                    ssize_t n = write(errPipe[1], &err, sizeof(err));
                    (void)n;
                    _exit(127);
                }
            }

#ifdef __linux__
            execvpe(path.c_str(), cargv.data(), env.envp.data());
#else
            // macOS lacks execvpe(3); replace `environ` and use
            // execvp. We're in the grandchild, single-threaded, about
            // to either exec (replacing the address space anyway) or
            // _exit, so mutating the global is safe.
            environ = env.envp.data();
            execvp(path.c_str(), cargv.data());
#endif
            int err = errno;
            ssize_t n = write(errPipe[1], &err, sizeof(err));
            (void)n;
            _exit(127);
        }
        if (inner < 0) {
            int err = errno;
            ssize_t n = write(errPipe[1], &err, sizeof(err));
            (void)n;
        }
        close(errPipe[1]);
        _exit(0);
    }

    // Parent: close write end so EOF is detectable on read once the
    // grandchild's copy goes away (either via exec or _exit).
    close(errPipe[1]);
    int status;
    waitpid(pid, &status, 0);

    int err = 0;
    ssize_t n;
    do {
        n = read(errPipe[0], &err, sizeof(err));
    } while (n < 0 && errno == EINTR);
    close(errPipe[0]);

    if (n == static_cast<ssize_t>(sizeof(err))) {
        spdlog::warn("platformSpawnDetached({}): execvp/chdir failed: {}",
                     path, strerror(err));
        // We still return pid; the intermediate child reaped fine.
        // The grandchild that failed exec has already _exit(127)'d
        // and been adopted by init for reaping — we never see it.
    }
    // n == 0  → exec succeeded (write end closed by O_CLOEXEC); no log.
    // n == -1 → read error (rare, e.g. EBADF if pipe was closed twice);
    //          treat as "unknown outcome" and stay silent.
    return pid;
}
