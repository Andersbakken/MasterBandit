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
