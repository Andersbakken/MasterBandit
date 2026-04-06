#include "ScriptFsModule.h"
#include "ScriptEngine.h"
#include "ScriptPermissions.h"

#include <quickjs.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace Script;

// ============================================================================
// Helpers
// ============================================================================

static Engine* fsEngFromCtx(JSContext* ctx)
{
    return static_cast<Engine*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

// Returns true if `path` (which may not exist) is under `allowedDir`.
// For existing paths, uses canonical resolution to prevent symlink escapes.
// For non-existing paths, canonicalises the parent and appends the filename.
static bool isUnderDir(const fs::path& path, const fs::path& allowedDir)
{
    std::error_code ec;
    auto canonicalAllowed = fs::canonical(allowedDir, ec);
    if (ec) return false;

    fs::path resolved;
    if (fs::exists(path, ec) && !ec) {
        resolved = fs::canonical(path, ec);
        if (ec) return false;
    } else {
        ec.clear();
        auto parent = fs::canonical(path.parent_path(), ec);
        if (ec) return false;
        resolved = parent / path.filename();
    }

    auto rel = resolved.lexically_relative(canonicalAllowed);
    return !rel.empty() && !rel.string().starts_with("..");
}

// Derive the per-script config directory: <configDir>/<stem>/
static fs::path scriptConfigDir(const Engine* eng, const Engine::Instance* inst)
{
    std::string stem = fs::path(inst->path).stem().string();
    return fs::path(eng->configDir()) / stem;
}

// Ensure the config dir exists, return false on failure.
static bool ensureConfigDir(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        spdlog::error("mb:fs: failed to create config dir '{}': {}", dir.string(), ec.message());
        return false;
    }
    return true;
}

// Check that `path` is readable by `inst`. Returns canonical path, or empty on failure.
// For built-ins: any path allowed.
// For user scripts: must be under script dir or config dir.
static std::string checkReadPath(JSContext* ctx, Engine* eng,
                                  Engine::Instance* inst, const std::string& raw)
{
    if (!(inst->permissions & Perm::FsRead)) {
        JS_ThrowTypeError(ctx, "mb:fs: fs.read permission required");
        return {};
    }

    if (inst->builtIn) {
        // Trusted — just resolve (file must exist for reads)
        std::error_code ec;
        if (!fs::exists(raw, ec) || ec) {
            JS_ThrowTypeError(ctx, "mb:fs: '%s' not found", raw.c_str());
            return {};
        }
        auto canon = fs::canonical(raw, ec);
        if (ec) {
            JS_ThrowTypeError(ctx, "mb:fs: cannot resolve '%s'", raw.c_str());
            return {};
        }
        return canon.string();
    }

    fs::path p(raw);
    fs::path scriptDir = fs::path(inst->path).parent_path();
    fs::path cfgDir    = scriptConfigDir(eng, inst);

    if (!isUnderDir(p, scriptDir) && !isUnderDir(p, cfgDir)) {
        JS_ThrowTypeError(ctx, "mb:fs: '%s' is outside allowed read directories", raw.c_str());
        return {};
    }

    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
        JS_ThrowTypeError(ctx, "mb:fs: '%s' not found", raw.c_str());
        return {};
    }
    auto canon = fs::canonical(p, ec);
    if (ec) {
        JS_ThrowTypeError(ctx, "mb:fs: cannot resolve '%s'", raw.c_str());
        return {};
    }
    return canon.string();
}

// Check that `path` is writable by `inst`. Returns the validated path string.
// For built-ins: any path allowed.
// For user scripts: must be under config dir only.
static std::string checkWritePath(JSContext* ctx, Engine* eng,
                                   Engine::Instance* inst, const std::string& raw)
{
    if (!(inst->permissions & Perm::FsWrite)) {
        JS_ThrowTypeError(ctx, "mb:fs: fs.write permission required");
        return {};
    }

    if (inst->builtIn)
        return raw; // unrestricted

    fs::path p(raw);
    fs::path cfgDir = scriptConfigDir(eng, inst);

    if (!isUnderDir(p, cfgDir)) {
        JS_ThrowTypeError(ctx, "mb:fs: '%s' is outside allowed write directory", raw.c_str());
        return {};
    }

    return raw;
}

// ============================================================================
// JS function implementations
// ============================================================================

static JSValue js_fs_readFileSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mb:fs: readFileSync requires a path argument");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string rawPath(raw);
    JS_FreeCString(ctx, raw);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    std::string path = checkReadPath(ctx, eng, inst, rawPath);
    if (path.empty()) return JS_EXCEPTION;

    std::ifstream f(path, std::ios::binary);
    if (!f) return JS_ThrowTypeError(ctx, "mb:fs: cannot open '%s'", path.c_str());

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    return JS_NewStringLen(ctx, content.data(), content.size());
}

static JSValue js_fs_writeFileSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "mb:fs: writeFileSync requires path and data arguments");

    const char* rawPath = JS_ToCString(ctx, argv[0]);
    if (!rawPath) return JS_EXCEPTION;
    std::string pathStr(rawPath);
    JS_FreeCString(ctx, rawPath);

    size_t dataLen = 0;
    const char* rawData = JS_ToCStringLen(ctx, &dataLen, argv[1]);
    if (!rawData) return JS_EXCEPTION;
    std::string data(rawData, dataLen);
    JS_FreeCString(ctx, rawData);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    std::string validatedPath = checkWritePath(ctx, eng, inst, pathStr);
    if (validatedPath.empty()) return JS_EXCEPTION;

    // Auto-create config dir (or parent for built-ins)
    fs::path parent = fs::path(validatedPath).parent_path();
    if (!ensureConfigDir(parent)) return JS_ThrowTypeError(ctx, "mb:fs: cannot create directory for '%s'", validatedPath.c_str());

    std::ofstream f(validatedPath, std::ios::binary | std::ios::trunc);
    if (!f) return JS_ThrowTypeError(ctx, "mb:fs: cannot write '%s'", validatedPath.c_str());

    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return JS_UNDEFINED;
}

static JSValue js_fs_readdirSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mb:fs: readdirSync requires a path argument");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string rawPath(raw);
    JS_FreeCString(ctx, raw);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    std::string path = checkReadPath(ctx, eng, inst, rawPath);
    if (path.empty()) return JS_EXCEPTION;

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) break;
        std::string name = entry.path().filename().string();
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, name.c_str()));
    }
    return arr;
}

static JSValue js_fs_statSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mb:fs: statSync requires a path argument");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string rawPath(raw);
    JS_FreeCString(ctx, raw);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    std::string path = checkReadPath(ctx, eng, inst, rawPath);
    if (path.empty()) return JS_EXCEPTION;

    std::error_code ec;
    auto st = fs::status(path, ec);
    if (ec) return JS_ThrowTypeError(ctx, "mb:fs: cannot stat '%s'", path.c_str());

    auto sz = fs::file_size(path, ec);
    uint64_t size = ec ? 0 : static_cast<uint64_t>(sz);
    ec.clear();

    auto lwt = fs::last_write_time(path, ec);
    double mtime = 0.0;
    if (!ec) {
        // Convert to milliseconds since epoch
        auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::file_clock::to_sys(lwt));
        mtime = static_cast<double>(sctp.time_since_epoch().count());
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size",        JS_NewInt64(ctx, static_cast<int64_t>(size)));
    JS_SetPropertyStr(ctx, obj, "isFile",      JS_NewBool(ctx, fs::is_regular_file(st)));
    JS_SetPropertyStr(ctx, obj, "isDirectory", JS_NewBool(ctx, fs::is_directory(st)));
    JS_SetPropertyStr(ctx, obj, "mtime",       JS_NewFloat64(ctx, mtime));
    return obj;
}

static JSValue js_fs_existsSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mb:fs: existsSync requires a path argument");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string rawPath(raw);
    JS_FreeCString(ctx, raw);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    if (!(inst->permissions & Perm::FsRead))
        return JS_ThrowTypeError(ctx, "mb:fs: fs.read permission required");

    fs::path p(rawPath);
    bool allowed = false;
    if (inst->builtIn) {
        allowed = true;
    } else {
        fs::path scriptDir = fs::path(inst->path).parent_path();
        fs::path cfgDir    = scriptConfigDir(eng, inst);
        allowed = isUnderDir(p, scriptDir) || isUnderDir(p, cfgDir);
    }

    if (!allowed)
        return JS_ThrowTypeError(ctx, "mb:fs: '%s' is outside allowed directories", rawPath.c_str());

    std::error_code ec;
    return JS_NewBool(ctx, fs::exists(p, ec) && !ec);
}

static JSValue js_fs_mkdirSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mb:fs: mkdirSync requires a path argument");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string rawPath(raw);
    JS_FreeCString(ctx, raw);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    std::string validatedPath = checkWritePath(ctx, eng, inst, rawPath);
    if (validatedPath.empty()) return JS_EXCEPTION;

    if (!ensureConfigDir(fs::path(validatedPath)))
        return JS_ThrowTypeError(ctx, "mb:fs: mkdirSync failed for '%s'", validatedPath.c_str());

    return JS_UNDEFINED;
}

static JSValue js_fs_unlinkSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "mb:fs: unlinkSync requires a path argument");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string rawPath(raw);
    JS_FreeCString(ctx, raw);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    std::string validatedPath = checkWritePath(ctx, eng, inst, rawPath);
    if (validatedPath.empty()) return JS_EXCEPTION;

    std::error_code ec;
    fs::remove(validatedPath, ec);
    if (ec) return JS_ThrowTypeError(ctx, "mb:fs: unlinkSync failed for '%s': %s",
                                      validatedPath.c_str(), ec.message().c_str());
    return JS_UNDEFINED;
}

static JSValue js_fs_renameSync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "mb:fs: renameSync requires oldPath and newPath arguments");

    const char* rawOld = JS_ToCString(ctx, argv[0]);
    if (!rawOld) return JS_EXCEPTION;
    std::string oldPath(rawOld);
    JS_FreeCString(ctx, rawOld);

    const char* rawNew = JS_ToCString(ctx, argv[1]);
    if (!rawNew) return JS_EXCEPTION;
    std::string newPath(rawNew);
    JS_FreeCString(ctx, rawNew);

    auto* eng  = fsEngFromCtx(ctx);
    auto* inst = eng->findInstanceByCtx(ctx);
    if (!inst) return JS_ThrowTypeError(ctx, "mb:fs: no script context");

    // Both paths must be writable
    std::string validatedOld = checkWritePath(ctx, eng, inst, oldPath);
    if (validatedOld.empty()) return JS_EXCEPTION;
    std::string validatedNew = checkWritePath(ctx, eng, inst, newPath);
    if (validatedNew.empty()) return JS_EXCEPTION;

    std::error_code ec;
    fs::rename(validatedOld, validatedNew, ec);
    if (ec) return JS_ThrowTypeError(ctx, "mb:fs: renameSync failed: %s", ec.message().c_str());
    return JS_UNDEFINED;
}

// ============================================================================
// Module registration
// ============================================================================

static int js_fs_module_init(JSContext* ctx, JSModuleDef* m)
{
    JS_SetModuleExport(ctx, m, "readFileSync",  JS_NewCFunction(ctx, js_fs_readFileSync,  "readFileSync",  1));
    JS_SetModuleExport(ctx, m, "writeFileSync", JS_NewCFunction(ctx, js_fs_writeFileSync, "writeFileSync", 2));
    JS_SetModuleExport(ctx, m, "readdirSync",   JS_NewCFunction(ctx, js_fs_readdirSync,   "readdirSync",   1));
    JS_SetModuleExport(ctx, m, "statSync",      JS_NewCFunction(ctx, js_fs_statSync,      "statSync",      1));
    JS_SetModuleExport(ctx, m, "existsSync",    JS_NewCFunction(ctx, js_fs_existsSync,    "existsSync",    1));
    JS_SetModuleExport(ctx, m, "mkdirSync",     JS_NewCFunction(ctx, js_fs_mkdirSync,     "mkdirSync",     1));
    JS_SetModuleExport(ctx, m, "unlinkSync",    JS_NewCFunction(ctx, js_fs_unlinkSync,    "unlinkSync",    1));
    JS_SetModuleExport(ctx, m, "renameSync",    JS_NewCFunction(ctx, js_fs_renameSync,    "renameSync",    2));
    return 0;
}

JSModuleDef* createFsNativeModule(JSContext* ctx, Script::Engine* /*eng*/)
{
    JSModuleDef* m = JS_NewCModule(ctx, "mb:fs", js_fs_module_init);
    if (!m) return nullptr;
    JS_AddModuleExport(ctx, m, "readFileSync");
    JS_AddModuleExport(ctx, m, "writeFileSync");
    JS_AddModuleExport(ctx, m, "readdirSync");
    JS_AddModuleExport(ctx, m, "statSync");
    JS_AddModuleExport(ctx, m, "existsSync");
    JS_AddModuleExport(ctx, m, "mkdirSync");
    JS_AddModuleExport(ctx, m, "unlinkSync");
    JS_AddModuleExport(ctx, m, "renameSync");
    return m;
}
