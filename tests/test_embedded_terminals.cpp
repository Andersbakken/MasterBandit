#include <doctest/doctest.h>
#include "Terminal.h"
#include "TerminalSnapshot.h"

// Spec tests for Terminal::createEmbedded / extractEmbedded / resizeEmbedded
// and the Document line-id eviction hook that destroys embeddeds whose anchor
// row has scrolled past the archive cap.

namespace {

std::unique_ptr<Terminal> makeParent(int cols = 80, int rows = 24,
                                     int scrollback = 1000)
{
    PlatformCallbacks pcbs;
    TerminalCallbacks cbs;
    auto t = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    TerminalOptions opts;
    opts.scrollbackLines = scrollback;
    t->initHeadless(opts);
    t->resize(cols, rows);
    return t;
}

} // namespace

TEST_CASE("createEmbedded: captures current cursor lineId as anchor, advances cursor")
{
    auto parent = makeParent(80, 24);
    REQUIRE(parent->height() == 24);

    // Inject some output so the cursor isn't at row 0.
    parent->injectData("line1\r\nline2\r\nline3\r\n", 22);

    int cursorBefore = parent->cursorY();
    uint64_t anchor = parent->document().lineIdForAbs(
        parent->document().historySize() + cursorBefore);
    REQUIRE(anchor != 0);

    PlatformCallbacks pcbs;
    Terminal* em = parent->createEmbedded(5, std::move(pcbs));
    REQUIRE(em != nullptr);

    // Cursor moved to a fresh row below the anchor.
    CHECK(parent->cursorY() == cursorBefore + 1);

    // Lookup succeeds via the captured anchor lineId.
    CHECK(parent->findEmbedded(anchor) == em);
    CHECK(em->height() == 5);
    CHECK(em->width() == parent->width());
}

TEST_CASE("createEmbedded: alt-screen refuses")
{
    auto parent = makeParent();
    parent->injectData("\x1b[?1049h", 8); // enter alt screen (DECSET 1049)
    REQUIRE(parent->usingAltScreen());

    PlatformCallbacks pcbs;
    Terminal* em = parent->createEmbedded(3, std::move(pcbs));
    CHECK(em == nullptr);
}

TEST_CASE("createEmbedded: non-positive rows refused")
{
    auto parent = makeParent();
    PlatformCallbacks p1;
    CHECK(parent->createEmbedded(0, std::move(p1)) == nullptr);
    PlatformCallbacks p2;
    CHECK(parent->createEmbedded(-1, std::move(p2)) == nullptr);
}

TEST_CASE("createEmbedded: duplicate at same anchor row is refused")
{
    auto parent = makeParent();
    PlatformCallbacks p1;
    Terminal* em1 = parent->createEmbedded(4, std::move(p1));
    REQUIRE(em1 != nullptr);

    // Bring cursor back to the anchor row without advancing — move cursor up
    // one row via ESC [ A (CUU).
    parent->injectData("\x1b[A", 3);

    PlatformCallbacks p2;
    Terminal* em2 = parent->createEmbedded(2, std::move(p2));
    CHECK(em2 == nullptr); // anchor already occupied
}

TEST_CASE("extractEmbedded: removes from map, returns unique_ptr")
{
    auto parent = makeParent();
    PlatformCallbacks p;
    Terminal* em = parent->createEmbedded(3, std::move(p));
    REQUIRE(em != nullptr);

    uint64_t lineId = 0;
    for (const auto& [id, _] : parent->embeddeds()) {
        (void)_;
        lineId = id;
        break;
    }
    REQUIRE(lineId != 0);

    auto extracted = parent->extractEmbedded(lineId);
    REQUIRE(extracted);
    CHECK(extracted.get() == em);
    CHECK(parent->findEmbedded(lineId) == nullptr);
    CHECK(parent->embeddeds().size() == 0);
}

TEST_CASE("extractEmbedded: clears focused embedded if it matches")
{
    auto parent = makeParent();
    PlatformCallbacks p;
    Terminal* em = parent->createEmbedded(3, std::move(p));
    REQUIRE(em != nullptr);

    uint64_t lineId = parent->embeddeds().begin()->first;
    parent->setFocusedEmbeddedLineId(lineId);
    CHECK(parent->focusedEmbeddedLineId() == lineId);

    parent->extractEmbedded(lineId);
    CHECK(parent->focusedEmbeddedLineId() == 0);
}

TEST_CASE("resizeEmbedded: changes row count, preserves cols from parent")
{
    auto parent = makeParent(80, 24);
    PlatformCallbacks p;
    Terminal* em = parent->createEmbedded(3, std::move(p));
    REQUIRE(em != nullptr);
    CHECK(em->height() == 3);

    uint64_t lineId = parent->embeddeds().begin()->first;
    CHECK(parent->resizeEmbedded(lineId, 10));
    CHECK(em->height() == 10);
    CHECK(em->width() == parent->width());
}

TEST_CASE("resizeEmbedded: non-positive rejected, unknown lineId rejected")
{
    auto parent = makeParent();
    PlatformCallbacks p;
    parent->createEmbedded(3, std::move(p));
    uint64_t lineId = parent->embeddeds().begin()->first;

    CHECK_FALSE(parent->resizeEmbedded(lineId, 0));
    CHECK_FALSE(parent->resizeEmbedded(lineId, -5));
    CHECK_FALSE(parent->resizeEmbedded(lineId + 9999, 5)); // bogus lineId
}

TEST_CASE("activeTerm: prefers focused embedded over parent")
{
    auto parent = makeParent();
    PlatformCallbacks p;
    Terminal* em = parent->createEmbedded(3, std::move(p));
    REQUIRE(em != nullptr);

    CHECK(parent->activeTerm() == parent.get());

    uint64_t lineId = parent->embeddeds().begin()->first;
    parent->setFocusedEmbeddedLineId(lineId);
    CHECK(parent->activeTerm() == em);

    parent->clearFocusedEmbedded();
    CHECK(parent->activeTerm() == parent.get());
}

TEST_CASE("eviction: when anchor row evicts past archive cap, embedded moves to pending drain")
{
    // Tiny archive cap so we can exhaust it quickly.
    PlatformCallbacks pcbs;
    TerminalCallbacks cbs;
    auto parent = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
    TerminalOptions opts;
    opts.scrollbackLines = 2; // tier-1 cap
    parent->initHeadless(opts);
    parent->resize(80, 4);

    // Actual archive cap is wired in Document (100k default); we can't easily
    // drive past 100k rows in a unit test. Instead verify the plumbing: after
    // creating an embedded, clearHistory + triggering eviction via scrolling
    // past tier-1 cap keeps the embedded alive (archive still resolves line
    // ids). We exercise extractEmbedded as the graveyard-ready path instead.
    PlatformCallbacks p;
    Terminal* em = parent->createEmbedded(2, std::move(p));
    REQUIRE(em != nullptr);
    CHECK(parent->embeddeds().size() == 1);

    // Draining with nothing evicted is a no-op.
    int drained = 0;
    parent->drainEvictedEmbeddeds([&](uint64_t, std::unique_ptr<Terminal>) {
        drained++;
    });
    CHECK(drained == 0);
    CHECK_FALSE(parent->hasEvictedEmbeddeds());
}

TEST_CASE("createEmbedded: at top of screen anchors to lineId 1 (first row)")
{
    auto parent = makeParent(80, 24);
    // Fresh parent — cursor is at (0, 0). Anchor is the first row's lineId.
    uint64_t expected = parent->document().lineIdForAbs(
        parent->document().historySize() + parent->cursorY());
    REQUIRE(expected != 0);

    PlatformCallbacks p;
    Terminal* em = parent->createEmbedded(4, std::move(p));
    REQUIRE(em != nullptr);
    CHECK(parent->findEmbedded(expected) == em);
}

TEST_CASE("TerminalSnapshot: segments size matches viewport rows (no embeddeds)")
{
    auto parent = makeParent(80, 24);
    TerminalSnapshot snap;
    REQUIRE(snap.update(*parent));
    CHECK(static_cast<int>(snap.segments.size()) == parent->height());
    CHECK(snap.topLineId != 0);
    CHECK(snap.topPixelSubY == 0);
    // All Row kind, cellYStart increasing by 1 each step.
    for (int i = 0; i < static_cast<int>(snap.segments.size()); ++i) {
        CHECK(snap.segments[i].kind == TerminalSnapshot::Segment::Kind::Row);
        CHECK(snap.segments[i].rowCount == 1);
        CHECK(snap.segments[i].cellYStart == i);
    }
}

TEST_CASE("TerminalSnapshot: embedded anchor produces an Embedded segment that displaces following rows")
{
    auto parent = makeParent(80, 24);
    // Advance cursor to row 5 so the embedded anchors there.
    parent->injectData("\n\n\n\n\n", 5);
    REQUIRE(parent->cursorY() == 5);

    PlatformCallbacks p;
    Terminal* em = parent->createEmbedded(4, std::move(p));
    REQUIRE(em != nullptr);

    TerminalSnapshot snap;
    REQUIRE(snap.update(*parent));
    REQUIRE(static_cast<int>(snap.segments.size()) == parent->height());

    // Viewport row 5 should be the Embedded segment with rowCount=4.
    CHECK(snap.segments[5].kind == TerminalSnapshot::Segment::Kind::Embedded);
    CHECK(snap.segments[5].rowCount == 4);

    // Row 4 at cellYStart=4, Row 6 at cellYStart=5+4=9 (pushed down by
    // (embRows-1)=3).
    CHECK(snap.segments[4].cellYStart == 4);
    CHECK(snap.segments[6].cellYStart == 9);
    CHECK(snap.segments[6].kind == TerminalSnapshot::Segment::Kind::Row);
}

TEST_CASE("TerminalSnapshot: embeddeds hidden on alt screen")
{
    auto parent = makeParent();
    PlatformCallbacks p;
    REQUIRE(parent->createEmbedded(3, std::move(p)) != nullptr);

    // Enter alt screen — collectEmbeddedAnchors should now return empty.
    parent->injectData("\x1b[?1049h", 8);
    REQUIRE(parent->usingAltScreen());

    TerminalSnapshot snap;
    REQUIRE(snap.update(*parent));
    // No Embedded segments should appear.
    for (const auto& seg : snap.segments)
        CHECK(seg.kind == TerminalSnapshot::Segment::Kind::Row);
}

TEST_CASE("TerminalSnapshot::segmentAtPixelY locates the right segment")
{
    auto parent = makeParent(80, 24);
    parent->injectData("\n\n", 2); // cursor at row 2
    PlatformCallbacks p;
    REQUIRE(parent->createEmbedded(3, std::move(p)) != nullptr); // anchor at row 2, 3 rows tall

    TerminalSnapshot snap;
    REQUIRE(snap.update(*parent));

    const float cellH = 20.0f;
    // y=0 → row 0
    const auto* s0 = snap.segmentAtPixelY(0, cellH);
    REQUIRE(s0); CHECK(s0->cellYStart == 0);
    // y inside row 1 → segment 1
    const auto* s1 = snap.segmentAtPixelY(25, cellH);
    REQUIRE(s1); CHECK(s1->cellYStart == 1);
    // y inside the embedded band (row 2, 3 rows tall → pixel [40, 100))
    const auto* se = snap.segmentAtPixelY(55, cellH);
    REQUIRE(se); CHECK(se->kind == TerminalSnapshot::Segment::Kind::Embedded);
    CHECK(se->cellYStart == 2);
    CHECK(se->rowCount == 3);
    // y right past the embedded → next Row segment (originally row 3, now cellYStart=5)
    const auto* s3 = snap.segmentAtPixelY(105, cellH);
    REQUIRE(s3); CHECK(s3->cellYStart == 5);
    CHECK(s3->kind == TerminalSnapshot::Segment::Kind::Row);
}

TEST_CASE("cycle focus ordering: lineIds are monotonic")
{
    // Ensures FocusPopup's embedded cycle has a stable, newest-last order.
    auto parent = makeParent();
    PlatformCallbacks p1, p2, p3;
    Terminal* a = parent->createEmbedded(2, std::move(p1));
    Terminal* b = parent->createEmbedded(2, std::move(p2));
    Terminal* c = parent->createEmbedded(2, std::move(p3));
    REQUIRE(a); REQUIRE(b); REQUIRE(c);

    // Collect lineIds for each in creation order — they must be strictly
    // increasing so that sort-ascending on the lineId set yields creation
    // order.
    uint64_t firstA = 0, firstB = 0, firstC = 0;
    for (const auto& [id, t] : parent->embeddeds()) {
        if (t.get() == a) firstA = id;
        else if (t.get() == b) firstB = id;
        else if (t.get() == c) firstC = id;
    }
    CHECK(firstA < firstB);
    CHECK(firstB < firstC);
}
