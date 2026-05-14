#include "TsTransform.h"

#include "Sha256.h"
#include "Utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <spdlog/spdlog.h>
#include <whiteout/whiteout.h>

namespace mb::tsx {

namespace fs = std::filesystem;

bool isTypeScriptPath(std::string_view path)
{
    return path.size() >= 3 && path.substr(path.size() - 3) == ".ts";
}

static fs::path cacheDir()
{
    // XDG_DATA_HOME with the standard ~/.local/share fallback. Cache scoped
    // under MasterBandit/ts-cache so other tools can't collide.
    const char *xdgData = std::getenv("XDG_DATA_HOME");
    fs::path base;
    if (xdgData && xdgData[0]) {
        base = xdgData;
    } else {
        const char *home = std::getenv("HOME");
        if (!home || !home[0]) {
            return {};
        }
        base = fs::path(home) / ".local" / "share";
    }
    return base / "MasterBandit" / "ts-cache";
}

// Pulls a per-thread whiteout_ctx so concurrent transforms don't contend.
// A single ctx isn't thread-safe; thread_local keeps each thread isolated and
// re-uses the parser across calls on the same thread.
static whiteout_ctx *threadCtx()
{
    thread_local struct CtxOwner
    {
        whiteout_ctx *ctx = whiteout_ctx_new();

        ~CtxOwner()
        {
            if (ctx) {
                whiteout_ctx_free(ctx);
            }
        }
    } owner;

    return owner.ctx;
}

bool transformTsInPlace(std::string &buf,
                        std::string_view pathForDiagnostics,
                        TransformError *errOut)
{
    whiteout_ctx *ctx = threadCtx();
    if (!ctx) {
        spdlog::error("TsTransform: failed to allocate whiteout context for '{}'", pathForDiagnostics);
        if (errOut) {
            errOut->message = "failed to allocate whiteout context";
            errOut->offset  = 0;
        }
        return false;
    }

    whiteout_error err {};
    whiteout_status st = whiteout_transform_inplace(ctx, buf.data(), buf.size(), &err);
    if (st != WHITEOUT_OK) {
        // err.message is owned by ctx — copy before we hand a pointer to the
        // caller, since ctx may be reused (or destroyed at thread exit) before
        // the message is consumed.
        std::string msg = err.message ? err.message : "(no message)";
        spdlog::error("TsTransform: '{}' whiteout error (status={}, offset={}): {}",
                      pathForDiagnostics,
                      static_cast<int>(st),
                      err.offset,
                      msg);
        if (errOut) {
            errOut->message = std::move(msg);
            errOut->offset  = err.offset;
        }
        // whiteout_transform_inplace guarantees buf is untouched on non-OK.
        return false;
    }
    return true;
}

// Per-source cache directory: sha256(WHITEOUT_VERSION || absPath). One dir per
// (path, version) pair, so editing a file overwrites its prior entry rather
// than orphaning a content-addressed file each time. A version bump produces
// a different dir, so old layouts coexist harmlessly until they're swept.
//
// Path is canonicalized first so `./foo.ts` and `/abs/foo.ts` resolve to the
// same key. weakly_canonical works on missing files (we may be hashing in
// advance of a write) and returns the absolute form unchanged otherwise.
static fs::path cacheEntryDirFor(std::string_view path)
{
    auto root = cacheDir();
    if (root.empty()) {
        return {};
    }
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(fs::path(path), ec), ec);
    if (ec || abs.empty()) {
        abs = fs::path(path);
    }
    std::string absStr = abs.string();

    crypto::Sha256 hasher;
    int version = WHITEOUT_VERSION;
    hasher.update(&version, sizeof version);
    hasher.update("\0", 1);
    hasher.update(absStr);
    return root / hasher.finalizeHex();
}

static std::optional<std::string> readFile(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        return std::nullopt;
    }
    auto size = f.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    f.seekg(0);
    std::string data(static_cast<size_t>(size), '\0');
    f.read(data.data(), size);
    return data;
}

// Atomic single-file write: temp sibling + rename. Concurrent readers either
// see the prior file's bytes or the new file's bytes, never a torn write.
static bool atomicWrite(const fs::path &finalPath, std::string_view bytes)
{
    fs::path tmp = finalPath;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::warn("TsTransform: cache write '{}' open failed", tmp.string());
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            spdlog::warn("TsTransform: cache write '{}' failed", tmp.string());
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, finalPath, ec);
    if (ec) {
        spdlog::warn("TsTransform: cache rename '{}' -> '{}' failed: {}",
                     tmp.string(),
                     finalPath.string(),
                     ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Write index.js (the transformed JS) then hash.txt (the raw-source sha256).
// Ordering matters for crash recovery: index.js lands first, hash.txt is the
// commit marker. If we crash between the two, the on-disk hash.txt stays at
// its prior value (or stays absent), the next load's hash compare fails, we
// re-transform — no stale-data risk. Per-process mutex prevents two threads
// in the same process from racing on the same entry; cross-process races
// settle to whoever's rename wins last (output is deterministic, so the
// result is identical either way).
static void writeCacheEntry(const fs::path &dir, std::string_view contentHash, std::string_view js)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        spdlog::warn("TsTransform: cache dir '{}' create failed: {}", dir.string(), ec.message());
        return;
    }

    static std::mutex sWriteMutex;
    std::lock_guard<std::mutex> lock(sWriteMutex);

    if (!atomicWrite(dir / "index.js", js)) {
        return;
    }
    atomicWrite(dir / "hash.txt", contentHash);
}

// Shared transform-with-cache step. `buf` enters holding the raw TS source
// (owned, mutable) and leaves holding either the cached JS (cache hit) or the
// freshly stripped JS (cache miss, mutation in place). On whiteout failure
// returns false and leaves buf as the raw source.
static bool transformAndCache(std::string &buf, std::string_view path, TransformError *errOut)
{
    fs::path entryDir       = cacheEntryDirFor(path);
    std::string contentHash = crypto::sha256Hex(buf);

    if (!entryDir.empty()) {
        if (auto storedHash = readFile(entryDir / "hash.txt")) {
            // Compare hex digests — these are written as exactly 64 chars with
            // no newline (see writeCacheEntry / Sha256::finalizeHex), so a
            // straight equality check is correct.
            if (*storedHash == contentHash) {
                if (auto cached = readFile(entryDir / "index.js")) {
                    buf = std::move(*cached);
                    return true;
                }
            }
        }
    }

    if (!transformTsInPlace(buf, path, errOut)) {
        return false;
    }
    if (!entryDir.empty()) {
        writeCacheEntry(entryDir, contentHash, buf);
    }
    return true;
}

std::string toJs(const std::string &path, std::string_view rawSource, TransformError *errOut)
{
    if (!isTypeScriptPath(path)) {
        return std::string(rawSource);
    }
    std::string buf(rawSource); // single owning copy; becomes the returned value
    if (!transformAndCache(buf, path, errOut)) {
        return {};
    }
    return buf;
}

std::string loadAsJs(const std::string &path, TransformError *errOut)
{
    std::string src = io::readFile(path);
    if (src.empty()) {
        return {};
    }
    if (!isTypeScriptPath(path)) {
        return src;
    }
    // We already own `src`; transform it in place rather than routing through
    // toJs (which would force an extra copy of the raw bytes).
    if (!transformAndCache(src, path, errOut)) {
        return {};
    }
    return src;
}

} // namespace mb::tsx
