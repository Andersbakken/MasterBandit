#include "TsTransform.h"

#include "ScriptPermissions.h"
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

static fs::path cacheFileFor(std::string_view sourceHash)
{
    auto dir = cacheDir();
    if (dir.empty()) {
        return {};
    }
    // Hex sha256 is fixed-width, safe to embed directly. .js extension keeps
    // it obvious in tooling (and matches what we'd produce by stripping).
    return dir / (std::string(sourceHash) + ".js");
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

static void writeCache(const fs::path &p, std::string_view js)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
        spdlog::warn("TsTransform: cache dir '{}' create failed: {}", p.parent_path().string(), ec.message());
        return;
    }

    // Atomic write: temp file in the same dir + rename, so a concurrent reader
    // never sees a half-written cache entry. Per-cache-file mutex avoids two
    // threads racing on the same hash key in this process; cross-process races
    // are still safe because rename() is atomic on POSIX.
    static std::mutex sWriteMutex;
    std::lock_guard<std::mutex> lock(sWriteMutex);

    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::warn("TsTransform: cache write '{}' open failed", tmp.string());
            return;
        }
        out.write(js.data(), static_cast<std::streamsize>(js.size()));
        if (!out) {
            spdlog::warn("TsTransform: cache write '{}' failed", tmp.string());
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        spdlog::warn("TsTransform: cache rename '{}' -> '{}' failed: {}",
                     tmp.string(),
                     p.string(),
                     ec.message());
        fs::remove(tmp, ec);
    }
}

// Shared transform-with-cache step. `buf` enters holding the raw TS source
// (owned, mutable) and leaves holding either the cached JS (cache hit) or the
// freshly stripped JS (cache miss, mutation in place). On whiteout failure
// returns false and leaves buf as the raw source.
//
// Content-addressed cache: keyed by sha256 of the source bytes. Whiteout
// output is a pure function of input, so a matching hash means a valid cached
// translation regardless of mtime games or path renames.
static bool transformAndCache(std::string &buf, std::string_view path, TransformError *errOut)
{
    std::string hash   = Script::sha256Hex(buf);
    fs::path cachePath = cacheFileFor(hash);
    if (!cachePath.empty()) {
        if (auto cached = readFile(cachePath)) {
            buf = std::move(*cached);
            return true;
        }
    }
    if (!transformTsInPlace(buf, path, errOut)) {
        return false;
    }
    if (!cachePath.empty()) {
        writeCache(cachePath, buf);
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
