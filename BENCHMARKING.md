# Benchmarking

Deterministic throughput measurement for the VT parser + render pipeline.
Used as the pre/post baseline for the threading refactor (see
`RENDER_THREADING.md`).

## TL;DR

```bash
# 1. Build release.
cmake --build build-release --target mb -j8

# 2. Capture a fixture. Real VT input has CRLF line endings — a plain .txt
#    from shell redirection has bare LF, which causes line wrap without CR.
cat /usr/share/dict/words /usr/share/dict/words /usr/share/dict/words | \
    head -c 1048576 | sed 's/$/\r/' > benches/fixtures/plain-1MB-crlf.txt

# 3. Start mb in another shell (GUI mode is fine; must be running).
./build-release/bin/mb &

# 4. Measure. Use repeat=N to get stable numbers by amortizing overhead.
./build-release/bin/mb --ctl feed benches/fixtures/plain-1MB-crlf.txt 40
```

Sample output:

```json
{"type":"ok","id":0,"bytes":42680640,"bytes_per_iter":1067016,"repeat":40,
 "parse_us":12740027,"mb_per_sec":3.35}
```

`mb_per_sec` is pure VT parser throughput — wall time spent in
`TerminalEmulator::injectData()` divided by bytes processed. No subprocess
or IPC overhead; the timer wraps the call and the mapped pages are
pre-touched.

## Infrastructure

### Atomic counters (`src/Observability.h`)

| Counter | Where updated | Meaning |
|---|---|---|
| `bytes_parsed` | `TerminalEmulator::injectData` (every source path) | Monotonic total bytes given to the VT parser |
| `frames_presented` | `PlatformDawn::renderFrame` after Present | Monotonic total frames rendered |
| `last_parse_time_us` | `injectData` tail | `steady_clock` µs at end of most recent non-empty parse |
| `frames_at_last_parse` | `injectData` tail | Snapshot of `frames_presented` taken at the same moment |

All `std::atomic<uint64_t>` with relaxed ordering. Per-call cost is one
atomic add plus one store; undetectable.

Read via `mb --ctl stats` (in the `obs` block of the JSON response).

### IPC commands

#### `feed <path> [repeat]`

`mmap`s the file, pre-touches its pages to avoid first-iteration page-fault
skew, then calls `Terminal::injectData(mapped, size)` in a loop `repeat`
times. Returns when all iterations complete.

Response:
```json
{"type":"ok","id":0,
 "bytes": <total bytes across all iterations>,
 "bytes_per_iter": <single fixture size>,
 "repeat": <count>,
 "parse_us": <microseconds in injectData loop>,
 "mb_per_sec": <bytes / parse_us, which is bytes/µs == MB/s>}
```

The path is resolved to an absolute path client-side via `realpath(3)`
before sending. File must be a regular file; server rejects otherwise.

#### `wait-idle [timeout_ms [settle_ms]]`

Registers a 25 ms repeating poll on the event loop that returns when the
terminal is "idle." Idle = no parse has happened in the last `settle_ms`
(default 200 ms) AND a frame has been presented since the last parse.

Defaults: `timeout_ms = 10000`, `settle_ms = 200`.

Response:
```json
{"type":"wait-idle","id":0,
 "idle": true|false,
 "elapsed_us": <µs from wait-idle start to predicate firing>,
 "bytes_parsed": <counter value>,
 "frames_presented": <counter value>}
```

Note: `elapsed_us` is *only* the polling window on the server side. It is
not the parse time — `feed` already returns that. `wait-idle` is useful to
confirm "the frame reflecting those bytes has been drawn" before another
operation (e.g. before taking a screenshot).

#### `stats`

Unchanged from before, but now includes an `obs` block with the counters
above and a `now_us` convenience field.

## Throughput measurements

### Parser-only throughput (deterministic, CPU-bound)

The `feed` response is the authoritative source. Use `repeat=N` large
enough that `parse_us` is at least ~500 ms — below that, start-of-run
cache effects dominate.

```bash
# Good: ~12s of parse work, stable within noise
./build-release/bin/mb --ctl feed benches/fixtures/plain-1MB-crlf.txt 40

# Too short: overhead visible
./build-release/bin/mb --ctl feed benches/fixtures/plain-1MB-crlf.txt 1
```

Run 5 times, record median.

### End-to-end (parse + render) throughput

Pipeline feed + wait-idle in one shell invocation:

```bash
./build-release/bin/mb --ctl feed fixture.txt 40 && \
./build-release/bin/mb --ctl wait-idle
```

Total wall clock covers both. Today (single-threaded, pre-refactor), this
is approximately `feed.parse_us + wait-idle.elapsed_us` plus subprocess
startup (~20 ms on macOS). Post-refactor with a render thread, expect
the frame cost to overlap with parsing.

### Watching render cadence during a long feed

In one shell:
```bash
./build-release/bin/mb --ctl feed fixture.txt 100
```

In another:
```bash
while true; do
  ./build-release/bin/mb --ctl stats | \
    jq -c '.obs | {bytes_parsed, frames_presented}'
  sleep 0.2
done
```

Today you'll see `bytes_parsed` frozen at its start value until feed
completes (event loop is blocked), then it jumps to the full value.
`frames_presented` barely advances during the feed — typically 0-1
increments, all happening after feed returns.

After phase 3 of the threading refactor, `frames_presented` should climb
continuously during a long feed, and `bytes_parsed` should advance
periodically as `injectData` chunks complete.

This is the behavioral predicate that proves phase 3 landed correctly.

## Fixtures

Version-control fixture files under `benches/fixtures/`. Treat them as
frozen — never regenerate between benchmark runs, or your numbers become
incomparable.

### Line endings matter

VT input captured through a real PTY contains `\r\n` because the master
PTY's `ONLCR` termios flag translates `\n` → `\r\n`. When you feed a
plain text file directly to the parser, you see `\n` alone: cursor moves
down without returning to column 0, and lines visibly stair-step.

Fixtures are treated as binary in git (`benches/.gitattributes`) so the
CRLF bytes survive commits and checkouts on every platform. If you add
a new fixture and see a `CRLF will be replaced by LF` warning from git,
the attributes aren't taking effect — check the `.gitattributes` is
present and re-add the file after `git rm --cached <file>`.

Always produce CRLF fixtures:

```bash
# From a plain text source
sed 's/$/\r/' plain.txt > plain-crlf.txt

# Or capture via script(1) from a real PTY, which gives you CRLF + any
# escapes the producer emits naturally (SGR from colorized output, etc.)
script -q /tmp/raw.txt bash -c 'cat /path/to/sourcedata'
head -c 1048576 /tmp/raw.txt > fixture.vt
```

### Recommended fixture set

| File | Source | Size | Purpose |
|---|---|---|---|
| `plain-1MB-crlf.txt` | `man` concat / dict words, sed CRLF | ~1 MB | Pure text throughput |
| `find-usr.vt` | `find /usr` output via script(1) | ~1 MB | Short-line throughput |
| `vim-session.vt` | `script` of a vim editing session | ~500 KB | Mixed escapes |
| `cmatrix-5s.vt` | `script` of 5s of cmatrix | ~2 MB | Dense animation |
| `synth-colored-1MB.txt` | Python script (see below) | 1 MB | Deterministic SGR-heavy |
| `paste-256kb.txt` | `head -c 262144 dict/words` | 256 KB | Simulates bracketed-paste |

Synthetic SGR fixture for deterministic escape-heavy workload:

```bash
python3 -c "
import sys
for i in range(16384):
    sys.stdout.write(f'\x1b[38;5;{i%256}mLine {i}: the quick brown fox jumps over the lazy dog\r\n')
" > benches/fixtures/synth-colored-1MB.txt
```

## Recording a baseline

Before starting the threading refactor, capture a pre-refactor baseline.

```bash
mb &
sleep 2  # let it settle

for fixture in benches/fixtures/*.txt benches/fixtures/*.vt; do
    for run in 1 2 3 4 5; do
        out=$(./build-release/bin/mb --ctl feed "$fixture" 40)
        mbs=$(echo "$out" | jq .mb_per_sec)
        echo "$(date +%s),$fixture,$run,$mbs"
    done
done > benches/baseline-$(git rev-parse --short HEAD).csv
```

Check the CSV into `benches/` or a side note. After each refactor phase
lands, re-run the same script and compare. Any phase that regresses
median MB/s by more than ~5% is worth investigating before moving on.

## Interpreting the numbers

- **`parse_us` is pure CPU time in the VT parser.** It does not include
  rendering, event loop overhead, or IPC transport. If it regresses
  after a refactor phase, the culprit is somewhere in the parser path
  itself or a new lock held too long inside `injectData`.

- **`wait-idle.elapsed_us` is polling window, not work time.** A small
  number there means "by the time the client connected, the terminal
  was already idle." Don't confuse it with parse time.

- **`frames_presented` is a behavioral indicator.** Its rate during a
  feed tells you whether the render loop is making progress concurrently
  with parsing. Pre-refactor: near-zero. Post-refactor phase 3: should
  hit vsync cap.

- **Throughput drifts slightly downward at very large repeat counts**
  (observed: 3.59 → 3.35 MB/s from 1× to 40× on plain text). Linux
  perf profiling of repeat=100 identified the cause: ~33% of runtime is
  in `Document::growRing()` — scroll-into-scrollback is reallocating
  and memcpy-ing the ring repeatedly rather than amortizing growth.
  Fixing it is a separate project from the threading refactor; for
  benchmark stability, either do a warmup feed to pre-grow the ring, or
  compare baselines at a fixed repeat count. The baseline numbers
  include this cost; that's fine as long as post-refactor numbers are
  recorded the same way.

- **SGR-heavy input is faster than plain text, not slower.** Observed:
  synthetic-colored-1MB (~10% of bytes in `CSI ... m` sequences) runs at
  ~3.9 MB/s vs. ~3.45 MB/s for plain ASCII at the same repeat count.
  The escape state machine accumulates digits cheaply and commits style
  once per sequence; the cell-write path runs per printable byte and
  does more work (width lookup, grid write, style resolution, dirty
  flag, extras probe). Counter-intuitive vs. most terminals where
  CSI-heavy input is slower because they SIMD-vectorize the plain-text
  skip path and the escape path is the bottleneck. Here it's the
  opposite: plain text is the hot path because cell writes are
  expensive. Useful signal when prioritizing future optimization —
  batching dirty flags, reworking cell-extras layout, or eliminating
  per-cell virtual dispatch would help more than SIMD-parsing the VT
  state machine would.

- **Absolute throughput is ~100× below SOTA terminals.** Ghostty,
  Alacritty, and kitty sit in the hundreds of MB/s range on comparable
  hardware. MasterBandit at 3.5 MB/s is enough for any realistic
  interactive workload (a user typing is ~10 bytes/sec; a full-screen
  `ls` is a few KB) but visibly slower than competitors on pathological
  floods (`cat huge.log`, `yes`). The threading refactor does not
  close this gap — it addresses a different problem (the UI freezing
  *during* a flood, regardless of its total duration). If raw
  throughput becomes a goal, profile first, don't guess — the bottleneck
  is almost certainly in the per-cell write path given the "SGR-heavier
  = faster" result above, but the specific hot frame needs to be
  identified via `sample(1)` or Instruments.

## Known limitations

- `feed` bypasses the PTY, so it doesn't exercise PTY read coalescing,
  foreground-process detection, or the filter callback path. For those,
  run a real workload through a shell (`cat large.log` inside mb).
- The `obs` counters are process-global, not per-pane. If you benchmark
  with multiple panes open, you see the sum.
- `wait-idle`'s 25 ms poll cadence limits its resolution. Don't treat
  its `elapsed_us` as sub-25-ms accurate.
- Pre-refactor, `injectData` holds the event loop thread synchronously
  for the duration of the call. A feed of 100 MB with a slow fixture
  will freeze mb for tens of seconds. Expected; the refactor fixes this.
