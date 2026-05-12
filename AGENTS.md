# AGENTS.md

Operating rules for AI agents working on this repository.

## 1. Always format C/C++ before committing

Run `clang-format -i` on every C/C++ file you create or modify. The
canonical style is in `.clang-format` at the repo root (WebKit-derived,
`ColumnLimit: 0`, `IndentWidth: 4`, spaces only).

```sh
clang-format -i path/to/file.cpp path/to/other.h
```

For a full-tree pass:

```sh
find src tests -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \
    -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) -print0 \
    | xargs -0 -P 8 -I {} clang-format -i {}
```

`.mm` files are skipped — `.clang-format` is `Language: Cpp` only.

## 2. Pipe test output to a file; grep the file

Never grep the live process output. Tests can emit binary bytes, ANSI
escapes, or huge logs that confuse interactive piping. Always redirect
to disk first, then search.

```sh
rm -f /tmp/mb-tests.out
ASAN_OPTIONS=detect_leaks=0 ./build/bin/mb-tests > /tmp/mb-tests.out 2>&1
echo "exit=$?"
grep -a -E 'Status:|FAIL|test cases:' /tmp/mb-tests.out
```

A full pass looks like:

```
[doctest] test cases: 1015 | 1015 passed | 0 failed | 0 skipped
[doctest] assertions: 4690 | 4690 passed | 0 failed |
[doctest] Status: SUCCESS!
```

If `test cases: N | N passed | 0 failed` and the summary says `SUCCESS!`,
the run is green — even with a non-zero exit code from ASAN leak reports
that landed after doctest finished.

Notes:
- Use `grep -a` (treat as text) — the output contains binary bytes from
  terminal protocol tests and plain `grep` reports "binary file matches"
  with no content.
- Capture both stdout and stderr (`2>&1`).
- Always check the exit code separately — a doctest run that crashed
  mid-suite can still produce a "SUCCESS!" line earlier in the file.
- `rm -f` the output file before redirecting: zsh's `noclobber` setopt
  (in use here) makes `>` error out when the file already exists, so a
  second run will fail silently without it. Use `>!` if you prefer.
- `ASAN_OPTIONS=detect_leaks=0` keeps fontconfig / third-party leak
  reports from drowning the doctest summary in the tail of the file.
  These leaks are pre-existing and not from `mb` code; turning them off
  during test verification is fine. Re-enable if you're hunting an
  actual leak.

For targeted runs use the doctest filters:

```sh
./build/bin/mb-tests --test-case='*MergeKeyBindings*' > /tmp/run.out 2>&1
grep -a -E 'Status:|FAIL|assertions:' /tmp/run.out
```

`mb-tests` with no filter runs every doctest suite in the binary,
including the `[render]` suite (`tests/test_render.cpp`). That suite
exercises the full WebGPU pipeline — compute shader, indirect draw,
screenshot capture — and diffs against reference PNGs in
`tests/reference/`. Do **not** describe a no-filter run as
"non-GPU" or claim the compute / shader path is unexercised: it is.

To regenerate reference PNGs after an intentional visual change:

```sh
MB_UPDATE_REFS=1 ./build/bin/mb-tests "[render]" > /tmp/refs.out 2>&1
```

## 3. Keep `TODO.md` in sync

`TODO.md` at the repo root tracks open work and shipped features as
checkbox items (`- [ ]` / `- [x]`). When a change touches anything
listed there:

- Mark the item `- [x]` if your change completes it.
- Update the description if the scope shifted.
- Add a new item if the change introduces follow-up work.

Don't silently ship a feature listed there without updating its line.
The file is the source of truth for "what's done"; out-of-date entries
mislead the next reader (human or agent).

## 4. Keep `types/mb.d.ts` in sync with the JS API

`types/mb.d.ts` is the public TypeScript declaration for the `mb` global
and every JS-exposed object (`Pane`, `Popup`, `EmbeddedTerminal`, `Tab`,
`Layout`, etc.) plus the `mb:fs` / `mb:tui` / `mb:ws` modules. It is
authoritative for any TS applet that transpiles down to JS for
`mb.loadScript`.

Whenever you change the JS API surface in C++ — adding/removing/renaming
a binding in `src/script/ScriptEngine.cpp` or any of the
`Script*Module.cpp` files, changing argument shapes, adding new event
names, etc. — update `types/mb.d.ts` in the same change.

Specific triggers:

- New `JS_NewCFunction` registered on `mb` or any exposed object → add
  the method/property to the corresponding interface.
- New `addEventListener` event name → add a new `addEventListener` /
  `removeEventListener` overload.
- New `JS_SetModuleExport` in any `Script*Module.cpp` → add the export
  to the `declare module "mb:..."` block.
- New permission bit in `ScriptPermissions` → extend the `MbPermission`
  union.
- Arg shape change (e.g., positional → named-object) → update the
  function signature.

Verify by reading the diff of `types/mb.d.ts` against your C++ change
side-by-side. The file lives under `types/` and is the only `.d.ts` in
the repo.
