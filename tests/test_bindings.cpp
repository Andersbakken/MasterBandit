// Tests for SequenceMatcher — the state machine behind multi-key binding
// sequences. Pins down current behavior so the upcoming "replay prefix on
// NoMatch" + timeout additions don't regress single-key or basic multi-key
// flows.

#include <doctest/doctest.h>
#include "Bindings.h"

namespace {

// Build a Binding with the given key sequence and a placeholder action.
// The action variant used for the placeholder doesn't matter for these
// tests — we only inspect which actions come back out of MatchResult.
Binding mkBinding(std::vector<KeyStroke> keys, Action::Any action)
{
    return Binding{std::move(keys), std::move(action)};
}

KeyStroke ks(const char* spec)
{
    auto parsed = parseKeyStroke(spec);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// Shorthand matchers for MatchResult::result
using R = SequenceMatcher::Result;

} // namespace

TEST_CASE("SequenceMatcher: single-key binding matches immediately")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+t")}, Action::NewTab{}),
    };
    SequenceMatcher sm;
    auto r = sm.advance(ks("ctrl+t"), bindings);
    CHECK(r.result == R::Match);
    REQUIRE(r.actions.size() == 1);
    CHECK(std::holds_alternative<Action::NewTab>(r.actions[0]));
}

TEST_CASE("SequenceMatcher: multi-key binding requires full sequence")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+x"), ks("2")}, Action::ClosePane{}),
    };
    SequenceMatcher sm;
    // First key: prefix match, no actions yet.
    auto a = sm.advance(ks("ctrl+x"), bindings);
    CHECK(a.result == R::Prefix);
    CHECK(a.actions.empty());

    // Second key completes the sequence.
    auto b = sm.advance(ks("2"), bindings);
    CHECK(b.result == R::Match);
    REQUIRE(b.actions.size() == 1);
    CHECK(std::holds_alternative<Action::ClosePane>(b.actions[0]));
}

TEST_CASE("SequenceMatcher: prefix then unrelated key is NoMatch")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+x"), ks("2")}, Action::ClosePane{}),
    };
    SequenceMatcher sm;
    CHECK(sm.advance(ks("ctrl+x"), bindings).result == R::Prefix);

    // Key that doesn't continue the sequence → NoMatch, matcher resets.
    auto r = sm.advance(ks("a"), bindings);
    CHECK(r.result == R::NoMatch);
    CHECK(r.actions.empty());

    // After NoMatch the matcher is reset; next ctrl+x is again a Prefix,
    // not a continuation of the aborted sequence.
    CHECK(sm.advance(ks("ctrl+x"), bindings).result == R::Prefix);
}

TEST_CASE("SequenceMatcher: exact match preferred over pure-prefix match")
{
    // Same stroke bound as both standalone and as a prefix of a longer
    // sequence. The standalone binding should fire immediately — the
    // longer binding becomes unreachable because the exact hit resets
    // the matcher before any follow-up key can arrive.
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+x")}, Action::NewTab{}),
        mkBinding({ks("ctrl+x"), ks("2")}, Action::ClosePane{}),
    };
    SequenceMatcher sm;
    auto r = sm.advance(ks("ctrl+x"), bindings);
    CHECK(r.result == R::Match);
    REQUIRE(r.actions.size() == 1);
    CHECK(std::holds_alternative<Action::NewTab>(r.actions[0]));
}

TEST_CASE("SequenceMatcher: multiple actions bound to same stroke all fire")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+t")}, Action::NewTab{}),
        mkBinding({ks("ctrl+t")}, Action::ReloadConfig{}),
    };
    SequenceMatcher sm;
    auto r = sm.advance(ks("ctrl+t"), bindings);
    CHECK(r.result == R::Match);
    REQUIRE(r.actions.size() == 2);
    CHECK(std::holds_alternative<Action::NewTab>(r.actions[0]));
    CHECK(std::holds_alternative<Action::ReloadConfig>(r.actions[1]));
}

TEST_CASE("SequenceMatcher: unrelated key with no prefix state is NoMatch")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+t")}, Action::NewTab{}),
    };
    SequenceMatcher sm;
    auto r = sm.advance(ks("a"), bindings);
    CHECK(r.result == R::NoMatch);
    CHECK(r.actions.empty());
}

TEST_CASE("SequenceMatcher: empty binding table returns NoMatch")
{
    std::vector<Binding> bindings;
    SequenceMatcher sm;
    CHECK(sm.advance(ks("ctrl+t"), bindings).result == R::NoMatch);
}

TEST_CASE("SequenceMatcher: reset() clears prefix state")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+x"), ks("2")}, Action::ClosePane{}),
    };
    SequenceMatcher sm;
    CHECK(sm.advance(ks("ctrl+x"), bindings).result == R::Prefix);
    sm.reset();
    // After reset, the 2 alone shouldn't match anything (it's the tail
    // of a sequence, not a standalone binding).
    CHECK(sm.advance(ks("2"), bindings).result == R::NoMatch);
}

TEST_CASE("SequenceMatcher: three-key sequence walks Prefix → Prefix → Match")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+a"), ks("b"), ks("c")}, Action::NewTab{}),
    };
    SequenceMatcher sm;
    CHECK(sm.advance(ks("ctrl+a"), bindings).result == R::Prefix);
    CHECK(sm.advance(ks("b"), bindings).result == R::Prefix);
    auto r = sm.advance(ks("c"), bindings);
    CHECK(r.result == R::Match);
    REQUIRE(r.actions.size() == 1);
    CHECK(std::holds_alternative<Action::NewTab>(r.actions[0]));
}

TEST_CASE("SequenceMatcher: NoMatch after prefix returns the aborted prefix keys")
{
    // Binding: ctrl+x, 2 → close_pane. User presses ctrl+x then 'a' —
    // ctrl+x should be returned as an abortedPrefix so the caller can
    // resend it to the shell. The current failing key ('a') is NOT
    // included; the caller handles it as a fresh keystroke.
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+x"), ks("2")}, Action::ClosePane{}),
    };
    SequenceMatcher sm;
    CHECK(sm.advance(ks("ctrl+x"), bindings).result == R::Prefix);
    auto r = sm.advance(ks("a"), bindings);
    CHECK(r.result == R::NoMatch);
    REQUIRE(r.abortedPrefix.size() == 1);
    CHECK(r.abortedPrefix[0] == ks("ctrl+x"));
}

TEST_CASE("SequenceMatcher: NoMatch with no prior prefix has empty abortedPrefix")
{
    // A single-stroke NoMatch (not preceded by any prefix keys) should
    // not produce anything to replay — the failing key is the caller's
    // concern to handle as a regular keystroke.
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+t")}, Action::NewTab{}),
    };
    SequenceMatcher sm;
    auto r = sm.advance(ks("a"), bindings);
    CHECK(r.result == R::NoMatch);
    CHECK(r.abortedPrefix.empty());
}

TEST_CASE("SequenceMatcher: multi-prefix NoMatch returns all prior prefix keys in order")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+a"), ks("b"), ks("c")}, Action::NewTab{}),
    };
    SequenceMatcher sm;
    CHECK(sm.advance(ks("ctrl+a"), bindings).result == R::Prefix);
    CHECK(sm.advance(ks("b"), bindings).result == R::Prefix);
    auto r = sm.advance(ks("x"), bindings);
    CHECK(r.result == R::NoMatch);
    REQUIRE(r.abortedPrefix.size() == 2);
    CHECK(r.abortedPrefix[0] == ks("ctrl+a"));
    CHECK(r.abortedPrefix[1] == ks("b"));
}

TEST_CASE("SequenceMatcher: Match clears prefix state, no abortedPrefix")
{
    std::vector<Binding> bindings = {
        mkBinding({ks("ctrl+x"), ks("2")}, Action::ClosePane{}),
    };
    SequenceMatcher sm;
    CHECK(sm.advance(ks("ctrl+x"), bindings).result == R::Prefix);
    auto r = sm.advance(ks("2"), bindings);
    CHECK(r.result == R::Match);
    CHECK(r.abortedPrefix.empty());
}

TEST_CASE("mergeKeyBindings: user shadows default on same stroke")
{
    std::vector<Binding> defaults = {
        mkBinding({ks("ctrl+shift+w")}, Action::ClosePane{}),
    };
    std::vector<Binding> user = {
        mkBinding({ks("ctrl+shift+w")}, Action::CloseTab{}),
    };
    auto merged = mergeKeyBindings(std::move(defaults), std::move(user));
    REQUIRE(merged.size() == 1);
    CHECK(std::holds_alternative<Action::CloseTab>(merged[0].action));
}

TEST_CASE("mergeKeyBindings: default for different stroke is preserved")
{
    std::vector<Binding> defaults = {
        mkBinding({ks("ctrl+shift+w")}, Action::ClosePane{}),
        mkBinding({ks("ctrl+shift+t")}, Action::NewTab{}),
    };
    std::vector<Binding> user = {
        mkBinding({ks("ctrl+shift+w")}, Action::CloseTab{}),
    };
    auto merged = mergeKeyBindings(std::move(defaults), std::move(user));
    REQUIRE(merged.size() == 2);
    CHECK(std::holds_alternative<Action::NewTab>(merged[0].action));
    CHECK(std::holds_alternative<Action::CloseTab>(merged[1].action));
}

TEST_CASE("mergeKeyBindings: empty user keeps all defaults")
{
    std::vector<Binding> defaults = {
        mkBinding({ks("ctrl+t")}, Action::NewTab{}),
        mkBinding({ks("ctrl+w")}, Action::ClosePane{}),
    };
    auto merged = mergeKeyBindings(std::move(defaults), {});
    REQUIRE(merged.size() == 2);
    CHECK(std::holds_alternative<Action::NewTab>(merged[0].action));
    CHECK(std::holds_alternative<Action::ClosePane>(merged[1].action));
}

TEST_CASE("mergeKeyBindings: empty defaults yields just user bindings")
{
    std::vector<Binding> user = {
        mkBinding({ks("ctrl+t")}, Action::NewTab{}),
    };
    auto merged = mergeKeyBindings({}, std::move(user));
    REQUIRE(merged.size() == 1);
    CHECK(std::holds_alternative<Action::NewTab>(merged[0].action));
}

TEST_CASE("mergeKeyBindings: multi-key sequence shadows by full sequence equality")
{
    std::vector<Binding> defaults = {
        mkBinding({ks("ctrl+x"), ks("2")}, Action::ClosePane{}),
        mkBinding({ks("ctrl+x"), ks("3")}, Action::NewTab{}),
    };
    std::vector<Binding> user = {
        mkBinding({ks("ctrl+x"), ks("2")}, Action::CloseTab{}),
    };
    auto merged = mergeKeyBindings(std::move(defaults), std::move(user));
    REQUIRE(merged.size() == 2);
    CHECK(std::holds_alternative<Action::NewTab>(merged[0].action));
    CHECK(std::holds_alternative<Action::CloseTab>(merged[1].action));
}

TEST_CASE("mergeKeyBindings: duplicate user bindings on same stroke are both kept")
{
    // Preserves the intentional double-fire feature: binding the same stroke
    // twice in user config should fire both actions (matches the dispatch
    // semantic tested in 'multiple actions bound to same stroke all fire').
    std::vector<Binding> user = {
        mkBinding({ks("ctrl+t")}, Action::NewTab{}),
        mkBinding({ks("ctrl+t")}, Action::ReloadConfig{}),
    };
    auto merged = mergeKeyBindings({}, std::move(user));
    REQUIRE(merged.size() == 2);
    CHECK(std::holds_alternative<Action::NewTab>(merged[0].action));
    CHECK(std::holds_alternative<Action::ReloadConfig>(merged[1].action));
}

TEST_CASE("mergeKeyBindings: stroke with different mods is not shadowed")
{
    std::vector<Binding> defaults = {
        mkBinding({ks("ctrl+w")}, Action::ClosePane{}),
    };
    std::vector<Binding> user = {
        mkBinding({ks("ctrl+shift+w")}, Action::CloseTab{}),
    };
    auto merged = mergeKeyBindings(std::move(defaults), std::move(user));
    REQUIRE(merged.size() == 2);
    CHECK(std::holds_alternative<Action::ClosePane>(merged[0].action));
    CHECK(std::holds_alternative<Action::CloseTab>(merged[1].action));
}

TEST_CASE("mergeKeyBindings: dispatch behavior — user-shadowed stroke fires only user action")
{
    std::vector<Binding> defaults = {
        mkBinding({ks("ctrl+shift+w")}, Action::ClosePane{}),
    };
    std::vector<Binding> user = {
        mkBinding({ks("ctrl+shift+w")}, Action::CloseTab{}),
    };
    auto merged = mergeKeyBindings(std::move(defaults), std::move(user));

    SequenceMatcher sm;
    auto r = sm.advance(ks("ctrl+shift+w"), merged);
    CHECK(r.result == R::Match);
    REQUIRE(r.actions.size() == 1);
    CHECK(std::holds_alternative<Action::CloseTab>(r.actions[0]));
}
