#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace crypto {

// Streaming SHA-256. Feed bytes with update(); call finalizeHex() to read
// the 64-character lowercase hex digest. After finalize, the instance is
// drained — call reset() to reuse the same allocation for another digest.
//
// Backend is CommonCrypto on Apple, OpenSSL (EVP) elsewhere — the choice
// matches the existing one-shot. On Apple the platform context lives
// inline; on OpenSSL we own an EVP_MD_CTX heap allocation because the
// struct is opaque since 1.1.
class Sha256
{
public:
    Sha256();
    ~Sha256();

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

#ifdef __APPLE__
    // Size/alignment of the inline backend context buffer. Public so the
    // .cpp file's static_asserts can compare them against
    // sizeof(CC_SHA256_CTX) without needing a friend declaration. Treat
    // as implementation detail — callers shouldn't touch.
    static constexpr std::size_t kStateSize  = 112;
    static constexpr std::size_t kStateAlign = 8;
#endif

private:
#ifdef __APPLE__
    // Inline storage sized to hold CommonCrypto's CC_SHA256_CTX. The .cpp
    // file static_asserts the actual sizeof to catch a future struct
    // change.
    alignas(kStateAlign) unsigned char state_[kStateSize];
#else
    // EVP_MD_CTX is opaque since OpenSSL 1.1; store the pointer and let
    // EVP_MD_CTX_new/free manage the allocation.
    void *state_ = nullptr;
#endif
};

// One-shot convenience for callers that already have all the bytes — same
// digest as Sha256{}.update(content).finalizeHex(), but skips the named
// variable.
std::string sha256Hex(std::string_view content);

} // namespace crypto
