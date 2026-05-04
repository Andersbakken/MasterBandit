#include <doctest/doctest.h>
#include "TestTerminal.h"

TEST_CASE("OSC 0 sets window title")
{
    TestTerminal t;
    t.osc("0;My Window Title");
    CHECK(t.capturedTitle == "My Window Title");
}

TEST_CASE("OSC 2 sets window title")
{
    TestTerminal t;
    t.osc("2;Hello Terminal");
    CHECK(t.capturedTitle == "Hello Terminal");
}

TEST_CASE("OSC 1 sets icon name")
{
    TestTerminal t;
    t.osc("1;myicon");
    CHECK(t.capturedIcon == "myicon");
}

TEST_CASE("OSC title with ESC-backslash terminator")
{
    TestTerminal t;
    // ST-terminated (ESC \) instead of BEL
    t.feed("\x1b]2;ST Title\x1b\\");
    CHECK(t.capturedTitle == "ST Title");
}

TEST_CASE("OSC title updates on successive calls")
{
    TestTerminal t;
    t.osc("2;First");
    CHECK(t.capturedTitle == "First");
    t.osc("2;Second");
    CHECK(t.capturedTitle == "Second");
}

// ── Title stack (CSI 22/23 t) ────────────────────────────────────────────────

TEST_CASE("CSI 22t pushes title, CSI 23t pops and restores")
{
    TestTerminal t;
    t.osc("0;shell");
    CHECK(t.capturedTitle == "shell");
    t.csi("22t");              // push "shell"
    t.osc("0;vim foo.txt");
    CHECK(t.capturedTitle == "vim foo.txt");
    t.csi("23t");              // pop → "shell"
    CHECK(t.capturedTitle == "shell");
}

TEST_CASE("OSC 0 between push/pop does not affect saved title")
{
    TestTerminal t;
    t.osc("0;original");
    t.csi("22t");
    t.osc("0;changed");
    t.osc("0;changed again");
    CHECK(t.capturedTitle == "changed again");
    t.csi("23t");
    CHECK(t.capturedTitle == "original");
}

TEST_CASE("Pop last title entry clears to empty string")
{
    TestTerminal t;
    t.osc("0;only title");
    CHECK(t.capturedTitle == "only title");
    t.csi("23t");
    CHECK(t.capturedTitle.empty());
}

TEST_CASE("Pop on empty stack is a no-op")
{
    TestTerminal t;
    t.csi("23t");
    CHECK(t.capturedTitle.empty());
}

TEST_CASE("Nested push/pop restores correctly")
{
    TestTerminal t;
    t.osc("0;level0");
    t.csi("22t");              // push level0
    t.osc("0;level1");
    t.csi("22t");              // push level1
    t.osc("0;level2");
    CHECK(t.capturedTitle == "level2");
    t.csi("23t");              // pop → level1
    CHECK(t.capturedTitle == "level1");
    t.csi("23t");              // pop → level0
    CHECK(t.capturedTitle == "level0");
}

TEST_CASE("Title stack caps at 10 entries")
{
    TestTerminal t;
    t.osc("0;base");
    for (int i = 0; i < 20; ++i)
        t.csi("22t");
    // Should not have grown beyond 10; pop all and verify we get back to base
    int pops = 0;
    while (pops < 20) {
        t.csi("23t");
        ++pops;
        if (t.capturedTitle.empty()) break;
    }
    // Should have popped at most 10 times before emptying
    CHECK(pops <= 10);
}

TEST_CASE("OSC 0 on empty stack creates first entry")
{
    TestTerminal t;
    CHECK(t.capturedTitle.empty());
    t.osc("0;first");
    CHECK(t.capturedTitle == "first");
    // Push should work now
    t.csi("22t");
    t.osc("0;second");
    t.csi("23t");
    CHECK(t.capturedTitle == "first");
}

// ── Icon stack (CSI 22;1 / 23;1 t) ──────────────────────────────────────────

TEST_CASE("CSI 22;1t pushes icon, CSI 23;1t pops and restores")
{
    TestTerminal t;
    t.osc("1;shell-icon");
    CHECK(t.capturedIcon == "shell-icon");
    t.csi("22;1t");
    t.osc("1;jsh");
    CHECK(t.capturedIcon == "jsh");
    t.csi("23;1t");
    CHECK(t.capturedIcon == "shell-icon");
}

TEST_CASE("CSI 23;1t on single-entry icon stack clears to empty")
{
    TestTerminal t;
    t.osc("1;jsh");
    CHECK(t.capturedIcon == "jsh");
    t.csi("23;1t");
    CHECK(t.capturedIcon.empty());
}

TEST_CASE("Icon push/pop is independent of title stack")
{
    TestTerminal t;
    t.osc("2;title-a");
    t.osc("1;icon-a");
    t.csi("22;1t");           // push icon only
    t.osc("1;icon-b");
    CHECK(t.capturedTitle == "title-a");
    CHECK(t.capturedIcon == "icon-b");
    t.csi("23;1t");           // pop icon only
    CHECK(t.capturedTitle == "title-a");
    CHECK(t.capturedIcon == "icon-a");
}

TEST_CASE("CSI 22t / 23t with no Ps pushes and pops both stacks")
{
    TestTerminal t;
    t.osc("2;title-a");
    t.osc("1;icon-a");
    t.csi("22t");             // push both
    t.osc("2;title-b");
    t.osc("1;icon-b");
    CHECK(t.capturedTitle == "title-b");
    CHECK(t.capturedIcon == "icon-b");
    t.csi("23t");             // pop both
    CHECK(t.capturedTitle == "title-a");
    CHECK(t.capturedIcon == "icon-a");
}

TEST_CASE("CSI 22;0t / 23;0t explicitly pushes and pops both stacks")
{
    TestTerminal t;
    t.osc("2;title-a");
    t.osc("1;icon-a");
    t.csi("22;0t");
    t.osc("2;title-b");
    t.osc("1;icon-b");
    t.csi("23;0t");
    CHECK(t.capturedTitle == "title-a");
    CHECK(t.capturedIcon == "icon-a");
}

TEST_CASE("CSI 22;2t pushes only title, leaves icon untouched")
{
    TestTerminal t;
    t.osc("2;title-a");
    t.osc("1;icon-a");
    t.csi("22;2t");           // push title only
    t.osc("2;title-b");
    t.osc("1;icon-b");
    t.csi("23;2t");           // pop title only
    CHECK(t.capturedTitle == "title-a");
    CHECK(t.capturedIcon == "icon-b");
}

TEST_CASE("Push-icon on empty stack, set icon, pop-icon clears icon")
{
    TestTerminal t;
    t.csi("22;1t");
    t.osc("1;temp");
    CHECK(t.capturedIcon == "temp");
    t.csi("23;1t");
    CHECK(t.capturedIcon.empty());
}

// ── OSC 7 (CWD) ─────────────────────────────────────────────────────────────

TEST_CASE("OSC 7 extracts path from file URL")
{
    TestTerminal t;
    t.osc("7;file://localhost/home/user/project");
    CHECK(t.capturedCWD == "/home/user/project");
}

TEST_CASE("OSC 7 handles missing hostname")
{
    TestTerminal t;
    t.osc("7;file:///tmp");
    CHECK(t.capturedCWD == "/tmp");
}

// ── OSC 8 (Hyperlinks) ──────────────────────────────────────────────────────

TEST_CASE("OSC 8 sets hyperlink on cells")
{
    TestTerminal t;
    t.osc("8;;https://example.com");
    t.feed("click");
    t.osc("8;;");

    // Cells 0-4 should have hyperlink extras
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    CHECK(ex->hyperlinkId != 0);

    const std::string* uri = t.term.hyperlinkURI(ex->hyperlinkId);
    REQUIRE(uri != nullptr);
    CHECK(*uri == "https://example.com");

    // Cell 5 should not have a hyperlink
    const CellExtra* ex5 = t.term.grid().getExtra(5, 0);
    CHECK((ex5 == nullptr || ex5->hyperlinkId == 0));
}

TEST_CASE("OSC 8 clears active hyperlink")
{
    TestTerminal t;
    t.osc("8;;https://example.com");
    t.feed("A");
    t.osc("8;;");
    t.feed("B");

    const CellExtra* exA = t.term.grid().getExtra(0, 0);
    REQUIRE(exA != nullptr);
    CHECK(exA->hyperlinkId != 0);

    const CellExtra* exB = t.term.grid().getExtra(1, 0);
    CHECK((exB == nullptr || exB->hyperlinkId == 0));
}

TEST_CASE("OSC 8 with id= reuses same hyperlink entry")
{
    TestTerminal t;
    t.osc("8;id=link1;https://example.com");
    t.feed("A");
    t.osc("8;;");
    t.feed(" ");
    t.osc("8;id=link1;https://example.com");
    t.feed("B");
    t.osc("8;;");

    const CellExtra* exA = t.term.grid().getExtra(0, 0);
    const CellExtra* exB = t.term.grid().getExtra(2, 0);
    REQUIRE(exA != nullptr);
    REQUIRE(exB != nullptr);
    CHECK(exA->hyperlinkId == exB->hyperlinkId);
}

TEST_CASE("OSC 8 linkAt: hyperlink URI resolved from cell extra")
{
    TestTerminal t;
    t.osc("8;;https://example.com");
    t.feed("link");
    t.osc("8;;");
    t.feed(" plain");

    const auto& doc = t.term.document();
    // Row 0 is the only row, get its stable line ID
    uint64_t lineId = doc.lineIdForAbs(doc.historySize());
    (void)lineId;

    // Cell 0 (inside link) should resolve to the URL
    const CellExtra* ex0 = t.term.grid().getExtra(0, 0);
    REQUIRE(ex0 != nullptr);
    REQUIRE(ex0->hyperlinkId != 0);
    const std::string* uri0 = t.term.hyperlinkURI(ex0->hyperlinkId);
    REQUIRE(uri0 != nullptr);
    CHECK(*uri0 == "https://example.com");

    // Cell 3 (last char of "link") should also have it
    const CellExtra* ex3 = t.term.grid().getExtra(3, 0);
    REQUIRE(ex3 != nullptr);
    CHECK(ex3->hyperlinkId != 0);

    // Cell 5 (inside " plain") should not
    const CellExtra* ex5 = t.term.grid().getExtra(5, 0);
    CHECK((ex5 == nullptr || ex5->hyperlinkId == 0));
}

TEST_CASE("OSC 8 getLinksFromRows: multiple links on same row")
{
    TestTerminal t;
    t.osc("8;;https://a.com");
    t.feed("AA");
    t.osc("8;;");
    t.feed(" ");
    t.osc("8;;https://b.com");
    t.feed("BB");
    t.osc("8;;");

    // Verify two distinct hyperlinks exist
    const CellExtra* exA = t.term.grid().getExtra(0, 0);
    const CellExtra* exB = t.term.grid().getExtra(3, 0);
    REQUIRE(exA != nullptr);
    REQUIRE(exB != nullptr);
    CHECK(exA->hyperlinkId != exB->hyperlinkId);

    const std::string* uriA = t.term.hyperlinkURI(exA->hyperlinkId);
    const std::string* uriB = t.term.hyperlinkURI(exB->hyperlinkId);
    REQUIRE(uriA != nullptr);
    REQUIRE(uriB != nullptr);
    CHECK(*uriA == "https://a.com");
    CHECK(*uriB == "https://b.com");

    // Link A spans cols 0-1, link B spans cols 3-4
    // Verify gap at col 2 has no link
    const CellExtra* exGap = t.term.grid().getExtra(2, 0);
    CHECK((exGap == nullptr || exGap->hyperlinkId == 0));
}

TEST_CASE("OSC 8 hyperlink spans multiple rows")
{
    TestTerminal t(10, 5); // narrow terminal
    t.osc("8;;https://long.com");
    t.feed("0123456789wrap"); // 14 chars on 10-col terminal wraps to row 2
    t.osc("8;;");

    // Row 0 cells should have the link
    const CellExtra* ex0 = t.term.grid().getExtra(0, 0);
    REQUIRE(ex0 != nullptr);
    CHECK(ex0->hyperlinkId != 0);

    // Row 1 (wrapped portion) should also have the link
    const CellExtra* ex1 = t.term.grid().getExtra(0, 1);
    REQUIRE(ex1 != nullptr);
    CHECK(ex1->hyperlinkId != 0);
    CHECK(ex0->hyperlinkId == ex1->hyperlinkId);
}

// ── OSC 99 (Desktop Notifications) ──────────────────────────────────────────

TEST_CASE("OSC 99 fires notification on d=1")
{
    TestTerminal t;
    t.osc("99;d=0:p=title;Hello World");
    CHECK(t.capturedNotifyTitle.empty()); // not fired yet
    t.osc("99;d=1:p=body;This is the body");
    CHECK(t.capturedNotifyTitle == "Hello World");
    CHECK(t.capturedNotifyBody == "This is the body");
}

TEST_CASE("OSC 99 does not fire without d=1")
{
    TestTerminal t;
    t.osc("99;d=0:p=title;Test");
    CHECK(t.capturedNotifyTitle.empty());
}

// kitty notifications.py: `done` defaults to True (line 250); only an explicit
// `d=0` keeps the notification buffered. A bare `99;;<text>` is the simplest
// valid form and must fire immediately.
TEST_CASE("OSC 99 default d=1: empty metadata fires title-only notification")
{
    TestTerminal t;
    t.osc("99;;Hello via OSC 99");
    CHECK(t.capturedNotifyTitle == "Hello via OSC 99");
    CHECK(t.capturedNotifyBody.empty());
}

// kitty notifications.py:285 — payload_type defaults to title when `p` is
// absent. Without this default the payload was dropped on the floor.
TEST_CASE("OSC 99 default p=title: payload becomes the title")
{
    TestTerminal t;
    t.osc("99;i=42;Just a title");
    CHECK(t.capturedNotifyTitle == "Just a title");
    CHECK(t.capturedNotifyId == "42");
}

TEST_CASE("OSC 99 explicit d=1 fires single chunk")
{
    TestTerminal t;
    t.osc("99;i=1:d=1;Done in one");
    CHECK(t.capturedNotifyTitle == "Done in one");
    CHECK(t.capturedNotifyId == "1");
}

TEST_CASE("OSC 99 chunked: title then body with same id")
{
    TestTerminal t;
    t.osc("99;i=7:d=0:p=title;The Title");
    CHECK(t.capturedNotifyTitle.empty());          // buffered
    t.osc("99;i=7:d=1:p=body;The Body");
    CHECK(t.capturedNotifyTitle == "The Title");
    CHECK(t.capturedNotifyBody == "The Body");
    CHECK(t.capturedNotifyId == "7");
}

TEST_CASE("OSC 99 firing clears buffered state")
{
    TestTerminal t;
    t.osc("99;i=1:d=1;First");
    CHECK(t.capturedNotifyTitle == "First");
    t.capturedNotifyTitle.clear();
    t.capturedNotifyBody.clear();
    t.capturedNotifyId.clear();
    // A second notification with no id should not inherit anything from the first.
    t.osc("99;;Second");
    CHECK(t.capturedNotifyTitle == "Second");
    CHECK(t.capturedNotifyBody.empty());
    CHECK(t.capturedNotifyId.empty());
}

// kitty notifications.py:132 — Urgency enum is Low=0, Normal=1, Critical=2.
// Default is Normal (line 438) when u= is not specified or is malformed.
TEST_CASE("OSC 99 urgency defaults to normal (1) without u=")
{
    TestTerminal t;
    t.osc("99;;default urgency");
    CHECK(t.capturedNotifyUrgency == 1);
}

TEST_CASE("OSC 99 u=0 sets urgency low")
{
    TestTerminal t;
    t.osc("99;u=0;low urgency");
    CHECK(t.capturedNotifyUrgency == 0);
}

TEST_CASE("OSC 99 u=2 sets urgency critical")
{
    TestTerminal t;
    t.osc("99;u=2;critical urgency");
    CHECK(t.capturedNotifyUrgency == 2);
}

// kitty silently ignores u= values that don't parse to {0,1,2}; we match that
// by leaving the previous urgency in place. Default for a fresh notification
// is still 1.
TEST_CASE("OSC 99 invalid u= is ignored (out-of-range single digit)")
{
    TestTerminal t;
    t.osc("99;u=3;ignored");
    CHECK(t.capturedNotifyUrgency == 1);
}

TEST_CASE("OSC 99 invalid u= is ignored (multi-character)")
{
    TestTerminal t;
    t.osc("99;u=12;ignored");
    CHECK(t.capturedNotifyUrgency == 1);
}

TEST_CASE("OSC 99 invalid u= is ignored (non-digit)")
{
    TestTerminal t;
    t.osc("99;u=x;ignored");
    CHECK(t.capturedNotifyUrgency == 1);
}

// Urgency, like the other accumulator fields, must reset to the default
// after a notification fires. Otherwise a critical notification followed by
// a default-urgency one would still be reported critical.
TEST_CASE("OSC 99 urgency resets to default after dispatch")
{
    TestTerminal t;
    t.osc("99;u=2;critical one");
    CHECK(t.capturedNotifyUrgency == 2);
    t.capturedNotifyUrgency = 1;  // observer reset; emulator state should also have reset
    t.osc("99;;normal one");
    CHECK(t.capturedNotifyUrgency == 1);
}

// Urgency carries across chunks within the same notification (same id, d=0
// then d=1). The u= value seen on any chunk wins (last write).
TEST_CASE("OSC 99 urgency carries across chunks")
{
    TestTerminal t;
    t.osc("99;i=7:d=0:u=2:p=title;Title");
    CHECK(t.capturedNotifyUrgency == 1);  // not fired yet, observer untouched
    t.osc("99;i=7:d=1:p=body;Body");
    CHECK(t.capturedNotifyUrgency == 2);
}

// Other notification entry points have no urgency channel and must default
// to normal (1).
TEST_CASE("OSC 9 notification has default urgency")
{
    TestTerminal t;
    t.osc("9;hello from osc9");
    CHECK(t.capturedNotifyTitle == "hello from osc9");
    CHECK(t.capturedNotifyUrgency == 1);
}

TEST_CASE("OSC 777 notification has default urgency")
{
    TestTerminal t;
    t.osc("777;notify;Title;Body");
    CHECK(t.capturedNotifyTitle == "Title");
    CHECK(t.capturedNotifyUrgency == 1);
}

// kitty notifications.py:301 — done = (v != "0"). Empty d= and any non-"0"
// value should fire; only an explicit d=0 buffers.
TEST_CASE("OSC 99 d= non-'0' values all fire")
{
    {
        TestTerminal t;
        t.osc("99;d=2;via d=2");
        CHECK(t.capturedNotifyTitle == "via d=2");
    }
    {
        TestTerminal t;
        t.osc("99;d=yes;via d=yes");
        CHECK(t.capturedNotifyTitle == "via d=yes");
    }
}

// ── OSC 99 c= (close-response request) ───────────────────────────────────────

TEST_CASE("OSC 99 c= absent: closeResponseRequested defaults to false")
{
    TestTerminal t;
    t.osc("99;;no c");
    CHECK(t.capturedNotifyCloseResponse == false);
}

TEST_CASE("OSC 99 c=1 sets closeResponseRequested")
{
    TestTerminal t;
    t.osc("99;c=1;wants close");
    CHECK(t.capturedNotifyCloseResponse == true);
}

TEST_CASE("OSC 99 c=0 explicitly sets closeResponseRequested false")
{
    TestTerminal t;
    t.osc("99;c=0;explicit no");
    CHECK(t.capturedNotifyCloseResponse == false);
}

TEST_CASE("OSC 99 c=1 carries across chunks")
{
    TestTerminal t;
    t.osc("99;i=7:d=0:c=1:p=title;Title");
    CHECK(t.capturedNotifyCloseResponse == false);  // not fired yet
    t.osc("99;i=7:d=1:p=body;Body");
    CHECK(t.capturedNotifyCloseResponse == true);
}

TEST_CASE("OSC 99 closeResponseRequested resets after dispatch")
{
    TestTerminal t;
    t.osc("99;c=1;first");
    CHECK(t.capturedNotifyCloseResponse == true);
    t.osc("99;;second");
    CHECK(t.capturedNotifyCloseResponse == false);
}

// ── OSC 99 p=close (programmatic dismissal) ──────────────────────────────────

TEST_CASE("OSC 99 p=close routes to onCloseNotification with the i= value")
{
    TestTerminal t;
    t.osc("99;i=42:p=close;");
    CHECK(t.closeNotificationCalls == 1);
    CHECK(t.capturedCloseId == "42");
    // Title/body accumulator must not have been touched.
    CHECK(t.capturedNotifyTitle.empty());
}

TEST_CASE("OSC 99 p=close without i= is dropped (no callback)")
{
    TestTerminal t;
    t.osc("99;p=close;");
    CHECK(t.closeNotificationCalls == 0);
}

TEST_CASE("OSC 99 p=close does not consume buffered title/body")
{
    TestTerminal t;
    // Buffer a title under id=7.
    t.osc("99;i=7:d=0:p=title;Buffered");
    CHECK(t.capturedNotifyTitle.empty());
    // Close a different id; should not fire the notification.
    t.osc("99;i=99:p=close;");
    CHECK(t.closeNotificationCalls == 1);
    CHECK(t.capturedCloseId == "99");
    CHECK(t.capturedNotifyTitle.empty());
    // Now finish the original.
    t.osc("99;i=7:d=1:p=body;Body");
    CHECK(t.capturedNotifyTitle == "Buffered");
}

// ── OSC 99 p=alive (existence query) ─────────────────────────────────────────

TEST_CASE("OSC 99 p=alive routes to onQueryAliveNotifications with the responder id")
{
    TestTerminal t;
    t.osc("99;i=q1:p=alive;");
    CHECK(t.queryAliveCalls == 1);
    CHECK(t.capturedAliveResponderId == "q1");
    CHECK(t.capturedNotifyTitle.empty());
}

TEST_CASE("OSC 99 p=alive without i= is dropped (no callback)")
{
    TestTerminal t;
    t.osc("99;p=alive;");
    CHECK(t.queryAliveCalls == 0);
}

// ── OSC 99 a= (action set) ───────────────────────────────────────────────────

// kitty notifications.py:232 — actions defaults to {focus}.
TEST_CASE("OSC 99 a= absent: actionFocus=true, actionReport=false (default)")
{
    TestTerminal t;
    t.osc("99;;default actions");
    CHECK(t.capturedNotifyActionFocus == true);
    CHECK(t.capturedNotifyActionReport == false);
}

TEST_CASE("OSC 99 a=focus,report sets both actions")
{
    TestTerminal t;
    t.osc("99;a=focus,report;both");
    CHECK(t.capturedNotifyActionFocus == true);
    CHECK(t.capturedNotifyActionReport == true);
}

TEST_CASE("OSC 99 a=-focus removes focus action")
{
    TestTerminal t;
    t.osc("99;a=-focus;no focus");
    CHECK(t.capturedNotifyActionFocus == false);
    CHECK(t.capturedNotifyActionReport == false);
}

TEST_CASE("OSC 99 a=+report keeps default focus and adds report")
{
    TestTerminal t;
    // +/- prefixes are deltas onto the default {focus}; without leading
    // bare token, focus stays.
    t.osc("99;a=+report;adds report");
    CHECK(t.capturedNotifyActionFocus == true);
    CHECK(t.capturedNotifyActionReport == true);
}

TEST_CASE("OSC 99 a= unknown tokens are silently ignored")
{
    TestTerminal t;
    t.osc("99;a=focus,report,bogus,nonsense;ignored extras");
    CHECK(t.capturedNotifyActionFocus == true);
    CHECK(t.capturedNotifyActionReport == true);
}

TEST_CASE("OSC 99 a= resets to default after dispatch")
{
    TestTerminal t;
    t.osc("99;a=report,-focus;first");
    CHECK(t.capturedNotifyActionFocus == false);
    CHECK(t.capturedNotifyActionReport == true);
    t.osc("99;;second");
    CHECK(t.capturedNotifyActionFocus == true);
    CHECK(t.capturedNotifyActionReport == false);
}

TEST_CASE("OSC 99 a= carries across chunks")
{
    TestTerminal t;
    t.osc("99;i=7:d=0:a=report,-focus:p=title;Title");
    CHECK(t.capturedNotifyActionFocus == true);  // not fired yet
    t.osc("99;i=7:d=1:p=body;Body");
    CHECK(t.capturedNotifyActionFocus == false);
    CHECK(t.capturedNotifyActionReport == true);
}

// ── OSC 99 p=buttons ─────────────────────────────────────────────────────────

// kitty notifications.py:421 — split on U+2028 (UTF-8: E2 80 A8).
TEST_CASE("OSC 99 p=buttons split on U+2028")
{
    TestTerminal t;
    // U+2028 is encoded as the 3-byte sequence 0xE2 0x80 0xA8.
    const char kU2028[] = "\xE2\x80\xA8";
    std::string payload = std::string("99;i=1:d=0:p=title;Title");
    t.osc(payload);
    payload = std::string("99;i=1:d=1:p=buttons;Open log") + kU2028 + "Retry" + kU2028 + "Ignore";
    t.osc(payload);
    REQUIRE(t.capturedNotifyButtons.size() == 3);
    CHECK(t.capturedNotifyButtons[0] == "Open log");
    CHECK(t.capturedNotifyButtons[1] == "Retry");
    CHECK(t.capturedNotifyButtons[2] == "Ignore");
}

TEST_CASE("OSC 99 p=buttons drops empty entries")
{
    TestTerminal t;
    const char kU2028[] = "\xE2\x80\xA8";
    std::string payload = std::string("99;p=buttons;A") + kU2028 + kU2028 + "B";
    t.osc(payload);
    REQUIRE(t.capturedNotifyButtons.size() == 2);
    CHECK(t.capturedNotifyButtons[0] == "A");
    CHECK(t.capturedNotifyButtons[1] == "B");
}

TEST_CASE("OSC 99 p=buttons caps at 8")
{
    TestTerminal t;
    const char kU2028[] = "\xE2\x80\xA8";
    std::string payload = "99;p=buttons;1";
    for (int i = 2; i <= 12; ++i) {
        payload += kU2028;
        payload += std::to_string(i);
    }
    t.osc(payload);
    CHECK(t.capturedNotifyButtons.size() == 8);
    CHECK(t.capturedNotifyButtons[0] == "1");
    CHECK(t.capturedNotifyButtons[7] == "8");
}

TEST_CASE("OSC 99 buttons reset across dispatch")
{
    TestTerminal t;
    const char kU2028[] = "\xE2\x80\xA8";
    std::string payload = std::string("99;p=buttons;A") + kU2028 + "B";
    t.osc(payload);
    REQUIRE(t.capturedNotifyButtons.size() == 2);
    t.capturedNotifyButtons.clear();
    t.osc("99;;next");
    CHECK(t.capturedNotifyButtons.empty());
}

// ── OSC 9 / 777 / 1337 notification forms ────────────────────────────────────
// OSC 9 is shared with ConEmu progress (handled separately above); the
// non-progress payload is treated as a title-only notification, matching
// Kitty's interpretation (kitty/notifications.py:1074). OSC 777 follows urxvt
// (optional leading "notify;", then title[;body]). OSC 1337 Notification= is
// iTerm2's KVP form, equivalent to a single-string title.

TEST_CASE("OSC 9 (non-progress) fires notification with payload as title")
{
    TestTerminal t;
    t.osc("9;Hello from OSC 9");
    CHECK(t.capturedNotifyTitle == "Hello from OSC 9");
    CHECK(t.capturedNotifyBody.empty());
    CHECK(t.capturedNotifyId.empty());
}

TEST_CASE("OSC 9 progress ('4;...' prefix) does NOT fire notification")
{
    TestTerminal t;
    t.osc("9;4;1;42");
    CHECK(t.capturedNotifyTitle.empty());
    CHECK(t.progressCallCount == 1);
}

TEST_CASE("OSC 777 splits 'title;body' on first semicolon")
{
    TestTerminal t;
    t.osc("777;notify;Build done;All tests passed");
    CHECK(t.capturedNotifyTitle == "Build done");
    CHECK(t.capturedNotifyBody == "All tests passed");
}

TEST_CASE("OSC 777 accepts payload without leading 'notify;'")
{
    TestTerminal t;
    t.osc("777;Build done;All tests passed");
    CHECK(t.capturedNotifyTitle == "Build done");
    CHECK(t.capturedNotifyBody == "All tests passed");
}

TEST_CASE("OSC 777 with title only (no body separator)")
{
    TestTerminal t;
    t.osc("777;notify;Just a title");
    CHECK(t.capturedNotifyTitle == "Just a title");
    CHECK(t.capturedNotifyBody.empty());
}

TEST_CASE("OSC 777 body may contain semicolons (only first separates)")
{
    TestTerminal t;
    t.osc("777;notify;Title;part one;part two");
    CHECK(t.capturedNotifyTitle == "Title");
    CHECK(t.capturedNotifyBody == "part one;part two");
}

TEST_CASE("OSC 1337 Notification= fires title-only notification")
{
    TestTerminal t;
    t.osc("1337;Notification=Hello via 1337");
    CHECK(t.capturedNotifyTitle == "Hello via 1337");
    CHECK(t.capturedNotifyBody.empty());
}

TEST_CASE("OSC 1337 non-Notification key does not fire notification")
{
    // OSC 1337 with File= (or any other key) goes through inline-image
    // handling, not the notification path. We only check that no notification
    // was emitted; the image path itself is exercised by test_osc_1337.
    TestTerminal t;
    t.osc("1337;SetMark");
    CHECK(t.capturedNotifyTitle.empty());
}

// === OSC 10/11/12 — default color query/set ===

TEST_CASE("OSC 10 query returns default foreground")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("10;?");
    // Default fg is #dddddd → rgb:dddd/dddd/dddd
    CHECK(t.output() == "\x1b]10;rgb:dddd/dddd/dddd\x1b\\");
}

TEST_CASE("OSC 11 query returns default background")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("11;?");
    // Default bg is #000000 → rgb:0000/0000/0000
    CHECK(t.output() == "\x1b]11;rgb:0000/0000/0000\x1b\\");
}

TEST_CASE("OSC 12 query returns cursor color")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("12;?");
    // Default cursor is #cccccc → rgb:cccc/cccc/cccc
    CHECK(t.output() == "\x1b]12;rgb:cccc/cccc/cccc\x1b\\");
}

TEST_CASE("OSC 10 set changes foreground")
{
    TestTerminal t;
    t.osc("10;#ff8800");
    t.clearOutput();
    t.osc("10;?");
    CHECK(t.output() == "\x1b]10;rgb:ffff/8888/0000\x1b\\");
}

TEST_CASE("OSC 11 set with rgb: format")
{
    TestTerminal t;
    t.osc("11;rgb:aa/bb/cc");
    t.clearOutput();
    t.osc("11;?");
    CHECK(t.output() == "\x1b]11;rgb:aaaa/bbbb/cccc\x1b\\");
}

TEST_CASE("OSC 10 set updates defaultColors struct")
{
    TestTerminal t;
    t.osc("10;#102030");
    auto dc = t.term.defaultColors();
    CHECK(dc.fgR == 0x10);
    CHECK(dc.fgG == 0x20);
    CHECK(dc.fgB == 0x30);
}

// ── OSC 22 — mouse pointer shape (kitty) ─────────────────────────────────────

TEST_CASE("OSC 22 set defaults to '=' op and updates current shape")
{
    TestTerminal t;
    t.osc("22;pointer");
    CHECK(t.term.currentPointerShape() == "pointer");
    CHECK(t.capturedPointerShape == "pointer");
    CHECK(t.pointerShapeCallCount == 1);
}

TEST_CASE("OSC 22 explicit '=' replaces top of stack")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.osc("22;=text");
    CHECK(t.term.currentPointerShape() == "text");
}

TEST_CASE("OSC 22 '>' pushes onto stack")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.osc("22;>text");
    CHECK(t.term.currentPointerShape() == "text");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "pointer");
}

TEST_CASE("OSC 22 '<' pops; popping empty stack is a no-op")
{
    TestTerminal t;
    t.osc("22;<");  // empty
    CHECK(t.term.currentPointerShape().empty());
    CHECK(t.pointerShapeCallCount == 0);
}

TEST_CASE("OSC 22 empty payload with '=' resets to default")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.osc("22;=");
    CHECK(t.term.currentPointerShape().empty());
}

TEST_CASE("OSC 22 '>' with comma list pushes each in order")
{
    TestTerminal t;
    t.osc("22;>pointer,text,wait");
    CHECK(t.term.currentPointerShape() == "wait");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "text");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "pointer");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape().empty());
}

TEST_CASE("OSC 22 '?' query: known/unknown CSS names")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("22;?pointer,bogus,text");
    CHECK(t.output() == "\x1b]22;1,0,1\x1b\\");
}

TEST_CASE("OSC 22 '?' __current__ returns current shape")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.clearOutput();
    t.osc("22;?__current__");
    CHECK(t.output() == "\x1b]22;pointer\x1b\\");
}

TEST_CASE("OSC 22 '?' __current__ on empty stack returns empty")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("22;?__current__");
    CHECK(t.output() == "\x1b]22;\x1b\\");
}

TEST_CASE("OSC 22 '?' __default__ returns 'default'")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("22;?__default__");
    CHECK(t.output() == "\x1b]22;default\x1b\\");
}

TEST_CASE("OSC 22 query does not mutate state")
{
    TestTerminal t;
    t.osc("22;pointer");
    int before = t.pointerShapeCallCount;
    t.osc("22;?text");
    CHECK(t.term.currentPointerShape() == "pointer");
    CHECK(t.pointerShapeCallCount == before);  // no callback for queries
}

TEST_CASE("OSC 22 push beyond stack limit drops oldest")
{
    TestTerminal t;
    // Push MAX + 1 entries; oldest should be dropped, current is the last pushed.
    for (int i = 0; i < 17; ++i) {
        t.osc(std::string("22;>shape") + std::to_string(i));
    }
    CHECK(t.term.currentPointerShape() == "shape16");
    // Pop 15 times; we should still see shape1 (shape0 was dropped).
    for (int i = 0; i < 15; ++i) t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "shape1");
}

TEST_CASE("OSC 22 main and alt screens have separate stacks")
{
    TestTerminal t;
    t.osc("22;pointer");                // main stack: [pointer]
    CHECK(t.term.currentPointerShape() == "pointer");
    t.csi("?1049h");                    // enter alt screen
    CHECK(t.term.currentPointerShape().empty());
    CHECK(t.capturedPointerShape.empty());  // toggle fires callback with new top
    t.osc("22;text");                   // alt stack: [text]
    CHECK(t.term.currentPointerShape() == "text");
    t.csi("?1049l");                    // back to main
    CHECK(t.term.currentPointerShape() == "pointer");
    CHECK(t.capturedPointerShape == "pointer");
}

TEST_CASE("OSC 22 RIS clears both stacks")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.csi("?1049h");
    t.osc("22;text");
    t.esc("c");                         // RIS (also exits alt screen)
    CHECK(t.term.currentPointerShape().empty());
    t.csi("?1049h");                    // alt again
    CHECK(t.term.currentPointerShape().empty());
}

TEST_CASE("isKnownPointerShape recognises CSS and X11 names")
{
    using TE = TerminalEmulator;
    CHECK(TE::isKnownPointerShape("pointer"));
    CHECK(TE::isKnownPointerShape("text"));
    CHECK(TE::isKnownPointerShape("nesw-resize"));
    CHECK(TE::isKnownPointerShape("hand2"));        // X11 alias
    CHECK(TE::isKnownPointerShape("sb_h_double_arrow"));
    CHECK_FALSE(TE::isKnownPointerShape("not-a-cursor-name"));
    CHECK_FALSE(TE::isKnownPointerShape(""));
}

// ── OSC 52 clipboard ──────────────────────────────────────────────────────────

TEST_CASE("OSC 52 c;<base64> writes to clipboard")
{
    TestTerminal t;
    t.osc("52;c;aGVsbG8=");          // base64("hello")
    CHECK(t.capturedClipboard == "hello");
}

TEST_CASE("OSC 52 c;? queries clipboard and responds with base64")
{
    TestTerminal t;
    t.clipboardContent = "world";
    t.clearOutput();
    t.osc("52;c;?");
    CHECK(t.output() == "\x1b]52;c;d29ybGQ=\x1b\\");
}

TEST_CASE("OSC 52 with empty data clears the clipboard")
{
    TestTerminal t;
    t.capturedClipboard = "previous";
    t.osc("52;c;");
    CHECK(t.capturedClipboard == "");
}

TEST_CASE("OSC 52 round-trip: write then query returns same content")
{
    TestTerminal t;
    t.osc("52;c;dGVzdGluZw==");      // base64("testing")
    t.clipboardContent = t.capturedClipboard;
    t.clearOutput();
    t.osc("52;c;?");
    CHECK(t.output() == "\x1b]52;c;dGVzdGluZw==\x1b\\");
}

TEST_CASE("OSC 52 p;<base64> routes to primary, leaves clipboard alone")
{
    TestTerminal t;
    t.osc("52;p;aGVsbG8=");          // base64("hello")
    CHECK(t.capturedPrimary   == "hello");
    CHECK(t.capturedClipboard == "");
}

TEST_CASE("OSC 52 s;<base64> aliases primary (X11 selection convention)")
{
    TestTerminal t;
    t.osc("52;s;aGVsbG8=");
    CHECK(t.capturedPrimary   == "hello");
    CHECK(t.capturedClipboard == "");
}

TEST_CASE("OSC 52 pc;<base64> sets both primary and clipboard")
{
    TestTerminal t;
    t.osc("52;pc;aGVsbG8=");
    CHECK(t.capturedClipboard == "hello");
    CHECK(t.capturedPrimary   == "hello");
}

TEST_CASE("OSC 52 with no recognized destination defaults to clipboard")
{
    TestTerminal t;
    t.osc("52;q;aGVsbG8=");          // q = secondary, ignored → fall back
    CHECK(t.capturedClipboard == "hello");
    CHECK(t.capturedPrimary   == "");

    TestTerminal t2;
    t2.osc("52;;aGVsbG8=");          // empty prefix
    CHECK(t2.capturedClipboard == "hello");
    CHECK(t2.capturedPrimary   == "");
}

TEST_CASE("OSC 52 p;? queries primary and echoes destination in response")
{
    TestTerminal t;
    t.primaryContent = "from-primary";
    t.clearOutput();
    t.osc("52;p;?");
    // Response prefix matches the requested destination.
    CHECK(t.output() == "\x1b]52;p;ZnJvbS1wcmltYXJ5\x1b\\");
}

TEST_CASE("OSC 52 pc;? prefers clipboard for read, echoes both in response")
{
    TestTerminal t;
    t.clipboardContent = "clip";
    t.primaryContent   = "prim";
    t.clearOutput();
    t.osc("52;pc;?");
    // Response carries clipboard content; prefix echoes both destinations
    // (clipboard first per parser order: c then p).
    CHECK(t.output() == "\x1b]52;cp;Y2xpcA==\x1b\\");
}

// ── OSC 9;4 kitty progress ────────────────────────────────────────────────────

TEST_CASE("OSC 9;4 reports progress state and percent")
{
    TestTerminal t;
    t.osc("9;4;1;42");
    CHECK(t.progressCallCount == 1);
    CHECK(t.capturedProgressState == 1);
    CHECK(t.capturedProgressPct == 42);
}

TEST_CASE("OSC 9;4 state 0 clears progress (percent optional)")
{
    TestTerminal t;
    t.osc("9;4;1;50");
    t.osc("9;4;0");
    CHECK(t.capturedProgressState == 0);
    CHECK(t.progressCallCount == 2);
}

TEST_CASE("OSC 9;4 states 2/3/4: error, indeterminate, pause")
{
    TestTerminal t;
    t.osc("9;4;2;25");
    CHECK(t.capturedProgressState == 2);
    t.osc("9;4;3");
    CHECK(t.capturedProgressState == 3);
    t.osc("9;4;4");
    CHECK(t.capturedProgressState == 4);
}

TEST_CASE("OSC 9 without ;4 does not fire progress callback")
{
    // The non-progress payload routes to the notification handler instead
    // (covered by the OSC 9 notification tests above); progress must stay
    // untouched so the two interpretations don't bleed into each other.
    TestTerminal t;
    t.osc("9;some other payload");
    CHECK(t.progressCallCount == 0);
    CHECK(t.capturedNotifyTitle == "some other payload");
}

// ── XTGETTCAP (DCS + q ... ST) ────────────────────────────────────────────────

TEST_CASE("XTGETTCAP returns 1+r for known capability (TN)")
{
    TestTerminal t;
    t.clearOutput();
    // Query "TN" (terminal name) — hex-encoded: "544E"
    t.dcs("+q544E");
    const std::string& out = t.output();
    REQUIRE(out.size() >= 9);
    CHECK(out.substr(0, 5) == "\x1bP1+r");
    CHECK(out.substr(5, 4) == "544E");
    CHECK(out.substr(out.size() - 2) == "\x1b\\");
}

TEST_CASE("XTGETTCAP returns 0+r for unknown capability")
{
    TestTerminal t;
    t.clearOutput();
    // "zzzz" — hex "7A7A7A7A"
    t.dcs("+q7A7A7A7A");
    const std::string& out = t.output();
    REQUIRE(out.size() >= 7);
    CHECK(out.substr(0, 5) == "\x1bP0+r");
    CHECK(out.substr(out.size() - 2) == "\x1b\\");
}

TEST_CASE("XTGETTCAP handles multiple capabilities separated by ;")
{
    TestTerminal t;
    t.clearOutput();
    t.dcs("+q544E;7A7A7A7A");
    const std::string& out = t.output();
    CHECK(out.find("\x1bP1+r") != std::string::npos);
    CHECK(out.find("\x1bP0+r") != std::string::npos);
}

// ── DSR 5 device status ───────────────────────────────────────────────────────

TEST_CASE("DSR 5 (device status) responds with OK")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("5n");
    CHECK(t.output() == "\x1b[0n");
}

// ── SGR 4:5 dashed underline ──────────────────────────────────────────────────

TEST_CASE("SGR 4:5 sets dashed underline style")
{
    TestTerminal t;
    t.csi("4:5m");
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
    // Only 2 bits of storage for style; dashed is aliased to dotted (3).
    CHECK(t.attrs(0, 0).underlineStyle() == 3);
}

// ── mode 2027 DECRQM always reports permanently set ───────────────────────────

TEST_CASE("DECRQM mode 2027 reports permanently set (pm=3)")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("?2027$p");
    CHECK(t.output() == "\x1b[?2027;3$y");
}
