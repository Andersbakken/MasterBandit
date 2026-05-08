#include "TestTerminal.h"
#include "UrlDetector.h"
#include <doctest/doctest.h>

// UrlDetector unit tests — run the regex + trim pipeline directly with no
// terminal in the loop. Integration tests below drive bytes through
// TestTerminal and assert on cell-level hyperlinkId stamping.

namespace {

const UrlDetector &det()
{
    return UrlDetector::instance();
}

bool has(const std::vector<UrlDetector::Match> &m, std::string_view text, std::string_view want)
{
    for (const auto &h : m) {
        if (text.substr(h.byteStart, h.byteEnd - h.byteStart) == want) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("UrlDetector: bare https matches")
{
    std::string s = "see https://example.com for details";
    auto m        = det().findUrls(s);
    REQUIRE(m.size() == 1);
    CHECK(s.substr(m[0].byteStart, m[0].byteEnd - m[0].byteStart) == "https://example.com");
}

TEST_CASE("UrlDetector: http and https both match")
{
    std::string s = "old http://a.example new https://b.example";
    auto m        = det().findUrls(s);
    REQUIRE(m.size() == 2);
    CHECK(has(m, s, "http://a.example"));
    CHECK(has(m, s, "https://b.example"));
}

TEST_CASE("UrlDetector: file:// and ssh:// schemes")
{
    std::string s = "edit file:///etc/hosts then push to ssh://user@host:22/repo";
    auto m        = det().findUrls(s);
    REQUIRE(m.size() == 2);
    CHECK(has(m, s, "file:///etc/hosts"));
    CHECK(has(m, s, "ssh://user@host:22/repo"));
}

TEST_CASE("UrlDetector: mailto is intentionally NOT detected")
{
    // Regression guard: user specifically asked for mailto: to be dropped.
    std::string s = "contact mailto:foo@example.com about it";
    auto m        = det().findUrls(s);
    CHECK(m.empty());
}

TEST_CASE("UrlDetector: trailing sentence punctuation is trimmed")
{
    {
        std::string s = "visit https://example.com.";
        auto m        = det().findUrls(s);
        REQUIRE(m.size() == 1);
        CHECK(s.substr(m[0].byteStart, m[0].byteEnd - m[0].byteStart) == "https://example.com");
    }
    {
        std::string s = "Try https://example.com, ok?";
        auto m        = det().findUrls(s);
        REQUIRE(m.size() == 1);
        CHECK(s.substr(m[0].byteStart, m[0].byteEnd - m[0].byteStart) == "https://example.com");
    }
    {
        std::string s = "https://example.com!";
        auto m        = det().findUrls(s);
        REQUIRE(m.size() == 1);
        CHECK(s.substr(m[0].byteStart, m[0].byteEnd - m[0].byteStart) == "https://example.com");
    }
}

TEST_CASE("UrlDetector: parenthesized URL drops outer parens")
{
    std::string s = "(see https://en.wikipedia.org/wiki/Foo) for context";
    auto m        = det().findUrls(s);
    REQUIRE(m.size() == 1);
    CHECK(s.substr(m[0].byteStart, m[0].byteEnd - m[0].byteStart) == "https://en.wikipedia.org/wiki/Foo");
}

TEST_CASE("UrlDetector: balanced parens inside URL are preserved")
{
    std::string s = "see https://en.wikipedia.org/wiki/Foo_(bar) for context";
    auto m        = det().findUrls(s);
    REQUIRE(m.size() == 1);
    CHECK(s.substr(m[0].byteStart, m[0].byteEnd - m[0].byteStart) == "https://en.wikipedia.org/wiki/Foo_(bar)");
}

TEST_CASE("UrlDetector: word boundary prevents leading-letter match")
{
    // `xhttps://...` should not detect as `https://...` — the `x` glues
    // it into a different token, so \b suppresses the match.
    std::string s = "garbage xhttps://example.com";
    auto m        = det().findUrls(s);
    CHECK(m.empty());
}

TEST_CASE("UrlDetector: degenerate scheme-only matches are dropped")
{
    // The regex requires at least one char of host (the `+` in the
    // pattern), but trim could in theory chew everything past the
    // scheme. Guard against returning bare-scheme matches.
    std::string s = "https://";
    auto m        = det().findUrls(s);
    CHECK(m.empty());
}

// =============================================================================
// Integration tests — bytes through TestTerminal, assert on stamped hyperlinkId.
// =============================================================================

TEST_CASE("URL detection: https URL in line gets hyperlinkId on its cells via lineFeed")
{
    TestTerminal t(80, 5);
    t.feed("see https://example.com\r\n");

    // After lineFeed, the line we just left should have hyperlinkId stamped
    // on cells 4..22 ("https://example.com" = 19 chars at col 4..22 inclusive).
    int firstStamped = -1, lastStamped = -1;
    uint32_t firstId = 0;
    for (int c = 0; c < 80; ++c) {
        const CellExtra *ex = t.term.document().getExtra(c, 0);
        if (ex && ex->hyperlinkId != 0) {
            if (firstStamped < 0) {
                firstStamped = c;
                firstId      = ex->hyperlinkId;
            }
            lastStamped = c;
        }
    }
    REQUIRE(firstStamped == 4);   // "see " is 4 chars, then URL begins
    CHECK(lastStamped == 4 + 18); // "https://example.com" length 19, last cell at 22
    CHECK(firstId != 0);
    // Confirm registry holds the URI.
    const std::string *uri = t.term.hyperlinkURI(firstId);
    REQUIRE(uri != nullptr);
    CHECK(*uri == "https://example.com");
}

TEST_CASE("URL detection: alt screen is skipped")
{
    TestTerminal t(80, 5);
    // CSI ?1049h: enter alt screen.
    t.feed("\x1b[?1049h");
    t.feed("https://example.com\r\n");

    bool any = false;
    for (int c = 0; c < 80; ++c) {
        const CellExtra *ex = t.term.grid().getExtra(c, 0); // active grid = alt
        if (ex && ex->hyperlinkId != 0) {
            any = true;
            break;
        }
    }
    CHECK_FALSE(any);
}

TEST_CASE("URL detection: explicit OSC 8 hyperlink is not overwritten by URL detection")
{
    TestTerminal t(80, 5);
    // OSC 8 ; ; <uri> ST  -- open hyperlink ranging over "anchor"; close;
    // then on the same line emit a plain URL. URL detection on LF must
    // leave the OSC 8 cells untouched.
    t.feed("\x1b]8;;https://osc8.example\x1b\\anchor\x1b]8;;\x1b\\ then https://plain.example\r\n");

    // "anchor" lives at cols 0..5. Verify those have OSC-8 id.
    uint32_t osc8Id = 0;
    for (int c = 0; c < 6; ++c) {
        const CellExtra *ex = t.term.document().getExtra(c, 0);
        REQUIRE(ex != nullptr);
        REQUIRE(ex->hyperlinkId != 0);
        if (osc8Id == 0) {
            osc8Id = ex->hyperlinkId;
        }
        CHECK(ex->hyperlinkId == osc8Id);
    }
    // Confirm OSC 8 URI registered correctly.
    const std::string *osc8Uri = t.term.hyperlinkURI(osc8Id);
    REQUIRE(osc8Uri != nullptr);
    CHECK(*osc8Uri == "https://osc8.example");

    // Find the plain URL's hyperlinkId — must be different from osc8Id.
    uint32_t plainId = 0;
    for (int c = 6; c < 80; ++c) {
        const CellExtra *ex = t.term.document().getExtra(c, 0);
        if (ex && ex->hyperlinkId != 0 && ex->hyperlinkId != osc8Id) {
            plainId = ex->hyperlinkId;
            break;
        }
    }
    REQUIRE(plainId != 0);
    CHECK(plainId != osc8Id);
    const std::string *plainUri = t.term.hyperlinkURI(plainId);
    REQUIRE(plainUri != nullptr);
    CHECK(*plainUri == "https://plain.example");
}

TEST_CASE("URL detection: registry URI matches the trimmed URL text")
{
    TestTerminal t(80, 5);
    t.feed("end of line: https://example.com.\r\n");

    uint32_t hid = 0;
    for (int c = 0; c < 80; ++c) {
        const CellExtra *ex = t.term.document().getExtra(c, 0);
        if (ex && ex->hyperlinkId != 0) {
            hid = ex->hyperlinkId;
            break;
        }
    }
    REQUIRE(hid != 0);
    const std::string *uri = t.term.hyperlinkURI(hid);
    REQUIRE(uri != nullptr);
    CHECK(*uri == "https://example.com"); // trailing `.` trimmed
}
