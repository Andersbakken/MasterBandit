#include "Decoration.h"
#include "TerminalEmulator.h"
#include "TerminalSnapshot.h"
#include "TestTerminal.h"
#include <doctest/doctest.h>

// Unit tests for the C++ animation primitive — start / finish / cancel,
// replacement on same target+prop slot, sampler correctness, drain-on-
// decoration-removal. These don't go through JS and don't run the
// renderer; they exercise the data flow that the rendered tests will
// exercise end-to-end.
//
// `mono()` is overridden via `setMonoForTest` so any timing-dependent
// behaviour is deterministic. Each test's TearDown clears it; tests
// that don't set it should not be affected because the override is
// only consulted when non-zero.

namespace {

// RAII guard for setMonoForTest — restores wall-clock at scope exit so
// later tests in the same process aren't poisoned by leftover state.
struct MonoGuard
{
    MonoGuard(uint64_t t) { TerminalEmulator::setMonoForTest(t); }
    ~MonoGuard() { TerminalEmulator::setMonoForTest(0); }
};

// Add a decoration anchored on a line that doesn't exist. The animation
// API operates on decoration IDs without needing the anchor to resolve —
// fine for primitive tests. For tests that need the decoration to appear
// in a snapshot, feed text first and use a real lineId.
uint64_t addBareDecoration(TestTerminal &t)
{
    Decoration d;
    d.kind            = DecorationKind::User;
    d.startLineId     = 1;
    d.startCellOffset = 0;
    d.endLineId       = 1;
    d.endCellOffset   = 5;
    d.tag             = "test";
    return t.term.addDecoration(std::move(d));
}

Animation makeAnim(uint64_t handleId, uint64_t targetId, AnimProp prop, int64_t startV,
                   int64_t endV, uint64_t startMs, uint64_t durationMs,
                   Ease ease = Ease::Linear)
{
    Animation a;
    a.handleId        = handleId;
    a.kind            = AnimTargetKind::Decoration;
    a.targetId        = targetId;
    a.prop            = prop;
    a.desc.startMs    = startMs;
    a.desc.durationMs = durationMs;
    a.desc.startValue = startV;
    a.desc.endValue   = endV;
    a.desc.ease       = ease;
    return a;
}

} // namespace

// ============================================================================
// Easing curves
// ============================================================================

TEST_CASE("anim ease: boundaries are exact for every curve")
{
    for (auto e : { Ease::Linear, Ease::EaseIn, Ease::EaseOut, Ease::EaseInOut, Ease::EaseInOutCubic }) {
        CHECK(easeApply(e, 0.0f) == doctest::Approx(0.0f));
        CHECK(easeApply(e, 1.0f) == doctest::Approx(1.0f));
        // Out-of-range is clamped, not extrapolated
        CHECK(easeApply(e, -0.5f) == doctest::Approx(0.0f));
        CHECK(easeApply(e, 1.5f) == doctest::Approx(1.0f));
    }
}

TEST_CASE("anim ease: linear is identity")
{
    CHECK(easeApply(Ease::Linear, 0.25f) == doctest::Approx(0.25f));
    CHECK(easeApply(Ease::Linear, 0.5f) == doctest::Approx(0.5f));
    CHECK(easeApply(Ease::Linear, 0.75f) == doctest::Approx(0.75f));
}

TEST_CASE("anim ease: easeIn is t² (slow start)")
{
    CHECK(easeApply(Ease::EaseIn, 0.5f) == doctest::Approx(0.25f));
    // Strictly less than linear at 0 < t < 1
    CHECK(easeApply(Ease::EaseIn, 0.3f) < 0.3f);
}

TEST_CASE("anim ease: easeOut is 1-(1-t)² (fast start)")
{
    CHECK(easeApply(Ease::EaseOut, 0.5f) == doctest::Approx(0.75f));
    // Strictly greater than linear at 0 < t < 1
    CHECK(easeApply(Ease::EaseOut, 0.3f) > 0.3f);
}

TEST_CASE("anim ease: easeInOut is symmetric around 0.5")
{
    CHECK(easeApply(Ease::EaseInOut, 0.5f) == doctest::Approx(0.5f));
    // f(t) + f(1-t) == 1 for symmetric curves
    for (float t : { 0.1f, 0.25f, 0.4f, 0.6f, 0.75f, 0.9f }) {
        CHECK(easeApply(Ease::EaseInOut, t) + easeApply(Ease::EaseInOut, 1.0f - t) ==
              doctest::Approx(1.0f).epsilon(0.001f));
    }
}

TEST_CASE("anim ease: easeInOutCubic is symmetric around 0.5")
{
    CHECK(easeApply(Ease::EaseInOutCubic, 0.5f) == doctest::Approx(0.5f));
    for (float t : { 0.1f, 0.25f, 0.4f, 0.6f, 0.75f, 0.9f }) {
        CHECK(easeApply(Ease::EaseInOutCubic, t) + easeApply(Ease::EaseInOutCubic, 1.0f - t) ==
              doctest::Approx(1.0f).epsilon(0.001f));
    }
}

// ============================================================================
// animProgress (t = elapsed / duration, clamped)
// ============================================================================

TEST_CASE("animProgress: clamps to [0, 1] across the duration window")
{
    AnimDescriptor d {};
    d.startMs    = 1000;
    d.durationMs = 200;

    CHECK(animProgress(d, 500) == doctest::Approx(0.0f));   // before start
    CHECK(animProgress(d, 1000) == doctest::Approx(0.0f));  // at start
    CHECK(animProgress(d, 1100) == doctest::Approx(0.5f));  // halfway
    CHECK(animProgress(d, 1200) == doctest::Approx(1.0f));  // at end
    CHECK(animProgress(d, 9999) == doctest::Approx(1.0f));  // past end
}

TEST_CASE("animProgress: zero duration is instantly 1.0")
{
    AnimDescriptor d {};
    d.startMs    = 1000;
    d.durationMs = 0;
    CHECK(animProgress(d, 500) == doctest::Approx(1.0f));
    CHECK(animProgress(d, 1000) == doctest::Approx(1.0f));
}

// ============================================================================
// Scalar sampler
// ============================================================================

TEST_CASE("sampleAnimScalar: linear lerp between ints")
{
    AnimDescriptor d {};
    d.startMs    = 1000;
    d.durationMs = 200;
    d.startValue = 0;
    d.endValue   = 100;
    d.ease       = Ease::Linear;
    CHECK(sampleAnimScalar(d, 1000) == 0);
    CHECK(sampleAnimScalar(d, 1100) == 50);
    CHECK(sampleAnimScalar(d, 1200) == 100);
}

TEST_CASE("sampleAnimScalar: negative endValue (deflate)")
{
    AnimDescriptor d {};
    d.startMs    = 1000;
    d.durationMs = 100;
    d.startValue = 0;
    d.endValue   = -8;
    d.ease       = Ease::Linear;
    CHECK(sampleAnimScalar(d, 1000) == 0);
    CHECK(sampleAnimScalar(d, 1050) == -4);
    CHECK(sampleAnimScalar(d, 1100) == -8);
}

// ============================================================================
// Color sampler — values are linear-RGB-lerped between the two RGBA8s.
// We verify boundary conditions (t=0 returns startValue, t=1 returns
// endValue exactly) and a midpoint sanity check (alpha lerps linearly,
// RGB falls between the two endpoints).
// ============================================================================

TEST_CASE("sampleAnimColor: t=0 returns startValue exactly")
{
    AnimDescriptor d {};
    d.startMs    = 1000;
    d.durationMs = 200;
    d.startValue = 0xFF0000FF; // alpha=FF, blue=0, green=0, red=FF (in 0xAABBGGRR)
    d.endValue   = 0xFFFF0000; // alpha=FF, blue=FF, green=0, red=0
    d.ease       = Ease::Linear;
    CHECK(sampleAnimColor(d, 1000) == 0xFF0000FFu);
}

TEST_CASE("sampleAnimColor: t=1 returns endValue exactly")
{
    AnimDescriptor d {};
    d.startMs    = 1000;
    d.durationMs = 200;
    d.startValue = 0xFF0000FF;
    d.endValue   = 0xFFFF0000;
    d.ease       = Ease::Linear;
    CHECK(sampleAnimColor(d, 1200) == 0xFFFF0000u);
}

TEST_CASE("sampleAnimColor: alpha lerps linearly across the midpoint")
{
    AnimDescriptor d {};
    d.startMs    = 1000;
    d.durationMs = 200;
    d.startValue = 0x00000000; // fully transparent
    d.endValue   = 0xFF000000; // fully opaque black
    d.ease       = Ease::Linear;
    uint32_t mid     = sampleAnimColor(d, 1100);
    uint8_t midAlpha = (mid >> 24) & 0xFF;
    CHECK(midAlpha >= 126);
    CHECK(midAlpha <= 129); // ~127.5 ± rounding
}

// ============================================================================
// TerminalEmulator::startAnimation / finishAnimation / cancelAnimation
// ============================================================================

TEST_CASE("startAnimation: no replacement on empty slot returns 0")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    auto a = makeAnim(/*handleId=*/1, decId, AnimProp::Bg, 0xFF000000, 0xFFFFFFFF, 1000, 200);
    uint64_t prior = t.term.startAnimation(a);
    CHECK(prior == 0);
    CHECK(t.term.animations().size() == 1);
    CHECK(t.term.animations().count(1) == 1);
}

TEST_CASE("startAnimation: same (target, prop) replaces and returns prior handleId")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    t.term.startAnimation(makeAnim(/*handleId=*/1, decId, AnimProp::Bg, 0, 0xFF, 1000, 200));
    uint64_t prior = t.term.startAnimation(makeAnim(/*handleId=*/2, decId, AnimProp::Bg, 0xFF, 0, 1100, 200));
    CHECK(prior == 1);
    CHECK(t.term.animations().size() == 1);
    CHECK(t.term.animations().count(1) == 0);
    CHECK(t.term.animations().count(2) == 1);
}

TEST_CASE("startAnimation: same target, different prop coexists")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    CHECK(t.term.startAnimation(makeAnim(1, decId, AnimProp::Bg, 0, 1, 1000, 200)) == 0);
    CHECK(t.term.startAnimation(makeAnim(2, decId, AnimProp::Fg, 0, 1, 1000, 200)) == 0);
    CHECK(t.term.animations().size() == 2);
}

TEST_CASE("finishAnimation: writes endValue and removes the entry")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    t.term.startAnimation(makeAnim(1, decId, AnimProp::BgInflateX, 0, 8, 1000, 200));

    REQUIRE(t.term.finishAnimation(1));
    CHECK(t.term.animations().count(1) == 0);

    // Static field should now hold endValue.
    const auto &decs = t.term.decorations();
    REQUIRE(decs.size() == 1);
    CHECK(decs[0].style.bgInflateX == 8);
}

TEST_CASE("finishAnimation: unknown handleId returns false")
{
    TestTerminal t;
    CHECK_FALSE(t.term.finishAnimation(999));
}

TEST_CASE("cancelAnimation snapToEnd=true: writes endValue (same as finish)")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    t.term.startAnimation(makeAnim(1, decId, AnimProp::BgInflateY, 0, 16, 1000, 200));
    REQUIRE(t.term.cancelAnimation(1, /*snapToEnd=*/true, /*nowMs=*/1100));
    CHECK(t.term.animations().count(1) == 0);
    CHECK(t.term.decorations()[0].style.bgInflateY == 16);
}

TEST_CASE("cancelAnimation snapToEnd=false: writes sampled-at-now")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    t.term.startAnimation(makeAnim(1, decId, AnimProp::BgInflateX, 0, 100, 1000, 200, Ease::Linear));
    // Cancel at the midpoint — sampled value should be 50 (linear).
    REQUIRE(t.term.cancelAnimation(1, /*snapToEnd=*/false, /*nowMs=*/1100));
    CHECK(t.term.animations().count(1) == 0);
    CHECK(t.term.decorations()[0].style.bgInflateX == 50);
}

TEST_CASE("cancelAnimation: unknown handleId returns false")
{
    TestTerminal t;
    CHECK_FALSE(t.term.cancelAnimation(999, false, 1000));
}

// ============================================================================
// drainAnimsForDecoration: removing the underlying decoration drops its
// animations and reports their handleIds via the out-param.
// ============================================================================

TEST_CASE("removeDecoration drains animations and returns cancelled handleIds")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    t.term.startAnimation(makeAnim(1, decId, AnimProp::Bg, 0, 1, 1000, 200));
    t.term.startAnimation(makeAnim(2, decId, AnimProp::BgInflateX, 0, 8, 1000, 200));

    std::vector<uint64_t> cancelled;
    bool ok = t.term.removeDecoration(decId, &cancelled);
    CHECK(ok);
    CHECK(t.term.animations().empty());
    REQUIRE(cancelled.size() == 2);
    // Order of drain is implementation-defined; just check the set.
    bool has1 = (cancelled[0] == 1) || (cancelled[1] == 1);
    bool has2 = (cancelled[0] == 2) || (cancelled[1] == 2);
    CHECK(has1);
    CHECK(has2);
}

TEST_CASE("clearUserDecorations drains animations across removed decorations")
{
    TestTerminal t;
    uint64_t a = addBareDecoration(t);
    uint64_t b = addBareDecoration(t);
    t.term.startAnimation(makeAnim(1, a, AnimProp::Bg, 0, 1, 1000, 200));
    t.term.startAnimation(makeAnim(2, b, AnimProp::Fg, 0, 1, 1000, 200));

    std::vector<uint64_t> cancelled;
    size_t n = t.term.clearUserDecorations({}, &cancelled);
    CHECK(n == 2);
    CHECK(t.term.animations().empty());
    CHECK(cancelled.size() == 2);
}

TEST_CASE("applyDecorationBatch: Clear op also drains animations")
{
    TestTerminal t;
    uint64_t decId = addBareDecoration(t);
    // Tag is "test" from addBareDecoration.
    t.term.startAnimation(makeAnim(1, decId, AnimProp::Bg, 0, 1, 1000, 200));

    std::vector<DecorationBatchOp> ops;
    DecorationBatchOp clear;
    clear.kind     = DecorationBatchOp::Kind::Clear;
    clear.clearTag = "test";
    ops.push_back(std::move(clear));

    std::vector<uint64_t> cancelled;
    auto added = t.term.applyDecorationBatch(std::move(ops), &cancelled);
    CHECK(added.empty());
    CHECK(t.term.animations().empty());
    REQUIRE(cancelled.size() == 1);
    CHECK(cancelled[0] == 1);
}

// ============================================================================
// setMonoForTest: overrides mono() globally
// ============================================================================

TEST_CASE("setMonoForTest: mono() returns the override when non-zero")
{
    MonoGuard g(12345);
    CHECK(TerminalEmulator::mono() == 12345);
}

TEST_CASE("setMonoForTest: 0 restores wall-clock")
{
    {
        MonoGuard g(99999);
        CHECK(TerminalEmulator::mono() == 99999);
    }
    // After scope exit, mono() is wall-clock again. We can't predict its
    // value but it's certainly not 99999.
    CHECK(TerminalEmulator::mono() != 99999);
}

// ============================================================================
// Snapshot: animations are copied verbatim (no per-decoration sidecar)
// ============================================================================

TEST_CASE("snapshot: animations vector mirrors emulator's registry")
{
    TestTerminal t(40, 5);
    t.feed("hello world\r\n");
    // Use a real lineId so the decoration resolves and lands in
    // snapshot.decorations. Animation copy is independent of resolution
    // but if the decoration didn't appear, the renderer's apply pass
    // would skip it — covered by the next test.
    Decoration d;
    d.kind            = DecorationKind::User;
    d.startLineId     = t.term.document().lineIdForAbs(0);
    d.startCellOffset = 0;
    d.endLineId       = d.startLineId;
    d.endCellOffset   = 4; // "hello"
    d.tag             = "test";
    uint64_t decId    = t.term.addDecoration(std::move(d));

    t.term.startAnimation(makeAnim(7, decId, AnimProp::Bg, 0xFF000000, 0xFFFFFFFF, 1000, 200));

    TerminalSnapshot snap;
    snap.update(t.term);

    REQUIRE(snap.animations.size() == 1);
    CHECK(snap.animations[0].handleId == 7);
    CHECK(snap.animations[0].targetId == decId);
    CHECK(snap.animations[0].prop == AnimProp::Bg);
    CHECK(snap.animations[0].desc.startMs == 1000);
    CHECK(snap.animations[0].desc.durationMs == 200);
}
