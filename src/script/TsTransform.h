#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace mb::tsx {

// Structured failure detail surfaced by transformTs / toJs / loadAsJs. The
// fields mirror whiteout_error so JS callers can construct a precise
// SyntaxError, and C++ callers can include offset/message in their logs.
struct TransformError
{
    // human-readable diagnostic from whiteout (e.g. "unsupported: namespace"
    // or "parse error near '<x>'"); empty if the failure was a read/open
    // problem rather than a whiteout problem.
    std::string message;
    // byte offset into the source where the failure was detected; only
    // meaningful when message is non-empty.
    std::size_t offset = 0;
};

// True iff the path's extension is ".ts" (case-sensitive — TypeScript itself
// is case-sensitive on the extension).
bool isTypeScriptPath(std::string_view path);

// Transforms TS source into byte-equal-length JS by erasing type syntax.
// Caches results on disk under $XDG_DATA_HOME/MasterBandit/ts-cache/, keyed by
// sha256 of the source bytes; cache hits skip the tree-sitter parse entirely.
// Logs on failure. If err is non-null, it is populated with whiteout's status
// detail so the caller can surface it (e.g. as a QuickJS exception).
std::optional<std::string> transformTs(std::string_view source,
                                       std::string_view pathForDiagnostics,
                                       TransformError *err = nullptr);

// Reads `path` from disk and, if it is a .ts file, returns the whiteout-stripped
// JS source (cached). For non-.ts files returns the raw bytes. Empty string on
// read failure or unrecoverable transform error (matching io::readFile's
// signal-via-empty convention). When `err` is non-null and the failure was a
// whiteout error (rather than a read error), `err->message` is set; on read
// failures `err->message` stays empty.
std::string loadAsJs(const std::string &path, TransformError *err = nullptr);

// As loadAsJs, but the caller has already read the raw bytes (e.g. because it
// also needs to hash them for the script allowlist). Avoids the second read.
// Returns empty on transform error; `err` semantics match loadAsJs.
std::string toJs(const std::string &path, std::string_view rawSource, TransformError *err = nullptr);

} // namespace mb::tsx
