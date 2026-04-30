# Parser Rewrite: Wezterm-Style Decode/Apply Split

Refactor `TerminalEmulator::injectData` so byte-level VT decoding produces
a `std::vector<Action>` **without holding the per-Terminal mutex**, and a
separate apply phase walks the action list **under the mutex** to mutate
the grid / document / mState fields.

## Contents

- [Why](#why)
- [What changes](#what-changes)
- [Code map](#code-map)
- [Action types](#action-types)
- [What stays the same](#what-stays-the-same)
- [Resolved decisions](#resolved-decisions)
- [What's tricky](#whats-tricky)
- [Step-by-step plan](#step-by-step-plan)
- [Validation](#validation)
- [Rollback plan](#rollback-plan)
- [References](#references)

## Why

Today the parse worker holds `mMutex` for the entire `injectData` call —
both decoding bytes and mutating cells happen under one lock. With a
64 KiB read-buffer cap and our measured ~3.5 MB/s plain-text throughput
(plus pathological dips to ~1.1 MB/s under sustained scrollback growth),
worst-case lock-hold per batch is around 50-200 ms. While the current
model is acceptable thanks to the bounded buffer + 3 ms outer coalesce,
any code path that needs `mMutex` (rendering, mouse hit-test, JS getters,
tab-bar title lookup, key dispatch into the focused pane) waits for the
full apply to complete.

Wezterm's pattern shrinks lock-hold to just the apply phase. For typical
input where parse and apply each take roughly equal time, that's
approximately a 2× reduction in worst-case main-thread / render-thread
stall. For OSC/DCS payloads with large bodies (kitty graphics base64
chunks, OSC 52 paste, DCS+q response queries), the win is much larger —
the entire payload accumulation runs under the lock today, and would
move outside under the new model.

## What changes

### Today

```cpp
size_t TerminalEmulator::injectData(const char* buf, size_t len_, size_t byteBudget)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);   // held throughout
    // ...
    for (int i = 0; i < len; ++i) {
        switch (mParserState) {
        case Normal:
            switch (buf[i]) {
            case '\r':  mState->cursorX = 0;        break;  // direct mutate
            case '\n':  lineFeed();                  break;  // direct mutate
            case 0x1b:  mParserState = InEscape;     break;
            default:    g.cell(x, y) = Cell{cp, attrs}; ... // direct mutate
            }
            break;
        case InEscape: ...      // accumulates into mEscapeBuffer, may dispatch processCSI
        case InStringSequence:  // accumulates into mStringSequence, dispatches processStringSequence
        case InUtf8: ...        // accumulates UTF-8 multi-byte
        }
    }
    // ...
    return len;
}
```

### Target

```cpp
// Lock-free phase: owns parser-state fields only. No grid/document/mState
// access. Emits an Action for every state-machine transition that mutates
// terminal state.
size_t parseToActions(const char* buf, size_t len, std::vector<Action>& out);

// Locked phase: walks the action list and mutates state. Calls the
// existing helpers (processCSI, processStringSequence, ...) which still
// touch grid/document/mState.
void applyActions(std::vector<Action>& actions);

size_t injectData(const char* buf, size_t len)
{
    std::vector<Action> actions;
    // No reserve() — eager Print→PrintString coalescing means a 64 KiB
    // ASCII flood produces ~1 action, not 64 K. Reserving len would
    // burn 2 MiB per call for nothing.
    parseToActions(buf, len, actions);   // lock-free

    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    applyActions(actions);
    return len;
}
```

The `byteBudget` parameter is gone — it was a leftover from the chunked
in-progress design. Bounding worst-case lock-hold under the new model
comes from the existing 64 KiB read-buffer cap (see
`Terminal::kReadBufferHigh`); the rewrite makes that cap directly bound
the apply duration, since parse no longer holds the lock.

**Verify before relying on this**: grep all callers of `queueParse` /
`injectData` to confirm none enqueue more than 64 KiB in a single call.
If any caller bypasses the cap, the apply phase loses its bound and the
rewrite needs to reintroduce chunking inside `injectData` itself.

## Code map

Relevant source files, with what each contains and what the rewrite touches:

| File | Function(s) | Touches in rewrite? |
|---|---|---|
| `src/terminal/TerminalEmulator.h` | All parser-state field declarations, `injectData` signature | Add `parseToActions` / `applyActions` decls. Drop `byteBudget` from `injectData`. No new mutex — parser-state is owned by the worker thread (see Resolved decisions). |
| `src/terminal/TerminalEmulator.cpp:743` | `injectData` (the for-loop state machine) | Split into `parseToActions` + `applyActions` |
| `src/terminal/TerminalEmulator.cpp:1267` | `processCSI` (reads `mEscapeBuffer` + `mEscapeIndex`) | Refactor to take `(buf, len, finalByte)` arguments |
| `src/terminal/TerminalEmulator.cpp:1907` | `onAction` (interprets parsed CSI Actions, mutates state) | Stays as-is, called from apply |
| `src/terminal/OSC.cpp:334` | `processStringSequence` (reads `mStringSequence`) | Refactor to take `std::string_view payload, uint8_t kind` |
| `src/terminal/OSC.cpp:752` | `processOSC_Title` | Stays as-is, called from `processStringSequence` |
| `src/terminal/OSC.cpp:766` | `processOSC_Clipboard` (calls `pasteFromClipboard` callback) | Stays as-is |
| `src/terminal/OSC.cpp:843` | `processOSC_Color` | Stays as-is |
| `src/terminal/OSC.cpp:895` | `processOSC_Palette` | Stays as-is |
| `src/terminal/OSC.cpp:951` | `processOSC_PaletteReset` | Stays as-is |
| `src/terminal/OSC.cpp:1011` | `processOSC_PointerShape` | Stays as-is |
| `src/terminal/OSC.cpp:130` | `processOSC_iTerm` | Stays as-is |
| `src/terminal/DCS.cpp:188` | `processDCS` (reads `mStringSequence`) | Refactor to take `std::string_view payload` |
| `src/terminal/SGR.cpp` | SGR attribute handling | Called from `onAction`, stays as-is |
| `src/terminal/KittyGraphics.cpp:252` | `processAPC` (kitty graphics protocol) | Stays as-is, called from `processStringSequence` for APC kind |
| `src/terminal/KittyGraphics.cpp:319-340` | `mKittyLoading` chunked-image state | See "What's tricky" — this state spans multiple PTY reads but lives in apply, no change |

## Action types

Wezterm's action enum (see `wezterm-escape-parser/src/lib.rs:42-62`)
fits in 32 bytes and covers the full VT vocabulary in 10 variants. Mirror
this granularity. Concretely, define:

```cpp
namespace ParserAction {

// Single visible cell. Coalesced into PrintString during emission (see
// Resolved Decisions: Print-run coalescing).
struct Print { char32_t cp; };

// Run of printable codepoints. Emitted by the coalescing append-helper;
// never emitted directly by the state machine.
struct PrintString { std::u32string cps; };

// C0/C1 controls handled directly by the apply (cursor moves, line feed,
// tab, etc.).
enum class ControlCode : uint8_t {
    BS,    // \b
    HT,    // \t
    LF,    // \n
    VT,    // \v
    FF,    // \f
    CR,    // \r
    BEL,   // \a
    SO,    // shift-out
    SI,    // shift-in
};
struct Control { ControlCode code; };

// ESC X (no parameters, no intermediates). E.g. ESC 7 (DECSC),
// ESC 8 (DECRC), ESC = (DECKPAM), ESC c (RIS), ESC D (IND), ESC E (NEL),
// ESC H (HTS), ESC M (RI).
struct EscSimple { char finalByte; };

// ESC ( X / ESC ) X — charset designation.
struct DesignateCharset { char slot; uint8_t charset; };

// CSI sequence. `buf` holds the parameter+intermediate bytes (excluding
// the leading \e[ and the final byte). bounded to 128 bytes by the
// existing escape-buffer limit. Boxed via std::unique_ptr inside the
// variant so sizeof(Action) stays at variant-overhead+ptr (~24B) rather
// than ~140B inline.
struct CSI {
    std::array<char, 128> buf;
    uint8_t len;
    char finalByte;
    bool isPrivate;   // mEscapeBuffer[1] == '?' was set
};

// String sequence (OSC / DCS / SOS / PM / APC). `payload` is moved out
// of mStringSequence at terminator time, so no copy. `kind` is the
// introducer byte (']' for OSC, 'P' for DCS, etc.) — same as
// mStringSequenceType today.
struct StringSequence {
    uint8_t kind;
    std::string payload;
};

// Mode 1049 alt-screen toggles, mode 2026 sync-output, RIS, etc. all go
// through onAction → apply state-machine. Encode them as CSI variants
// (the apply side dispatches via processCSI, which already handles
// these).

}  // namespace ParserAction

using Action = std::variant<
    ParserAction::Print,
    ParserAction::PrintString,
    ParserAction::Control,
    ParserAction::EscSimple,
    ParserAction::DesignateCharset,
    std::unique_ptr<ParserAction::CSI>,    // boxed: see Resolved decisions
    ParserAction::StringSequence>;
```

`CSI` is boxed via `std::unique_ptr` inside the variant. Inline at 132
bytes blows up the variant 4× and breaks the 32-byte size assertion.
The cost is one heap alloc per CSI; in plain-text workloads CSIs are a
small fraction of actions, and for SGR-heavy workloads the alloc is
overshadowed by the actual SGR processing in apply.

Print + Control + EscSimple cover ~99% of bytes in a typical workload.
The state machine emits one of these per ASCII byte. PrintString emerges
via the append-helper (see Resolved Decisions).

## What stays the same

- All `TerminalCallbacks` (`onTitleChanged`, `onProgressChanged`,
  `onForegroundProcessChanged`, etc.) fire from the apply phase, exactly
  like today. They already `eventLoop_->post` to main, so worker-thread
  firing remains safe.
- All existing helpers (`processCSI`, `processStringSequence`,
  `processOSC_*`, `processDCS`, `processAPC`, `dispatchSGR`, `onAction`)
  stay unchanged in their core logic — they're called from the apply
  phase. The only changes are: (1) they take their input as args
  instead of reading `mEscapeBuffer` / `mStringSequence` member fields,
  and (2) they no longer take `mMutex` themselves (apply already holds
  it).
- `mState` swapping (mode 1049, alt-screen alt-grid swap) stays in
  the apply phase.
- Selection state (`mSelection`), command ring (`mCommandRing`),
  hyperlink registry (`mHyperlinkRegistry`), image registry
  (`mImageRegistry`), kitty-loading state (`mKittyLoading`), title /
  icon stacks, scrollback document — all mutated only in the apply
  phase. Their lock discipline (under `mMutex`) is unchanged.
- The atomic shadows added during the threading work
  (`mTitleAtomic`, `mIconAtomic`, `mUsingAltScreenAtomic`,
  `mFocusedEmbeddedLineId`, `mEvictedHasItems`, `mParseInFlight`)
  remain. They're maintained from inside the apply phase — same writers
  as today.
- The graveyard pattern for Terminal lifetime — `mParseInFlight` already
  wraps the entire worker submit lambda, covering both parse and apply.
  No change.
- The 3 ms outer coalesce in `Terminal::queueParse` and the 64 KiB
  read-buffer cap. These bound the apply duration and remain the
  primary backpressure / coalescing knobs.

## Resolved decisions

These were initially listed as open questions, but wezterm's code gives
a clear answer for each. Implement these as specified; don't re-litigate.

### Action variant size: 32 bytes target, box heavy variants

Wezterm's `Action` enum is exactly **32 bytes** on 64-bit, asserted in a
test at `wezterm-escape-parser/src/lib.rs:90-98`:

```rust
#[cfg(all(test, target_pointer_width = "64"))]
#[test]
fn action_size() {
    assert_eq!(core::mem::size_of::<Action>(), 32);
    assert_eq!(core::mem::size_of::<DeviceControlMode>(), 16);
    assert_eq!(core::mem::size_of::<ControlCode>(), 1);
    assert_eq!(core::mem::size_of::<CSI>(), 32);
    assert_eq!(core::mem::size_of::<Esc>(), 4);
}
```

The shape (from `wezterm-escape-parser/src/lib.rs:42-62`):

```rust
pub enum Action {
    Print(char),                                       // 4 bytes
    PrintString(String),                               // ptr+len+cap = 24 bytes
    Control(ControlCode),                              // 1 byte
    DeviceControl(DeviceControlMode),                  // 16 bytes
    OperatingSystemCommand(Box<OperatingSystemCommand>),  // ptr = 8 bytes
    CSI(CSI),                                          // 32 bytes (inline)
    Esc(Esc),                                          // 4 bytes
    Sixel(Box<Sixel>),                                 // ptr
    XtGetTcap(Vec<String>),                            // ptr+len+cap
    KittyImage(Box<KittyImage>),                       // ptr
}
```

For our C++ `std::variant<...>`, target the same 32-byte size. Heavy
variants (long OSC payloads, CSI bigger than ~24 bytes) hold a
`std::unique_ptr<...>` or store the buffer inline if it fits. Plain
`Print(char32_t)`, `Control`, `EscSimple`, `DesignateCharset` stay
inline.

Add a static-assert similar to wezterm's test (target ≤32 bytes; with
boxed CSI the variant is ~24 bytes on 64-bit):

```cpp
static_assert(sizeof(Action) <= 32, "Action variant should fit in 32 bytes");
```

### Print run coalescing: do it eagerly via append helper

Wezterm coalesces consecutive `Print(c)` actions into `PrintString(s)`
during emission, not as a deferred optimization
(`wezterm-escape-parser/src/lib.rs:64-88`):

```rust
impl Action {
    pub fn append_to(self, dest: &mut Vec<Self>) {
        if let Action::Print(c) = &self {
            match dest.last_mut() {
                Some(Action::PrintString(s)) => { s.push(*c); return; }
                Some(Action::Print(prior))   => {
                    let mut s = prior.to_string();
                    dest.pop();
                    s.push(*c);
                    dest.push(Action::PrintString(s));
                    return;
                }
                _ => {}
            }
        }
        dest.push(self);
    }
}
```

The parse-buffered-data loop calls `action.append_to(&mut actions)`
(`mux/src/lib.rs:186`), so plain ASCII text becomes one
`Action::PrintString` covering the whole run, not thousands of individual
`Action::Print` entries.

C++ equivalent: a free helper `emit(actions, action)` that does the same
merge for `Print` and `PrintString`. ~10× fewer action entries for plain
ASCII flood; the `std::u32string::push_back` and `+=` paths are amortized
O(1).

### Apply error handling: spdlog is thread-safe

`processCSI` etc. may log errors ("Invalid CSI sequence", "Unknown
private mode N") from the worker thread. spdlog is thread-safe; keep
the existing log calls unchanged.

### Kitty graphics OSC payload: biggest single workload win

OSC 5113x sequences use the run-scan optimization in the current
`InStringSequence` branch (`TerminalEmulator.cpp:1219-1228`) — it scans
ahead for non-special bytes and bulk-appends to `mStringSequence`. Under
the rewrite, that scan runs lock-free in `parseToActions` and emits a
single `Action::StringSequence` once the terminator arrives. The entire
base64 payload accumulation moves out of `mMutex`. This is the largest
single workload-specific win the rewrite delivers; for non-graphics
workloads the win is incremental.

### CSI representation: boxed via std::unique_ptr

The raw CSI buffer is 128 bytes, and inlining it inside the variant
breaks the 32-byte size target by ~4×. Box via `std::unique_ptr<CSI>`
inside the variant; `sizeof(Action)` stays at variant-overhead + pointer
(~24 bytes on 64-bit). The cost is one heap alloc per CSI sequence,
which is dominated by the SGR processing in apply for any realistic
workload.

The alternative — parsing CSI into a structured semantic enum at
decode time (matching wezterm's `CSI::Cursor(…)` / `CSI::Sgr(…)` /
`CSI::Mode(…)`) — would let CSI fit in 32 bytes inline but requires
shredding `processCSI` into per-variant handlers. That's ~2× the work of
this rewrite and is explicitly out of scope here.

### createEmbedded: inline the \r\n effects, don't route through injectData

`createEmbedded` (main thread, holds `mMutex`) currently calls
`injectData("\r\n", 2)` to advance the parent cursor. Under the rewrite
this would force a main-thread synchronous entry into the parser while
a worker may be mid-`parseToActions` on the same Terminal — a
re-entrancy hazard.

Resolution: replace the call with the explicit mutations the `\r\n` was
producing — `mState->wrapPending = false`, `mState->cursorX = 0`,
`lineFeed()`, plus whatever `mDocument` continued-flag bookkeeping the
old path was doing. With this change, `parseToActions` is **only ever
called from the worker thread**, which is what makes the `mParseInFlight`
gating model sufficient (see next item).

Verify during implementation that the inlined mutations match the
current `\r\n` effects exactly; the doctest suite covers this via the
existing createEmbedded tests.

### DEC mode 2026 (synchronized output): parse-phase hold

Match wezterm's behavior. `parseToActions` keeps a local `hold` flag:

- On `CSI ?2026h`: flush prior actions to the apply phase, set
  `hold = true`. Don't return — keep accumulating into the same call's
  action vector across reads if needed.
- On `CSI ?2026l` or RIS or `CSI !p` (SoftReset): clear `hold`, flush
  the accumulated batch as one apply call.
- While `hold` is set: keep appending without flushing. Render thread
  sees one atomic frame.

This is real new behavior, not just a refactor — today the rewrite-naive
path applies actions per-byte and the render thread can see partial
frames. Adding the parse-phase hold gives tear-free TUI rendering.

The hold state lives in `parseToActions` and is reset on RIS like the
other parser-state fields.

### Parser-state concurrency: mParseInFlight gating only (no extra mutex)

Parser-state fields (`mParserState`, `mEscapeBuffer`, `mEscapeIndex`,
`mStringSequence`, `mUtf8Buffer`, `mUtf8Index`, `mGraphemeState`,
`mLastPrintedChar`, `mWasInStringSequence`, `mStringSequenceType`, plus
the new `hold` flag) are owned by whoever is currently inside
`parseToActions`. `mParseInFlight` ensures at most one entrant at a
time.

Combined with the createEmbedded inlining decision above, the worker
thread is the **only** thread that ever enters `parseToActions`. No
extra mutex is needed for parser-state fields — there's no concurrent
reader.

**TSAN gates the model**: a stress test that hammers `createEmbedded` +
key dispatch + tab switch from the main thread while a worker parse
runs on the same Terminal must report clean under TSAN. If TSAN reports
a race, fall back to the two-mutex design (mParseStateMutex around
parse, mMutex around apply, strict lock ordering).

This test runs **immediately after phase 5** wires up `injectData`,
before the regression-fix phase. Single-threaded doctests can't surface
parser-state races, so running 50 escape-sequence fixes before
validating the threading model means any race gets misattributed.

### Graveyard predicate: unchanged

Today's `parseInFlight()` predicate keeps a `Terminal` alive while a
worker is inside `injectData`. Since `mParseInFlight` wraps the entire
worker submit lambda — covering both parse and apply — the predicate
already covers the new model. No change.

## Open questions

None remain that need resolution before implementation. All prior
open questions (concurrency model, createEmbedded sync parse, graveyard
predicate) have been resolved above.

## What's tricky

### 1. Parser state must persist across `parseToActions` calls

PTY data arrives in fragments — a CSI sequence can be split across two
`read()` calls. The parser-state fields (`mParserState`,
`mEscapeBuffer`, `mEscapeIndex`, `mStringSequence`, `mUtf8Buffer`,
`mUtf8Index`, `mGraphemeState`, `mLastPrintedChar`,
`mWasInStringSequence`, `mStringSequenceType`) must be preserved between
calls. They're already member variables; under the split, they're owned
by `parseToActions` exclusively (not touched in apply).

Tests use `injectData` directly via `TestTerminal::feed`. Since tests
are single-threaded, ownership is whoever-holds-the-test-thread (no
concurrency). Production goes through the worker; ownership is the
worker thread (see "Resolved decisions: parser-state concurrency").

### 2. Synchronous query callbacks fired from inside parse handlers

`processOSC_Clipboard` calls `mCallbacks.pasteFromClipboard()` and writes
the result back to the PTY via `writeToOutput`. `processCSI` (for DA1,
DA2, DECRQM) and `processDCS` (XTGETTCAP) also write replies. These
already work in the threaded model because:

- The callbacks bounce through `runOnMain` (clipboard, isDarkMode,
  customTcap), so they're called on the worker thread but execute on
  main.
- `writeToOutput` queues bytes into `mWriteQueue` under `mMutex`.

In the new model, these all stay in the apply phase. `processCSI` etc.
still call `writeToOutput` while holding `mMutex`. No change.

### 3. mStringSequence accumulation across calls

A 1 MiB OSC 52 paste can arrive in multiple PTY reads. The current
parser appends bytes to `mStringSequence` as they arrive. Under the
split, `parseToActions` keeps appending; the action `StringSequence`
is only emitted when the BEL or ST terminator arrives. The action's
`payload` is **moved out of `mStringSequence`**, so no copy.

### 4. Cursor coordinate reads during parse

Some inline branches in the current state machine read
`mState->cursorX`, `mState->cursorY`, `mState->wrapPending` to make
decisions during decoding. For example, the `\r` case at
`TerminalEmulator.cpp:790-795` reads `mState->cursorY` to clear that
row's continued flag.

Under the rewrite, these reads happen only during apply. The state
machine emits `Action::Control{CR}`; apply does the
`mDocument.setRowContinued` work using the current `mState->cursorY`.
This is straightforward — just move the inline `\r` case body into
the apply visit branch.

### 5. The Print case writes mState fields too

```cpp
mState->cursorX++;
if (mState->cursorX >= mWidth) {
    mState->cursorX = mWidth - 1;
    if (mState->autoWrap) mState->wrapPending = true;
}
```

These cursor-advance operations depend on `mState->autoWrap`. They have
to happen during apply — the state machine emits `Action::Print{cp}`,
and apply handles the cursor advance + wrap + insertMode + extras.
Same logic as today, just moved to a visit branch.

This means for plain ASCII flood, the win is purely "no lock during
UTF-8 decoding and run-scanning" — small. The bigger wins are OSC/DCS
where bytes accumulate without per-byte mState reads.

**PrintString fast-path**: the naive apply branch
`for (char32_t cp : ps.cps) writePrintable(cp)` re-runs the per-cell
autoWrap / wrapPending / insertMode guards once per codepoint. For a
64 K ASCII run inside one PrintString, that's 64 K redundant guard
checks. Add a `writePrintableRun(std::u32string_view)` that reads
`mState->autoWrap` once, advances cursorX in bulk, and handles the wrap
boundary at the end. Measure before micro-optimizing further.

### 6. CSI parameter accumulation refactor

`processCSI` (`TerminalEmulator.cpp:1267`) currently reads
`mEscapeBuffer` + `mEscapeIndex` directly. Refactor to take buffer as
arguments:

```cpp
void processCSI(const char* buf, int len);
```

Same applies to `processStringSequence` and `processDCS`. They already
have a `string_view`-friendly internal structure — minimal change.

### 7. Eviction callbacks fire during apply

`Document::evictToArchive` fires the line-id eviction callback, which
moves an embedded Terminal onto `mEvictedEmbeddeds`. This runs from
inside the apply phase (the document mutation is in `writePrintable`,
in apply). No change.

### 8. Kitty graphics chunked-image state spans multiple PTY reads

`mKittyLoading` (`KittyGraphics.cpp:319-340`) is a state machine within
the parser that accumulates kitty-graphics image data across multiple
APC sequences. Each APC is one `Action::StringSequence{kind='_'}`; they
fire from `processStringSequence` → `processAPC`, all in apply.

The state lives across calls but is mutated only in apply. No change.
But note: `processAPC` reads `mKittyLoading.active` to decide whether
to fire `Update` events, and other code paths suppress redraw during
chunked load. Verify this still works after the split.

## Step-by-step plan

### Phase 1: Define the Action variant (1 day)

- Define `ParserAction::*` structs in
  `src/terminal/ParserAction.h` (new file).
- Define `using Action = std::variant<...>;`.
- Add `static_assert(sizeof(Action) == 32, ...)`.
- Define `void emit(std::vector<Action>& dest, Action action)` that does
  the Print → PrintString coalesce.

### Phase 2: Refactor existing helpers to take args (1 day)

- `processCSI()` → `processCSI(const char* buf, int len)`. Drop reads
  of `mEscapeBuffer`/`mEscapeIndex` member fields from inside.
- `processStringSequence()` → `processStringSequence(uint8_t kind,
  std::string_view payload)`. Drop reads of `mStringSequenceType` /
  `mStringSequence`.
- `processDCS()` → `processDCS(std::string_view payload)`.
- Audit all helpers and identify member-state reads vs.
  parser-buffer reads. Member-state (mState, grid, document, command
  ring, etc.) stays as-is; parser-buffer reads become arguments.
- Run all 797 tests after this phase. Should still pass — semantics
  unchanged.

### Phase 3: Implement `parseToActions` (2 days)

- Copy the entire for-loop body from `injectData` into
  `parseToActions`.
- Replace each direct mutation with an `emit(out, Action::...{ ... })`.
- Keep the parser-state machine (mParserState, mEscapeBuffer,
  mUtf8Buffer, mStringSequence, mGraphemeState, mLastPrintedChar)
  intact.
- Don't touch mState, grid, or document.
- Don't touch any callback (`mCallbacks.*`) — those fire from apply.
- Plain text → emit `Print` actions (auto-coalesced).
- Each control char → emit `Control{code}`.
- Each completed CSI → emit `CSI{buf, len, finalByte, isPrivate}`.
- Each completed string sequence (OSC/DCS/SOS/PM/APC) → emit
  `StringSequence{kind, std::move(mStringSequence)}`.
- ESC + simple final → emit `EscSimple{finalByte}`.
- ESC ( X / ESC ) X → emit `DesignateCharset{slot, charset}`.

### Phase 4: Implement `applyActions` (1 day)

- New method that takes `std::vector<Action>&` and dispatches via
  `std::visit` to the existing helpers.
- Each variant maps to one inline branch from the old code:

```cpp
void applyActions(std::vector<Action>& actions) {
    for (auto& a : actions) {
        std::visit(overloaded {
            [&](Print& p)               { writePrintable(p.cp); },
            [&](PrintString& ps)        { for (char32_t cp : ps.cps) writePrintable(cp); },
            [&](Control& c)             { applyControl(c.code); },
            [&](EscSimple& e)           { applyEsc(e.finalByte); },
            [&](DesignateCharset& d)    { applyDesignateCharset(d.slot, d.charset); },
            [&](CSI& csi)               { processCSI(csi.buf.data(), csi.len); },
            [&](StringSequence& ss)     {
                                          // dispatch by kind
                                          processStringSequence(ss.kind, ss.payload);
                                        },
        }, a);
    }
    // Update event dispatch logic — currently fires once per
    // injectData; preserve that behavior. See KittyGraphics.cpp
    // suppression for chunked load.
    if (!mKittyLoading.active && mCallbacks.event)
        mCallbacks.event(this, static_cast<int>(Update), nullptr);
}
```

- Pull the cursor-advance / wrap-pending logic from the current
  `Print` inline branch into `writePrintable(char32_t)`.
- Pull the line-feed / carriage-return / tab logic into helper
  functions (`applyControl`).

### Phase 5: Wire `injectData` to call parse then apply, inline createEmbedded (half day)

- New `injectData(buf, len)`:
  - Allocate `std::vector<Action> actions` (no `reserve` — coalescing
    makes it pointless).
  - Call `parseToActions(buf, len, actions)` — lock-free.
  - Acquire `mMutex`.
  - Call `applyActions(actions)`.
  - Release `mMutex`.
  - Return `len`.
- The byteBudget param: delete. The current chunking code in the worker
  is replaced by the natural batch boundary at `injectData`'s call (one
  apply per worker iteration).
- **Inline createEmbedded's `\r\n` effects** (see Resolved decisions).
  Replace the `injectData("\r\n", 2)` call with the explicit mutations
  it was producing. After this, `parseToActions` is only ever entered
  from the worker thread.

### Phase 5.5: TSAN stress test (1 day)

Before fixing escape-sequence regressions, validate the threading model.
Single-threaded doctests can't expose parser-state races; running
phase 6 first means any race gets misattributed to whichever escape
sequence happened to surface it.

- Build with TSAN (`-fsanitize=thread`).
- Stress test: 4 panes each fed by a `yes`-flood, plus main-thread
  hammers — `createEmbedded`, key dispatch, tab switching, mouse
  hit-test, scrollback queries — for at least 5 minutes.
- TSAN must report clean. If it reports a parser-state race, fall back
  to the two-mutex design (mParseStateMutex around parse, mMutex around
  apply) and re-run.

### Phase 6: Run the existing test suite, fix regressions (2-3 days)

- 797 tests. Each escape sequence is a regression risk.
- Specific risk areas:
  - CSI parsing edge cases (intermediate bytes, parameter overflow).
  - OSC payload accumulation across reads.
  - DCS handler.
  - Kitty graphics chunked transfer (`mKittyLoading.active` suppression).
  - Mode 1049 alt-screen swap.
  - RIS reset (clears parser state + screen + everything).
  - Title/icon stack push/pop atomic mirror updates.
  - ESC-in-InEscape cancel behavior (regression for the `Unknown
    escape sequence \033` bug — see test_screen.cpp).
- Strategy: run tests, fix one failure at a time. Don't merge a branch
  with any failing tests.

### Phase 7: Re-benchmark and update docs (half day)

- Re-run feed benchmarks with the same fixtures (`benches/fixtures/`).
- Compare `mb_per_sec` before and after.
- Compare worst-case lock-hold during a flood (timing instrumentation
  added in `Terminal::queueParse` and `applyPendingMutations`).
- Capture a kitty-graphics fixture (sample image base64-loaded via
  APC) and benchmark it specifically.
- Update `BENCHMARKING.md` with new numbers.
- Update `DESIGN.md` §17 to describe the parse/apply split.

### Total

Phase 1 (1d) + Phase 2 (1d) + Phase 3 (2d) + Phase 4 (1d) +
Phase 5 (0.5d) + Phase 5.5 TSAN (1d) + Phase 6 regressions (2-3d) +
Phase 7 benchmarks (0.5d) ≈ **9-11 days** of focused work. Plus 2-3
days of buffer for unexpected regressions = **about 2 weeks**.

## Validation

Beyond the existing test suite, the rewrite needs additional validation
because the changes are pervasive.

### Functional

- All 797 doctest cases pass (`./build/bin/mb-tests`). Each existing
  test exercises one or more escape sequences.
- Run the IPC `feed` benchmark with all fixtures in
  `benches/fixtures/`. Compare `mb --ctl screenshot` output before and
  after the rewrite — should be byte-for-byte identical PNG. Workflow:
  ```bash
  # Before rewrite (on the pre-rewrite branch):
  ./build-release/bin/mb --test --ipc &
  PID=$!
  sleep 1
  ./build-release/bin/mb --ctl feed benches/fixtures/synth-colored-1MB-crlf.txt 1
  ./build-release/bin/mb --ctl wait-idle
  ./build-release/bin/mb --ctl screenshot > /tmp/before.png
  kill $PID

  # After rewrite, repeat. Diff PNGs.
  cmp /tmp/before.png /tmp/after.png
  ```
- Run interactive shells (zsh, bash, fish) and TUIs (vim, neovim,
  htop, tmux, less, claude code) for at least an hour each. Regression
  signal: anything that looks weird on screen.
- Specific stress: `kitty +icat some-image.png` (kitty graphics);
  multi-pane with flood + selection in a different pane; OSC 52 paste
  from clipboard; OSC 99 desktop notifications.

### Performance

- `mb --ctl feed benches/fixtures/plain-1MB-crlf.txt 40` — measure
  `mb_per_sec`. Should stay roughly the same or improve slightly
  (parse-decode work didn't change; the win is reduced lock-hold,
  which `feed` doesn't measure since there's no concurrent reader).
- `mb --ctl feed benches/fixtures/synth-colored-1MB-crlf.txt 40` —
  same.
- Add a new fixture with kitty graphics base64 chunks. Expected: large
  improvement in MB/s here because the OSC payload scan moves out of
  the lock.
- Reproduce the user's `yes` flood + alt+1/alt+2 test. Compare
  `[TIMING]` log entries before and after. Expected: lock-hold
  durations drop by 30-50% for plain text; dramatically more for
  OSC/DCS workloads.

### Threading

- Build a TSAN config:
  ```bash
  cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
  ```
- Run the test suite under TSAN. Fix any reported races.
- Run the test suite under ASAN (existing config). Fix any leaks.
- Stress: 4 panes each running `yes`, plus user typing in a 5th pane,
  plus tab-switching. No deadlocks, no UAFs, no observable hitches.
  Run for 5 minutes minimum.

## Rollback plan

If the rewrite is merged and a regression is discovered:

- The pre-rewrite implementation lives at the parent commit. `git
  revert` the merge commit and rebuild — zero migration overhead.
- No file format / on-disk state has changed. Scrollback persistence
  (if any), config files, key bindings — all untouched.
- The atomic-shadow fields on `TerminalEmulator` (mTitleAtomic etc.)
  were added during the prior threading work; they survive the
  rewrite and the revert.

If the rewrite is **in-progress** and gets stuck:

- Don't merge until the full test suite + benchmarks + manual stress
  pass.
- The current bounded-buffer + 64 KiB cap model is acceptable per
  user feedback. If 80% through the rewrite something fundamental
  breaks, the fallback is "scrap the branch, keep the current model."

## References

### MasterBandit

- `BENCHMARKING.md` — current parser throughput numbers and methodology.
- `DESIGN.md` §17 — current async parsing architecture.
- `TODO.md` — `Document::growRing` flagged as orthogonal throughput
  bottleneck (~33% of parse time on long floods); fixing it would
  multiply this rewrite's win.

### wezterm (sha 577474d89ee61aef4a48145cdec82a638d874751)

These citations are pinned to the wezterm commit above. Re-verify
before consulting if the local checkout has moved.

#### `mux/src/lib.rs`

- `102-116` — `Mux` struct (RwLock<HashMap> for tabs/panes/...).
- `118` — `const BUFSIZE: usize = 1024 * 1024;` (PTY read buffer +
  socketpair SO_SNDBUF/SO_RCVBUF).
- `122-138` — `send_actions_to_mux`: takes `Vec<Action>`, calls
  `pane.perform_actions(actions)`, notifies `MuxNotification::PaneOutput`.
  Runs on the parse thread.
- `140-243` — `parse_buffered_data`: per-pane parse thread. Reads bytes
  from socketpair (line 150), parses into `Vec<Action>` via
  `termwiz::escape::parser::Parser::parse` (line 160), calls
  `send_actions_to_mux` to apply (line 224).
- `186` — `action.append_to(&mut actions)` — the eager Print-coalesce
  call site.
- `213` — the `poll()` call that implements the 3 ms coalesce window.
- `162-185` — DEC private mode 2026 (synchronized output) handling:
  when set, `hold = true` blocks flushing actions until the matching
  reset.
- `279-360` — `read_from_pane_pty`: per-pane PTY-reader thread that
  does only blocking `read()` and `tx.write_all()` to the socketpair.
  Does not parse.
- `449` — `main_thread_id` captured at Mux construction.
- `464` — `is_main_thread()` impl.

#### `mux/src/localpane.rs`

- `14` — `use parking_lot::{Mutex, MutexGuard, ...}` (plain Mutex, not
  RwLock).
- `124-139` — `LocalPane` struct.
- `126` — `terminal: Mutex<Terminal>` — the per-pane lock that
  serializes parser-side `perform_actions` against renderer-side
  `with_lines_mut` and main-thread input handlers.
- `167` — `cursor_position()` takes `terminal.lock()`.
- `206-208` — `with_lines_mut`: takes `terminal.lock()` and runs the
  closure. Used by the renderer.
- `390-392` — `LocalPane::perform_actions`: takes `terminal.lock()`,
  calls `Terminal::perform_actions(actions)`.
- `396, 408, 414, 424` — mouse / key / paste / resize handlers all
  take `terminal.lock()`.
- `1007` — `terminal: Mutex::new(terminal)` initialization in
  `LocalPane::new`.

#### `term/src/terminal.rs`

- `85-90` — `Terminal` struct: `state: TerminalState` + `parser: Parser`.
- `164-174` — `Terminal::advance_bytes`: parses bytes inline via
  `self.parser.parse(...)` and applies actions through a `Performer`.
  Used by tests; production goes through `perform_actions`.
- `176-185` — `Terminal::perform_actions`: takes
  `Vec<termwiz::escape::Action>`, increments seqno, runs each through
  `Performer::perform`. Caller must hold `Mutex<Terminal>` from
  LocalPane.

#### `wezterm-escape-parser/src/lib.rs`

- `42-62` — `Action` enum definition.
- `64-88` — `Action::append_to`: the Print-run coalescing helper.
- `90-98` — `action_size` test (asserts `sizeof(Action) == 32`).

#### `wezterm-gui/src/termwindow/render/pane.rs`

- `557-571` — `LineRender::with_lines_mut` impl: render path that
  holds `terminal.lock()` for the duration of glyph shaping + quad
  construction + `line_quad_cache.put`.
- `568` — `pos.pane.with_lines_mut(stable_range, &mut render)` — the
  call that triggers the render-side lock acquire.

#### `config/src/config.rs`

- `398-399` — `pub mux_output_parser_buffer_size: usize`.
- `411-412` — `pub mux_output_parser_coalesce_delay_ms: u64`.
- `1662-1664` — default coalesce delay = **3 ms**.
- `1666-1668` — default parser buffer size = **128 KiB**.

### kitty (sha 07f4d3c7ae17efdf17508c7fe564ae54eb347482)

Cited as an alternative model (parse on main thread, no concurrent
parser/renderer at all). Useful for comparison; not the design we're
adopting.

#### `kitty/vt-parser.c`

- `18` — `#define BUF_SZ (1024u*1024u)` — fixed 1 MiB ring buffer
  between the I/O thread and the main thread.
- `1499-1500` — `with_lock` / `end_with_lock` macros bracketing
  `pthread_mutex_lock(&self->lock)` / `pthread_mutex_unlock(&self->lock)`.
- `1502-1533` — `run_worker`: called on the main thread. Acquires
  `Parser->lock`, reads how many bytes are pending, **releases the
  lock** (line 1518) while calling `consume_input` (which mutates the
  Screen but is single-threaded — main thread is the only writer to
  Screen), re-acquires the lock to update read-buffer offsets (line 1520).
- `1512` — `input_delay` coalescing decision.

#### `kitty/child-monitor.c`

- `io_loop` and `read_bytes` (referenced in DESIGN.md §17 timing
  analysis; line numbers not re-verified here).
