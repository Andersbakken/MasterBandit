#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace crypto {

// Streaming SHA-256. Feed bytes with update(); call finalizeHex() to read
// the 64-character lowercase hex digest. After finalize, the instance is
// drained — call reset() to reuse the same allocation for another digest.
//
// Backend is CommonCrypto on Apple, OpenSSL elsewhere — the choice matches
// the existing one-shot. The platform context lives inline (sized to fit
// either backend's struct), so no heap allocation.
class Sha256
{
public:
    Sha256();

    Sha256(const Sha256 &)            = delete;
    Sha256 &operator=(const Sha256 &) = delete;

    // Append `len` bytes. Returns *this for chaining.
    Sha256 &update(const void *data, std::size_t len);
    Sha256 &update(std::string_view s) { return update(s.data(), s.size()); }

    // Consume the accumulated state and return the 64-char lowercase hex
    // digest. Don't call twice without an intervening reset() — backend
    // behaviour after Final is undefined.
    std::string finalizeHex();

    // Re-initialize for another digest; cheaper than re-constructing.
    void reset();

private:
    // Inline storage sized to hold the largest backend context (OpenSSL's
    // 112-byte SHA256_CTX exceeds Apple's CC_SHA256_CTX). The .cpp file
    // static_asserts the actual sizeof to catch a future struct change.
    alignas(8) unsigned char state_[112];
};

// One-shot convenience for callers that already have all the bytes — same
// digest as Sha256{}.update(content).finalizeHex(), but skips the named
// variable.
std::string sha256Hex(std::string_view content);

} // namespace crypto
