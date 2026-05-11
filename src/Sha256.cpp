#include "Sha256.h"

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
using BackendCtx = CC_SHA256_CTX;
#else
#include <openssl/sha.h>
using BackendCtx = SHA256_CTX;
#endif

namespace crypto {

// Keep Sha256::state_ in sync with whichever backend's context is larger.
// If a future SDK update grows BackendCtx, this catches it at compile time
// instead of silently corrupting memory.
static_assert(sizeof(BackendCtx) <= 112,
              "Sha256::state_ too small for backend SHA-256 context");
static_assert(alignof(BackendCtx) <= 8,
              "Sha256::state_ alignment too weak for backend context");

// state_ holds the platform context; reinterpret rather than aliasing so we
// don't drag the backend header into Sha256.h.
static inline BackendCtx *ctx(unsigned char *state)
{
    return reinterpret_cast<BackendCtx *>(state);
}

Sha256::Sha256()
{
    reset();
}

void Sha256::reset()
{
#ifdef __APPLE__
    CC_SHA256_Init(ctx(state_));
#else
    SHA256_Init(ctx(state_));
#endif
}

Sha256 &Sha256::update(const void *data, std::size_t len)
{
    if (len == 0) {
        return *this;
    }
#ifdef __APPLE__
    CC_SHA256_Update(ctx(state_), data, static_cast<CC_LONG>(len));
#else
    SHA256_Update(ctx(state_), data, len);
#endif
    return *this;
}

std::string Sha256::finalizeHex()
{
    unsigned char digest[32];
#ifdef __APPLE__
    CC_SHA256_Final(digest, ctx(state_));
#else
    SHA256_Final(digest, ctx(state_));
#endif

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 32; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 0x0F];
    }
    return result;
}

std::string sha256Hex(std::string_view content)
{
    Sha256 h;
    h.update(content);
    return h.finalizeHex();
}

} // namespace crypto
