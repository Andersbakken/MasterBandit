// Tests for Terminal::addOutputCapture / removeOutputCapture /
// deliverCapturedOutput, the on-disk PTY-output capture API used by
// pane.captureOutputToFile from the script layer.
//
// These tests exercise the Terminal layer directly (no script engine,
// no PTY) — we drive `deliverCapturedOutput` synchronously and inspect
// the written files. This keeps the tests fast and deterministic; the
// JS-binding integration is intentionally not covered here (mb-tests
// doesn't link the script engine).

#include <doctest/doctest.h>

#include "Terminal.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────
// Test fixture
// ─────────────────────────────────────────────────────────────────────

namespace {

// Headless Terminal that exposes the otherwise-protected capture API
// for tests. The capture API is declared on the public class but lives
// after a `protected:` block, so we re-export it here.
struct CaptureTestTerminal {
    std::unique_ptr<Terminal> term;
    fs::path tmpDir;

    // onCaptureStopped fires once per stop event. We accumulate them
    // for assertion. Synchronisation via a mutex because the auto-stop
    // path may fire from whichever thread did the failing write — in
    // these tests it's always the test thread (we drive
    // deliverCapturedOutput synchronously), but the contract allows
    // any thread.
    std::mutex stopMu;
    struct StopEvent {
        std::string path;
        Terminal::CaptureStopReason reason;
        std::string error;
    };
    std::vector<StopEvent> stops;

    CaptureTestTerminal() {
        // Per-test scratch dir under the system tmp. Created here,
        // removed in dtor — keeps the test artifacts together and
        // off the user's allowed-script-write paths.
        tmpDir = fs::temp_directory_path()
               / ("mb-cap-test-" + std::to_string(::getpid())
                  + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(tmpDir);

        PlatformCallbacks pcbs;
        TerminalCallbacks tcbs;
        term = std::make_unique<Terminal>(std::move(pcbs), std::move(tcbs));
        TerminalOptions opts;
        opts.scrollbackLines = 0;
        term->initHeadless(opts);
        term->resize(80, 24);

        term->onCaptureStopped =
            [this](const std::string& p,
                   Terminal::CaptureStopReason r,
                   const std::string& err) {
                std::lock_guard<std::mutex> lk(stopMu);
                stops.push_back({p, r, err});
            };
    }

    ~CaptureTestTerminal() {
        // Drop the Terminal first — its dtor closes any still-open
        // captures so the file handles release before we unlink.
        term.reset();
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
    }

    fs::path pathFor(const char* name) const { return tmpDir / name; }

    // Slurp a file as bytes. Empty string on missing file.
    static std::string readFile(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Snapshot stops vector. Returns a copy so subsequent mutations
    // don't invalidate iterators on caller assertions.
    std::vector<StopEvent> snapshotStops() {
        std::lock_guard<std::mutex> lk(stopMu);
        return stops;
    }
};

// Thin alias to keep the friend declaration we'd need otherwise out
// of Terminal.h: we only call addOutputCapture / removeOutputCapture
// / hasOutputCapture, which are public, plus deliverCapturedOutput
// which lives under `protected:`. To reach the protected method from
// tests we go through a tiny subclass that re-publishes it.
struct ExposedTerminal : Terminal {
    ExposedTerminal(PlatformCallbacks p, TerminalCallbacks t)
        : Terminal(std::move(p), std::move(t)) {}
    using Terminal::deliverCapturedOutput;
};

// Variant of CaptureTestTerminal that uses ExposedTerminal so tests
// can call deliverCapturedOutput directly.
struct CaptureTestExposed {
    std::unique_ptr<ExposedTerminal> term;
    fs::path tmpDir;

    std::mutex stopMu;
    struct StopEvent {
        std::string path;
        Terminal::CaptureStopReason reason;
        std::string error;
    };
    std::vector<StopEvent> stops;

    CaptureTestExposed() {
        tmpDir = fs::temp_directory_path()
               / ("mb-cap-test-" + std::to_string(::getpid())
                  + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(tmpDir);

        PlatformCallbacks pcbs;
        TerminalCallbacks tcbs;
        term = std::make_unique<ExposedTerminal>(std::move(pcbs), std::move(tcbs));
        TerminalOptions opts;
        opts.scrollbackLines = 0;
        term->initHeadless(opts);
        term->resize(80, 24);

        term->onCaptureStopped =
            [this](const std::string& p,
                   Terminal::CaptureStopReason r,
                   const std::string& err) {
                std::lock_guard<std::mutex> lk(stopMu);
                stops.push_back({p, r, err});
            };
    }

    ~CaptureTestExposed() {
        term.reset();
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
    }

    fs::path pathFor(const char* name) const { return tmpDir / name; }

    static std::string readFile(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::vector<StopEvent> snapshotStops() {
        std::lock_guard<std::mutex> lk(stopMu);
        return stops;
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Add / remove / has — basic registration semantics
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("Terminal::addOutputCapture: opens file and registers")
{
    CaptureTestExposed t;
    auto p = t.pathFor("raw.bin");

    std::string err;
    bool ok = t.term->addOutputCapture(p.string(),
                                        Terminal::CaptureFormat::Raw, &err);
    CHECK(ok);
    CHECK(err.empty());
    CHECK(t.term->hasOutputCapture(p.string()));
    CHECK(fs::exists(p));
    // Raw capture writes nothing until bytes arrive.
    CHECK(fs::file_size(p) == 0);
}

TEST_CASE("Terminal::addOutputCapture: rejects duplicate path")
{
    CaptureTestExposed t;
    auto p = t.pathFor("dup.bin");

    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));

    std::string err;
    bool ok = t.term->addOutputCapture(p.string(),
                                        Terminal::CaptureFormat::Raw, &err);
    CHECK_FALSE(ok);
    CHECK(err.find("already active") != std::string::npos);
}

TEST_CASE("Terminal::addOutputCapture: open failure surfaces error")
{
    CaptureTestExposed t;
    // Path under a non-existent directory — fopen("...") returns ENOENT.
    auto p = t.tmpDir / "missing-subdir" / "out.bin";

    std::string err;
    bool ok = t.term->addOutputCapture(p.string(),
                                        Terminal::CaptureFormat::Raw, &err);
    CHECK_FALSE(ok);
    CHECK(err.find("fopen failed") != std::string::npos);
    CHECK_FALSE(t.term->hasOutputCapture(p.string()));
}

TEST_CASE("Terminal::removeOutputCapture: closes and notifies once")
{
    CaptureTestExposed t;
    auto p = t.pathFor("explicit-stop.bin");

    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));

    bool removed = t.term->removeOutputCapture(p.string());
    CHECK(removed);
    CHECK_FALSE(t.term->hasOutputCapture(p.string()));

    auto stops = t.snapshotStops();
    REQUIRE(stops.size() == 1);
    CHECK(stops[0].path == p.string());
    CHECK(stops[0].reason == Terminal::CaptureStopReason::Explicit);
    CHECK(stops[0].error.empty());

    // Idempotent: removing again returns false and does NOT re-notify.
    bool again = t.term->removeOutputCapture(p.string());
    CHECK_FALSE(again);
    CHECK(t.snapshotStops().size() == 1);
}

// ─────────────────────────────────────────────────────────────────────
// Raw format — content fidelity
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("Raw capture: bytes written verbatim, including escapes and NULs")
{
    CaptureTestExposed t;
    auto p = t.pathFor("verbatim.bin");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));

    // ANSI SGR + plain text + a NUL + a UTF-8 high-byte run.
    const char chunk[] = "\x1b[31mhello\x1b[0m\x00\xe2\x9c\x93\n";
    constexpr size_t chunkLen = sizeof(chunk) - 1; // exclude terminator
    t.term->deliverCapturedOutput(chunk, chunkLen);

    // Force the capture closed so buffered fwrite flushes to disk
    // before we read the file.
    REQUIRE(t.term->removeOutputCapture(p.string()));

    std::string got = CaptureTestExposed::readFile(p);
    REQUIRE(got.size() == chunkLen);
    CHECK(std::memcmp(got.data(), chunk, chunkLen) == 0);
}

TEST_CASE("Raw capture: multiple deliveries concatenate")
{
    CaptureTestExposed t;
    auto p = t.pathFor("concat.bin");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));

    t.term->deliverCapturedOutput("foo", 3);
    t.term->deliverCapturedOutput("bar", 3);
    t.term->deliverCapturedOutput("baz", 3);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "foobarbaz");
}

TEST_CASE("Raw capture: empty delivery is a no-op")
{
    CaptureTestExposed t;
    auto p = t.pathFor("empty.bin");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));

    t.term->deliverCapturedOutput("", 0);
    t.term->deliverCapturedOutput(nullptr, 0);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(fs::file_size(p) == 0);
}

// ─────────────────────────────────────────────────────────────────────
// Asciicast v2 format
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("Asciicast capture: writes header on open")
{
    CaptureTestExposed t;
    auto p = t.pathFor("hdr.cast");
    REQUIRE(t.term->addOutputCapture(p.string(),
                                      Terminal::CaptureFormat::Asciicast));
    // Stop without delivering anything. File should contain only the header.
    REQUIRE(t.term->removeOutputCapture(p.string()));

    std::string got = CaptureTestExposed::readFile(p);
    // Header is one line of JSON ending in \n. We don't assert the
    // exact timestamp (system_clock-dependent) but every required
    // key must be present, and the line must end with newline.
    REQUIRE_FALSE(got.empty());
    CHECK(got.back() == '\n');
    CHECK(got.find("\"version\":2") != std::string::npos);
    CHECK(got.find("\"width\":80") != std::string::npos);
    CHECK(got.find("\"height\":24") != std::string::npos);
    CHECK(got.find("\"timestamp\":") != std::string::npos);
}

TEST_CASE("Asciicast capture: each delivery becomes one record")
{
    CaptureTestExposed t;
    auto p = t.pathFor("records.cast");
    REQUIRE(t.term->addOutputCapture(p.string(),
                                      Terminal::CaptureFormat::Asciicast));

    t.term->deliverCapturedOutput("hi", 2);
    // Sleep 10ms so the second record carries a measurably-larger
    // elapsed time than the first. We don't assert the exact value
    // (steady_clock granularity varies), just monotonic ordering.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    t.term->deliverCapturedOutput("there", 5);

    REQUIRE(t.term->removeOutputCapture(p.string()));

    std::string got = CaptureTestExposed::readFile(p);
    // Split on '\n'. Expect 3 lines (header + 2 records) plus a
    // trailing empty after the final newline.
    std::vector<std::string> lines;
    std::string cur;
    for (char c : got) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else            cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);

    REQUIRE(lines.size() == 3);
    // Record format: [<float>, "o", "<payload>"]
    // Sanity-check the JSON shape on each record line.
    CHECK(lines[1].front() == '[');
    CHECK(lines[1].back()  == ']');
    CHECK(lines[1].find(", \"o\", \"hi\"") != std::string::npos);
    CHECK(lines[2].find(", \"o\", \"there\"") != std::string::npos);

    // Elapsed time on record 2 must be >= record 1. Parse the
    // leading float by hand — pulling in a JSON dep for one number
    // is overkill.
    auto leadingFloat = [](const std::string& line) -> double {
        size_t comma = line.find(',');
        if (comma == std::string::npos) return -1.0;
        return std::stod(line.substr(1, comma - 1));
    };
    CHECK(leadingFloat(lines[2]) >= leadingFloat(lines[1]));
}

TEST_CASE("Asciicast capture: JSON-escapes special bytes")
{
    CaptureTestExposed t;
    auto p = t.pathFor("escapes.cast");
    REQUIRE(t.term->addOutputCapture(p.string(),
                                      Terminal::CaptureFormat::Asciicast));

    // Each of these bytes triggers a different branch in the encoder.
    // We send them as one chunk so they all live on a single record line.
    const char chunk[] = "\"\\\b\f\n\r\t\x01\x1b";
    constexpr size_t chunkLen = sizeof(chunk) - 1;
    t.term->deliverCapturedOutput(chunk, chunkLen);

    REQUIRE(t.term->removeOutputCapture(p.string()));

    std::string got = CaptureTestExposed::readFile(p);
    // Find the second line (header is first). It must contain the
    // expected escape sequences inside the payload string. We assert
    // each escape token appears; their order matches the input order
    // so a contiguous substring search is valid.
    auto nl = got.find('\n');
    REQUIRE(nl != std::string::npos);
    std::string record = got.substr(nl + 1);
    CHECK(record.find("\\\"")    != std::string::npos);
    CHECK(record.find("\\\\")    != std::string::npos);
    CHECK(record.find("\\b")     != std::string::npos);
    CHECK(record.find("\\f")     != std::string::npos);
    CHECK(record.find("\\n")     != std::string::npos);
    CHECK(record.find("\\r")     != std::string::npos);
    CHECK(record.find("\\t")     != std::string::npos);
    CHECK(record.find("\\u0001") != std::string::npos);
    CHECK(record.find("\\u001b") != std::string::npos);
}

TEST_CASE("Asciicast capture: UTF-8 high bytes pass through verbatim")
{
    CaptureTestExposed t;
    auto p = t.pathFor("utf8.cast");
    REQUIRE(t.term->addOutputCapture(p.string(),
                                      Terminal::CaptureFormat::Asciicast));

    // U+2713 CHECK MARK = E2 9C 93 in UTF-8.
    const char chunk[] = "\xe2\x9c\x93";
    t.term->deliverCapturedOutput(chunk, 3);

    REQUIRE(t.term->removeOutputCapture(p.string()));

    std::string got = CaptureTestExposed::readFile(p);
    auto nl = got.find('\n');
    REQUIRE(nl != std::string::npos);
    std::string record = got.substr(nl + 1);
    // High bytes must NOT have been \u-escaped — they should appear
    // raw inside the JSON string. Embedded UTF-8 bytes in JSON strings
    // are valid per RFC 8259 §8.1.
    CHECK(record.find("\xe2\x9c\x93") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────
// Text format — escape stripping
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("Text capture: writes nothing on open (no header)")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-empty.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));
    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(fs::file_size(p) == 0);
}

TEST_CASE("Text capture: passes printable ASCII through verbatim")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-printable.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    const char* msg = "the quick brown fox jumps over the lazy dog";
    t.term->deliverCapturedOutput(msg, std::strlen(msg));

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == msg);
}

TEST_CASE("Text capture: keeps LF and TAB, drops other C0 controls and DEL")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-c0.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // BEL (\a=0x07), BS (\b=0x08), VT (\v=0x0b), FF (\f=0x0c), DEL (0x7f)
    // are all dropped. \n and \t pass through.
    // \x7f is DEL. Adjacent string-literals concatenate at compile
    // time and the split lets the compiler stop including the next
    // character as part of the hex escape.
    const char data[] = "a\tb\n\a\b\v\f\x7f" "c";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "a\tb\nc");
}

TEST_CASE("Text capture: strips simple CSI sequences (SGR colours)")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-csi.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // \x1b[31m red \x1b[0m → " red "
    // \x1b[1;33;42m bold yellow on green → ""
    // \x1b[H \x1b[2J → cursor home + clear screen → "" (no payload)
    const char data[] =
        "\x1b[31mhello\x1b[0m world\n"
        "\x1b[1;33;42mbold\x1b[m\n"
        "\x1b[H\x1b[2Jclean\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) ==
          "hello world\nbold\nclean\n");
}

TEST_CASE("Text capture: strips OSC sequences with BEL terminator")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-osc-bel.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // OSC 0;new title\a → ""
    // OSC 8;;https://x\a link \x1b[0m → " link "
    const char data[] =
        "before"
        "\x1b]0;new title\a"
        "after\n"
        "\x1b]8;;https://example.com\alink\x1b]8;;\a\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) ==
          "beforeafter\nlink\n");
}

TEST_CASE("Text capture: strips OSC sequences with ESC \\ (ST) terminator")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-osc-st.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // OSC <body> ST, where ST is ESC \\.
    const char data[] =
        "x"
        "\x1b]52;c;abcd\x1b\\"
        "y\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "xy\n");
}

TEST_CASE("Text capture: strips DCS sequences (ESC P ... ESC \\)")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-dcs.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // DCS bodies often contain BEL — must NOT terminate on it (unlike OSC).
    const char data[] =
        "before"
        "\x1b" "Pq#0;2;100;0;0!~\a~~~\x1b\\" // sixel-ish gibberish with embedded BEL
        "after\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "beforeafter\n");
}

TEST_CASE("Text capture: strips SS3 single-byte sequences (ESC O ?)")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-ss3.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // ESC O P = F1 keypress; should drop the whole 3-byte sequence
    // and keep surrounding text.
    const char data[] = "a\x1b" "OPb\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "ab\n");
}

TEST_CASE("Text capture: strips two-byte ESC sequences (RIS, NEL, IND)")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-esc2.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // ESC c = RIS (reset). ESC E = NEL. ESC D = IND.
    const char data[] = "a\x1b" "cb\x1b" "Ec\x1b" "Dd\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "abcd\n");
}

TEST_CASE("Text capture: strips ESC <intermediate> <final> (charset designation)")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-esc-intermediate.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // ESC ( B = designate G0 = US-ASCII (3-byte sequence: ESC, '(', 'B')
    const char data[] = "a\x1b" "(Bb\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "ab\n");
}

TEST_CASE("Text capture: CRLF normalises to LF; bare CR drops")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-cr.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // "line1\r\nline2\r\n" → "line1\nline2\n"
    // "progress\rfinal\n" → "progressfinal\n" (bare \r drops; the
    //   characters before it stay since we don't model cursor moves)
    // trailing bare \r at chunk end stays in SeenCr state → next
    //   chunk's first byte is re-evaluated.
    const char data[] =
        "line1\r\n"
        "line2\r\n"
        "progress\rfinal\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) ==
          "line1\nline2\nprogressfinal\n");
}

TEST_CASE("Text capture: bare CR followed by ESC re-processes ESC correctly")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-cr-esc.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // "x\r" then "\x1b[31mhello\n" — the SeenCr → Ground transition
    // must let the ESC enter the Esc state cleanly. Result: "xhello\n".
    const char data[] = "x\r" "\x1b[31mhello\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "xhello\n");
}

TEST_CASE("Text capture: UTF-8 high bytes pass through")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-utf8.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // U+2713 CHECK MARK (E2 9C 93), U+1F600 GRINNING FACE (F0 9F 98 80)
    const char data[] = "\xe2\x9c\x93 \xf0\x9f\x98\x80\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "\xe2\x9c\x93 \xf0\x9f\x98\x80\n");
}

TEST_CASE("Text capture: parser state persists across chunk boundaries")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-multichunk.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // Split a CSI sequence across three chunks. The stripper must
    // NOT emit any of the bytes — the whole sequence is one logical
    // unit even though it arrived in pieces.
    t.term->deliverCapturedOutput("hello\x1b", 6); // ground + start of ESC
    t.term->deliverCapturedOutput("[3", 2);         // CSI + first param
    t.term->deliverCapturedOutput("1m", 2);         // last param + final
    t.term->deliverCapturedOutput("world\n", 6);    // payload after sequence

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "helloworld\n");
}

TEST_CASE("Text capture: OSC payload split across chunks doesn't leak")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-osc-split.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    t.term->deliverCapturedOutput("a\x1b]0;par", 7);   // OSC start + partial title
    t.term->deliverCapturedOutput("t of ti", 7);       // more OSC body
    t.term->deliverCapturedOutput("tle\ab\n", 6);      // BEL + post-payload

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "ab\n");
}

TEST_CASE("Text capture: CRLF split across chunks still produces single LF")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-crlf-split.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    t.term->deliverCapturedOutput("line\r", 5);
    t.term->deliverCapturedOutput("\nnext\n", 6);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "line\nnext\n");
}

TEST_CASE("Text capture: CR at end of chunk followed by non-LF on next chunk drops the CR")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-cr-then-other.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // "abc\r" then "def\n" — the bare \r at chunk-1 end must NOT
    // accidentally consume the 'd' (regression: an early version of
    // the state machine accidentally swallowed the next byte).
    t.term->deliverCapturedOutput("abc\r", 4);
    t.term->deliverCapturedOutput("def\n", 4);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) == "abcdef\n");
}

TEST_CASE("Text capture: realistic shell output sample")
{
    CaptureTestExposed t;
    auto p = t.pathFor("text-realistic.txt");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Text));

    // Approximation of `ls --color`-style output with a prompt.
    const char data[] =
        "\x1b]0;~/projects\a"               // OSC title
        "\x1b[?2004h"                        // CSI bracketed-paste-mode
        "\x1b[01;32muser@host\x1b[0m:"      // bold green prompt
        "\x1b[01;34m~/projects\x1b[0m$ "    // bold blue path
        "ls\r\n"
        "\x1b[01;34mDocuments\x1b[0m  "
        "\x1b[01;34mDownloads\x1b[0m  "
        "README.md\r\n";
    t.term->deliverCapturedOutput(data, sizeof(data) - 1);

    REQUIRE(t.term->removeOutputCapture(p.string()));
    CHECK(CaptureTestExposed::readFile(p) ==
          "user@host:~/projects$ ls\nDocuments  Downloads  README.md\n");
}

// ─────────────────────────────────────────────────────────────────────
// Multiple captures per terminal
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("Multiple captures: same delivery reaches all of them")
{
    CaptureTestExposed t;
    auto pa = t.pathFor("multi-a.bin");
    auto pb = t.pathFor("multi-b.bin");
    auto pc = t.pathFor("multi-c.cast");
    auto pd = t.pathFor("multi-d.txt");

    REQUIRE(t.term->addOutputCapture(pa.string(), Terminal::CaptureFormat::Raw));
    REQUIRE(t.term->addOutputCapture(pb.string(), Terminal::CaptureFormat::Raw));
    REQUIRE(t.term->addOutputCapture(pc.string(),
                                      Terminal::CaptureFormat::Asciicast));
    REQUIRE(t.term->addOutputCapture(pd.string(), Terminal::CaptureFormat::Text));

    // Deliberately include an escape so each format's distinct
    // handling is observable.
    const char chunk[] = "\x1b[31mpayload\x1b[0m";
    t.term->deliverCapturedOutput(chunk, sizeof(chunk) - 1);

    REQUIRE(t.term->removeOutputCapture(pa.string()));
    REQUIRE(t.term->removeOutputCapture(pb.string()));
    REQUIRE(t.term->removeOutputCapture(pc.string()));
    REQUIRE(t.term->removeOutputCapture(pd.string()));

    // Raw: bytes verbatim including escapes.
    CHECK(CaptureTestExposed::readFile(pa) ==
          std::string(chunk, sizeof(chunk) - 1));
    CHECK(CaptureTestExposed::readFile(pb) ==
          std::string(chunk, sizeof(chunk) - 1));
    // Asciicast: JSON-escaped record contains the payload wrapped
    // in the o-event shape. ESC bytes appear as \u001b in JSON.
    CHECK(CaptureTestExposed::readFile(pc).find("payload")
          != std::string::npos);
    // Text: just the payload, no escapes.
    CHECK(CaptureTestExposed::readFile(pd) == "payload");
}

TEST_CASE("Removing one capture leaves the others intact")
{
    CaptureTestExposed t;
    auto pa = t.pathFor("keep-a.bin");
    auto pb = t.pathFor("keep-b.bin");

    REQUIRE(t.term->addOutputCapture(pa.string(), Terminal::CaptureFormat::Raw));
    REQUIRE(t.term->addOutputCapture(pb.string(), Terminal::CaptureFormat::Raw));

    t.term->deliverCapturedOutput("first", 5);
    REQUIRE(t.term->removeOutputCapture(pa.string()));
    t.term->deliverCapturedOutput("second", 6);
    REQUIRE(t.term->removeOutputCapture(pb.string()));

    CHECK(CaptureTestExposed::readFile(pa) == "first");
    CHECK(CaptureTestExposed::readFile(pb) == "firstsecond");
}

// ─────────────────────────────────────────────────────────────────────
// Failure-mode auto-stop
// ─────────────────────────────────────────────────────────────────────

// Sanity: an explicit stop path doesn't accidentally surface as IoError.
TEST_CASE("Stop reason: explicit removeOutputCapture reports Explicit, not IoError")
{
    CaptureTestExposed t;
    auto p = t.pathFor("explicit-not-io.bin");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));
    t.term->deliverCapturedOutput("ok", 2);
    CHECK(t.snapshotStops().empty());
    REQUIRE(t.term->removeOutputCapture(p.string()));
    auto stops = t.snapshotStops();
    REQUIRE(stops.size() == 1);
    CHECK(stops[0].reason == Terminal::CaptureStopReason::Explicit);
    CHECK(stops[0].error.empty());
}

#ifdef __linux__
// /dev/full is a Linux-specific device that accepts open(O_WRONLY)
// but returns ENOSPC on every write. It's the standard way to test
// write-failure paths without filling the actual disk.
//
// Strategy: open the capture file normally so addOutputCapture
// succeeds; then dup2 the fd of /dev/full over the FILE*'s
// underlying fd. Subsequent fwrite calls will fail with ENOSPC and
// trigger the auto-stop path.
//
// We can't reach the FILE* from outside Terminal directly. The
// trick: the capture FILE* is the LAST one we opened on this
// process, and FILE*'s underlying fd is the highest fd we've got.
// More robust approach: pre-open /dev/full, then immediately after
// addOutputCapture (which opens our target file), redirect FD
// (highest) at /dev/full via dup2. We don't know the exact fd
// number; we walk up from STDERR_FILENO+1 looking for an open fd
// that points at our capture path.
TEST_CASE("Auto-stop on write failure: ENOSPC triggers stopped event"
          * doctest::skip(::access("/dev/full", W_OK) != 0))
{
    CaptureTestExposed t;
    auto p = t.pathFor("will-fail.bin");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));

    // Find the fd backing the capture FILE*. Walks /proc/self/fd
    // and matches by realpath — robust to any fd number the libc
    // happens to assign.
    int captureFd = -1;
    for (int fd = 0; fd < 4096; ++fd) {
        char link[256];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        char target[PATH_MAX];
        ssize_t n = ::readlink(link, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            if (p.string() == target) { captureFd = fd; break; }
        }
    }
    REQUIRE_MESSAGE(captureFd >= 0,
                    "could not locate fd backing capture FILE*; "
                    "test setup assumption violated");

    int devFull = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    REQUIRE(devFull >= 0);
    // Atomically replace captureFd's open file with /dev/full so
    // the FILE*'s buffered writes ultimately fail at the syscall.
    REQUIRE(::dup2(devFull, captureFd) == captureFd);
    ::close(devFull);

    // Trigger a write big enough to bypass libc buffering. BUFSIZ
    // is the libc default block size for FILE*; one full block
    // forces an fflush-equivalent and reaches the kernel.
    std::string big(BUFSIZ * 2, 'x');
    t.term->deliverCapturedOutput(big.data(), big.size());

    auto stops = t.snapshotStops();
    REQUIRE(stops.size() == 1);
    CHECK(stops[0].path == p.string());
    CHECK(stops[0].reason == Terminal::CaptureStopReason::IoError);
    CHECK(stops[0].error.find("write failed") != std::string::npos);
    // After auto-stop, the capture is unregistered as far as
    // hasOutputCapture is concerned (the entry is still in the
    // vector but marked stopped, which hasOutputCapture excludes).
    CHECK_FALSE(t.term->hasOutputCapture(p.string()));

    // A subsequent removeOutputCapture is still a structural remove
    // (the entry is in the vector); but it should NOT fire a second
    // stopped event because the entry is already stopped.
    bool removed = t.term->removeOutputCapture(p.string());
    CHECK(removed);
    CHECK(t.snapshotStops().size() == 1);
}
#endif // __linux__

// ─────────────────────────────────────────────────────────────────────
// Lifetime: dtor closes still-open captures without notifying
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("Terminal destructor closes captures silently (no stopped event)")
{
    // Use a directory we control directly (not the fixture's tmpDir,
    // which is removed by ~CaptureTestExposed) so we can read the
    // capture file AFTER the Terminal is destroyed. Otherwise the
    // dtor's fclose flushes buffered bytes to a path that has
    // already been unlinked, and we can't observe the flush.
    fs::path persistent =
        fs::temp_directory_path()
        / ("mb-cap-dtor-" + std::to_string(::getpid()));
    fs::create_directories(persistent);
    fs::path p = persistent / "teardown.bin";

    std::vector<CaptureTestExposed::StopEvent> stops;
    {
        CaptureTestExposed t;
        // Override the capture path to live OUTSIDE t.tmpDir.
        REQUIRE(t.term->addOutputCapture(p.string(),
                                          Terminal::CaptureFormat::Raw));
        t.term->deliverCapturedOutput("data", 4);
        stops = t.snapshotStops();
        // Don't read the file here — the FILE* is still open with
        // buffered bytes; only the dtor's fclose guarantees the
        // flush. We assert post-dtor below.
    }
    // Dtor must NOT have fired the stopped callback (the engine that
    // subscribed is also being torn down in production).
    CHECK(stops.empty());
    // Dtor's fclose flushes buffered bytes to disk.
    CHECK(CaptureTestExposed::readFile(p) == "data");

    // Cleanup: tmpDir removal in the fixture only handles paths under
    // tmpDir; this file lives in our own scratch dir.
    std::error_code ec;
    fs::remove_all(persistent, ec);
}

// ─────────────────────────────────────────────────────────────────────
// Concurrent producer (smoke)
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("Concurrent deliverCapturedOutput from multiple threads serialises writes")
{
    CaptureTestExposed t;
    auto p = t.pathFor("concurrent.bin");
    REQUIRE(t.term->addOutputCapture(p.string(), Terminal::CaptureFormat::Raw));

    // Two writer threads each write distinct single-byte chunks
    // many times. Per-capture mutex must serialise so neither byte
    // stream interleaves *within* a single fwrite call. Per-byte
    // ordering across threads is undefined, but each chunk must
    // appear atomically.
    constexpr int kPerThread = 1000;
    auto writer = [&](char byte) {
        for (int i = 0; i < kPerThread; ++i) {
            t.term->deliverCapturedOutput(&byte, 1);
        }
    };
    std::thread t1(writer, 'A');
    std::thread t2(writer, 'B');
    t1.join();
    t2.join();

    REQUIRE(t.term->removeOutputCapture(p.string()));
    std::string got = CaptureTestExposed::readFile(p);
    REQUIRE(got.size() == 2 * kPerThread);
    int countA = 0, countB = 0;
    for (char c : got) {
        if (c == 'A') ++countA;
        else if (c == 'B') ++countB;
        else FAIL("unexpected byte in capture: ", static_cast<int>(c));
    }
    CHECK(countA == kPerThread);
    CHECK(countB == kPerThread);
}
