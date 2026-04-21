// Regression tests for Uuid::toString / Uuid::fromString.
//
// Guards the prior bug where toString placed hyphens at byte indices 4/6/8/10
// instead of string positions 8/13/18/23, producing malformed 36-char strings
// that rejected as nil on round-trip.

#include <doctest/doctest.h>

#include "Uuid.h"

#include <string>
#include <unordered_set>

TEST_CASE("Uuid: canonical form has hyphens at positions 8/13/18/23")
{
    Uuid u = Uuid::generate();
    std::string s = u.toString();
    REQUIRE(s.size() == 36);
    CHECK(s[8]  == '-');
    CHECK(s[13] == '-');
    CHECK(s[18] == '-');
    CHECK(s[23] == '-');
    // No other hyphens; every other char is lowercase hex.
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const char c = s[i];
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(isHex);
    }
}

TEST_CASE("Uuid: v4 variant bits are set (version 4, variant 10xx)")
{
    for (int i = 0; i < 64; ++i) {
        Uuid u = Uuid::generate();
        std::string s = u.toString();
        // Position 14 is the version nibble; must be '4'.
        CHECK(s[14] == '4');
        // Position 19 is the variant nibble; must be one of 8, 9, a, b.
        const char v = s[19];
        CHECK((v == '8' || v == '9' || v == 'a' || v == 'b'));
    }
}

TEST_CASE("Uuid: toString -> fromString round-trips for generated UUIDs")
{
    for (int i = 0; i < 256; ++i) {
        Uuid u = Uuid::generate();
        REQUIRE_FALSE(u.isNil());
        Uuid v = Uuid::fromString(u.toString());
        CHECK(v == u);
        CHECK_FALSE(v.isNil());
    }
}

TEST_CASE("Uuid: fromString accepts canonical hand-written form")
{
    // Handcrafted valid v4 UUID (version 4, variant 'a').
    const std::string s = "12345678-90ab-4cde-a123-456789abcdef";
    Uuid u = Uuid::fromString(s);
    REQUIRE_FALSE(u.isNil());
    CHECK(u.toString() == s);
}

TEST_CASE("Uuid: fromString accepts uppercase hex and normalizes to lowercase")
{
    const std::string upper = "12345678-90AB-4CDE-A123-456789ABCDEF";
    const std::string lower = "12345678-90ab-4cde-a123-456789abcdef";
    Uuid u = Uuid::fromString(upper);
    REQUIRE_FALSE(u.isNil());
    CHECK(u.toString() == lower);
}

TEST_CASE("Uuid: fromString rejects malformed inputs")
{
    // Wrong length.
    CHECK(Uuid::fromString("").isNil());
    CHECK(Uuid::fromString("12345").isNil());
    CHECK(Uuid::fromString(std::string(35, 'a')).isNil());
    CHECK(Uuid::fromString(std::string(37, 'a')).isNil());

    // Hyphens in the wrong places.
    CHECK(Uuid::fromString("123456789-0ab-4cde-a123-456789abcdef").isNil());
    CHECK(Uuid::fromString("12345678-90ab4-cde-a123-456789abcdef").isNil());

    // Non-hex char where a hex digit should be.
    CHECK(Uuid::fromString("12345678-90ab-4cde-a123-456789abcdeZ").isNil());
    CHECK(Uuid::fromString("ZZZZZZZZ-ZZZZ-4ZZZ-aZZZ-ZZZZZZZZZZZZ").isNil());
}

TEST_CASE("Uuid: nil UUID round-trips as all zeros")
{
    Uuid nil{};
    CHECK(nil.isNil());
    const std::string s = nil.toString();
    CHECK(s == "00000000-0000-0000-0000-000000000000");
    CHECK(Uuid::fromString(s) == nil);
}

TEST_CASE("Uuid: byte-order is big-endian (bytes 0..7 from high, 8..15 from low)")
{
    // Construct a UUID with a known bit pattern and verify the textual form
    // matches big-endian expectations. high = 0x0123456789abcdef places
    // 01 23 45 67 89 ab cd ef into string positions 0..15 (skipping hyphens).
    Uuid u{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    CHECK(u.toString() == "01234567-89ab-cdef-fedc-ba9876543210");
}

TEST_CASE("Uuid: generated UUIDs are unique across many samples")
{
    std::unordered_set<std::string> seen;
    const int n = 1024;
    for (int i = 0; i < n; ++i) {
        seen.insert(Uuid::generate().toString());
    }
    CHECK(seen.size() == n);
}
