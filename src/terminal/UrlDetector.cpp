#include "UrlDetector.h"

#include <re2/re2.h>

struct UrlDetector::Impl
{
    re2::RE2 regex;

    Impl()
        : regex(R"(\b(?:https?|file|ssh)://[A-Za-z0-9\-._~:/?#\[\]@!$&'*+,;=%()]+)",
                []
                {
                    re2::RE2::Options o;
                    o.set_log_errors(false);
                    return o;
                }())
    {
    }
};

const UrlDetector &UrlDetector::instance()
{
    static UrlDetector inst;
    return inst;
}

UrlDetector::UrlDetector()
    : mImpl(new Impl())
{
}

namespace {

// Trim characters at the right edge of `[start..end)` that are almost
// never part of a URL (sentence punctuation), AND drop unmatched closing
// parens / brackets. Returns the new end (>= start).
//
// The scheme is two-pass: first walk back over `.,;:!?` (low-cost, simple
// trailing-sentence cleanup), then balance parens / brackets — count `(`
// vs `)` (and `[` vs `]`) over the surviving span and pop trailing
// closers while they exceed openers. This catches `(see https://foo)`
// correctly without losing `https://en.wikipedia.org/wiki/Foo_(bar)`.
size_t trimTrailing(std::string_view text, size_t start, size_t end)
{
    auto isSentenceTrail = [](char c) -> bool
    {
        return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?';
    };
    while (end > start && isSentenceTrail(text[end - 1])) {
        --end;
    }
    // Balance parens + brackets — at most a few iterations in practice
    // since we stop as soon as opens >= closes.
    while (end > start) {
        char last = text[end - 1];
        if (last != ')' && last != ']') {
            break;
        }
        char open  = (last == ')') ? '(' : '[';
        char close = last;
        int opens = 0, closes = 0;
        for (size_t i = start; i < end; ++i) {
            if (text[i] == open) {
                ++opens;
            } else if (text[i] == close) {
                ++closes;
            }
        }
        if (closes > opens) {
            --end;
            // After dropping a `)`, sentence punctuation may now be
            // exposed as the new trailing character; re-trim.
            while (end > start && isSentenceTrail(text[end - 1])) {
                --end;
            }
        } else {
            break;
        }
    }
    return end;
}

} // namespace

std::vector<UrlDetector::Match>
UrlDetector::findUrls(std::string_view text) const
{
    std::vector<Match> out;
    if (text.empty()) {
        return out;
    }
    re2::StringPiece textSP(text.data(), text.size());
    re2::StringPiece sub;
    size_t pos          = 0;
    const size_t endpos = text.size();
    while (pos <= endpos) {
        if (!mImpl->regex.Match(textSP, pos, endpos, re2::RE2::UNANCHORED, &sub, 1)) {
            break;
        }
        size_t s = static_cast<size_t>(sub.data() - text.data());
        size_t e = s + sub.size();
        if (e == s) {
            // Defensive — the URL pattern can't match zero bytes (the
            // scheme alone is at least 6 chars), but guarantee progress.
            pos = s + 1;
            continue;
        }
        size_t trimmedEnd = trimTrailing(text, s, e);
        // After trimming, ensure at least one host character survives
        // past the `://`. Shortest valid scheme is `ssh://` (6 chars), so
        // requiring `> s + 6` keeps `ssh://X` and rejects degenerate
        // matches that trimmed away to bare scheme.
        if (trimmedEnd > s + 6) {
            out.push_back({ s, trimmedEnd });
        }
        pos = e; // resume past the original (untrimmed) match
    }
    return out;
}
