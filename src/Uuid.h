#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// RFC 4122 UUID v4 (random), stored as two 64-bit halves for cheap hashing
// and equality. Big-endian byte order when serialised: bytes 0..7 come from
// `high` (MSB first), bytes 8..15 from `low`.
struct Uuid {
    uint64_t high = 0;
    uint64_t low  = 0;

    // Generate a fresh v4 UUID using the platform CSPRNG
    // (arc4random on Apple, getrandom() on Linux with /dev/urandom fallback).
    static Uuid generate();

    // Canonical 36-char form: "xxxxxxxx-xxxx-4xxx-Nxxx-xxxxxxxxxxxx"
    // where N is one of 8, 9, a, b.
    std::string toString() const;

    // Parse canonical 36-char form. Returns the nil UUID on malformed input.
    static Uuid fromString(std::string_view s);

    bool isNil() const { return high == 0 && low == 0; }
    bool operator==(const Uuid&) const = default;
};

struct UuidHash {
    size_t operator()(const Uuid& u) const noexcept {
        return static_cast<size_t>(u.high ^ u.low);
    }
};
