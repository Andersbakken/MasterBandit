#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

// UrlDetector — find URL spans in plain UTF-8 text.
//
// Recognizes the schemes:
//   https://, http://, file://, ssh://
//
// `mailto:` is intentionally NOT detected: email addresses in scrollback
// (e.g. git log output, error messages, maintainer files) are common
// enough that auto-linking them would surface a high false-positive rate
// and the hover-underline would be visually noisy. Add it later behind
// config if a user asks.
//
// Anchored on a leading word boundary (\b) so substrings inside other
// tokens don't match (e.g. `xhttps://...` does not match).
//
// Trailing punctuation that is almost never part of the URL itself
// (`.,;:!?`) is trimmed from the right edge. Closing parens are kept by
// default (Wikipedia URLs end with `)`), but unbalanced trailing parens
// are trimmed: a match like `(see https://en.wikipedia.org/wiki/Foo)`
// returns just the URL, dropping the surrounding `(` ... `)`.
//
// All offsets are byte indices into the input, not codepoints. Callers
// that have a byte→cell mapping (e.g. Document::LineBuf::byteToOffset)
// can convert directly.
class UrlDetector
{
public:
    struct Match
    {
        size_t byteStart; // inclusive
        size_t byteEnd;   // exclusive
    };

    // Singleton — the underlying RE2 instance is compiled once and is
    // safe for concurrent reads (RE2::Match is thread-safe).
    static const UrlDetector &instance();

    std::vector<Match> findUrls(std::string_view text) const;

private:
    UrlDetector();
    UrlDetector(const UrlDetector &)            = delete;
    UrlDetector &operator=(const UrlDetector &) = delete;

    struct Impl;
    Impl *mImpl;
};
