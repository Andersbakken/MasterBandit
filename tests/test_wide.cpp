#include <doctest/doctest.h>
#include "TestTerminal.h"

// CJK character U+4E2D (中), UTF-8: 0xE4 0xB8 0xAD — wcwidth == 2
static const std::string CJK_ZHONG = "\xe4\xb8\xad";

// Emoji U+1F600 😀, UTF-8: 0xF0 0x9F 0x98 0x80 — wcwidth == 2
static const std::string EMOJI_GRIN = "\xf0\x9f\x98\x80";

TEST_CASE("wide character occupies two columns")
{
    TestTerminal t;
    t.feed(CJK_ZHONG);
    CHECK(t.wc(0, 0) == U'\u4e2d');
    CHECK(t.attrs(0, 0).wide());
}

TEST_CASE("wide character second cell is a spacer")
{
    TestTerminal t;
    t.feed(CJK_ZHONG);
    CHECK(t.attrs(1, 0).wideSpacer());
}

TEST_CASE("cursor advances by two after wide character")
{
    TestTerminal t;
    t.feed(CJK_ZHONG);
    CHECK(t.term.cursorX() == 2);
}

TEST_CASE("narrow character after wide character is at correct column")
{
    TestTerminal t;
    t.feed(CJK_ZHONG + "A");
    CHECK(t.wc(2, 0) == U'A');
    CHECK_FALSE(t.attrs(2, 0).wide());
    CHECK_FALSE(t.attrs(2, 0).wideSpacer());
}

TEST_CASE("multiple wide characters advance cursor correctly")
{
    TestTerminal t;
    t.feed(CJK_ZHONG + CJK_ZHONG);
    CHECK(t.wc(0, 0) == U'\u4e2d');
    CHECK(t.attrs(1, 0).wideSpacer());
    CHECK(t.wc(2, 0) == U'\u4e2d');
    CHECK(t.attrs(3, 0).wideSpacer());
    CHECK(t.term.cursorX() == 4);
}

TEST_CASE("emoji wide character occupies two columns")
{
    TestTerminal t;
    t.feed(EMOJI_GRIN);
    CHECK(t.wc(0, 0) == U'\U0001f600');
    CHECK(t.attrs(0, 0).wide());
    CHECK(t.attrs(1, 0).wideSpacer());
    CHECK(t.term.cursorX() == 2);
}

// U+26A0 WARNING SIGN (narrow by default), UTF-8: 0xE2 0x9A 0xA0
static const std::string WARNING_SIGN = "\xe2\x9a\xa0";
// VS16 (U+FE0F, emoji presentation selector), UTF-8: 0xEF 0xB8 0x8F
static const std::string VS16 = "\xef\xb8\x8f";

TEST_CASE("VS16: warning sign without selector stays narrow")
{
    TestTerminal t;
    t.feed(WARNING_SIGN);
    CHECK(t.wc(0, 0) == U'\u26a0');
    CHECK_FALSE(t.attrs(0, 0).wide());
    CHECK_FALSE(t.attrs(1, 0).wideSpacer());
    CHECK(t.term.cursorX() == 1);
}

TEST_CASE("VS16: warning sign with VS16 is widened to two columns")
{
    TestTerminal t;
    t.feed(WARNING_SIGN + VS16);
    CHECK(t.wc(0, 0) == U'\u26a0');
    CHECK(t.attrs(0, 0).wide());
    CHECK(t.attrs(1, 0).wideSpacer());
    CHECK(t.term.cursorX() == 2);
}

TEST_CASE("VS16: combiningCps stored in cell extra")
{
    TestTerminal t;
    t.feed(WARNING_SIGN + VS16);
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    REQUIRE(ex->combiningCps.size() == 1);
    CHECK(ex->combiningCps[0] == U'\ufe0f');
}

TEST_CASE("VS16: character after emoji-with-VS16 lands at correct column")
{
    TestTerminal t;
    t.feed(WARNING_SIGN + VS16 + "A");
    CHECK(t.wc(0, 0) == U'\u26a0');
    CHECK(t.wc(2, 0) == U'A');
    CHECK(t.term.cursorX() == 3);
}

TEST_CASE("VS16: does not re-widen an already-wide emoji")
{
    TestTerminal t;
    t.feed(EMOJI_GRIN + VS16);
    CHECK(t.wc(0, 0) == U'\U0001f600');
    CHECK(t.attrs(0, 0).wide());
    CHECK(t.attrs(1, 0).wideSpacer());
    CHECK(t.term.cursorX() == 2);
}

// ── ZWJ emoji sequences ─────────────────────────────────────────────────────

// 🧑‍🌾 = U+1F9D1 ZWJ U+1F33E (farmer)
static const std::string FARMER = "\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x8c\xbe";

// 👨‍👩‍👧 = U+1F468 ZWJ U+1F469 ZWJ U+1F467 (family)
static const std::string FAMILY = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";

TEST_CASE("ZWJ emoji sequence stored as single grapheme cluster")
{
    TestTerminal t;
    t.feed(FARMER);
    // Base codepoint is U+1F9D1
    CHECK(t.wc(0, 0) == U'\U0001f9d1');
    CHECK(t.attrs(0, 0).wide());
    CHECK(t.attrs(1, 0).wideSpacer());
    // ZWJ + U+1F33E stored as combining codepoints
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    REQUIRE(ex->combiningCps.size() == 2);
    CHECK(ex->combiningCps[0] == U'\u200d');  // ZWJ
    CHECK(ex->combiningCps[1] == U'\U0001f33e'); // 🌾
    // Cursor after the single cluster
    CHECK(t.term.cursorX() == 2);
}

TEST_CASE("ZWJ family emoji stored as single grapheme cluster")
{
    TestTerminal t;
    t.feed(FAMILY);
    CHECK(t.wc(0, 0) == U'\U0001f468');
    CHECK(t.attrs(0, 0).wide());
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    // ZWJ + 👩 + ZWJ + 👧 = 4 continuation codepoints
    REQUIRE(ex->combiningCps.size() == 4);
    CHECK(ex->combiningCps[0] == U'\u200d');
    CHECK(ex->combiningCps[1] == U'\U0001f469');
    CHECK(ex->combiningCps[2] == U'\u200d');
    CHECK(ex->combiningCps[3] == U'\U0001f467');
    CHECK(t.term.cursorX() == 2);
}

TEST_CASE("character after ZWJ sequence lands at correct column")
{
    TestTerminal t;
    t.feed(FARMER + "A");
    CHECK(t.wc(0, 0) == U'\U0001f9d1');
    CHECK(t.wc(2, 0) == U'A');
    CHECK(t.term.cursorX() == 3);
}
