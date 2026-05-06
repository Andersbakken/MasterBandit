// Tests for Script::parsePermissions / permissionsToString — the
// string ↔ bitmask round-trips that scripts and the allowlist file
// rely on. Also covers the recently-added bits (PaneRead,
// ProcessSpawn) so a careless rename of either name table desyncs
// the round-trip and the test fails loudly.
//
// The functions live in ScriptPermissions.cpp which has no QuickJS or
// Engine deps; we link the TU directly into mb-tests (see
// tests/CMakeLists.txt).

#include <doctest/doctest.h>

#include "ScriptPermissions.h"

#include <string>

using Script::Perm;
using Script::parsePermissions;
using Script::permissionsToString;

// ─────────────────────────────────────────────────────────────────────
// Single-bit names
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("parsePermissions: empty string is None")
{
    CHECK(parsePermissions("") == 0u);
}

TEST_CASE("parsePermissions: unknown name yields 0 (and logs warn)")
{
    CHECK(parsePermissions("nope.does.not.exist") == 0u);
}

TEST_CASE("parsePermissions: every individual bit name resolves to its bit")
{
    // Names must match ScriptPermissions.cpp::kPermNames exactly.
    // Adding a bit and forgetting to register it here is a bug that
    // a TS-side d.ts update would otherwise silently mask.
    CHECK(parsePermissions("ui.popup.create")  == Perm::UiPopupCreate);
    CHECK(parsePermissions("ui.popup.destroy") == Perm::UiPopupDestroy);
    CHECK(parsePermissions("ui.focus")         == Perm::UiFocus);
    CHECK(parsePermissions("io.filter.input")  == Perm::IoFilterInput);
    CHECK(parsePermissions("io.filter.output") == Perm::IoFilterOutput);
    CHECK(parsePermissions("io.inject")        == Perm::IoInject);
    CHECK(parsePermissions("shell.write")      == Perm::ShellWrite);
    CHECK(parsePermissions("shell.commands")   == Perm::ShellReadCommands);
    CHECK(parsePermissions("actions.invoke")   == Perm::ActionsInvoke);
    CHECK(parsePermissions("tabs.create")      == Perm::TabsCreate);
    CHECK(parsePermissions("tabs.close")       == Perm::TabsClose);
    CHECK(parsePermissions("scripts.load")     == Perm::ScriptsLoad);
    CHECK(parsePermissions("scripts.unload")   == Perm::ScriptsUnload);
    CHECK(parsePermissions("fs.read")          == Perm::FsRead);
    CHECK(parsePermissions("fs.write")         == Perm::FsWrite);
    CHECK(parsePermissions("net.listen.local") == Perm::NetListenLocal);
    CHECK(parsePermissions("clipboard.read")   == Perm::ClipboardRead);
    CHECK(parsePermissions("clipboard.write")  == Perm::ClipboardWrite);
    CHECK(parsePermissions("pane.read")        == Perm::PaneRead);
    CHECK(parsePermissions("layout.modify")    == Perm::LayoutModify);
    CHECK(parsePermissions("config.modify")    == Perm::ConfigModify);
    CHECK(parsePermissions("process.spawn")    == Perm::ProcessSpawn);
}

TEST_CASE("parsePermissions: group names expand to all member bits")
{
    CHECK(parsePermissions("ui")        == Perm::GroupUi);
    CHECK(parsePermissions("io")        == Perm::GroupIo);
    CHECK(parsePermissions("shell")     == Perm::GroupShell);
    CHECK(parsePermissions("actions")   == Perm::GroupActions);
    CHECK(parsePermissions("tabs")      == Perm::GroupTabs);
    CHECK(parsePermissions("scripts")   == Perm::GroupScripts);
    CHECK(parsePermissions("fs")        == Perm::GroupFs);
    CHECK(parsePermissions("net")       == Perm::GroupNet);
    CHECK(parsePermissions("clipboard") == Perm::GroupClipboard);
    CHECK(parsePermissions("layout")    == Perm::GroupLayout);
    CHECK(parsePermissions("config")    == Perm::GroupConfig);
    CHECK(parsePermissions("process")   == Perm::GroupProcess);
}

// ─────────────────────────────────────────────────────────────────────
// Comma-separated lists + whitespace tolerance
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("parsePermissions: comma-joined names OR together")
{
    uint32_t got = parsePermissions("shell,fs.write,process.spawn");
    CHECK((got & Perm::GroupShell)   == Perm::GroupShell);
    CHECK((got & Perm::FsWrite)      == Perm::FsWrite);
    CHECK((got & Perm::ProcessSpawn) == Perm::ProcessSpawn);
    // No other bits should leak in.
    CHECK((got & ~(Perm::GroupShell | Perm::FsWrite | Perm::ProcessSpawn)) == 0u);
}

TEST_CASE("parsePermissions: leading/trailing whitespace is stripped per token")
{
    CHECK(parsePermissions(" ui , fs.read ")
          == (Perm::GroupUi | Perm::FsRead));
}

TEST_CASE("parsePermissions: empty tokens (`,,`) are tolerated")
{
    // A stray double-comma in a hand-edited TOML shouldn't poison the
    // parse — empty tokens are silently skipped.
    CHECK(parsePermissions("ui,,fs.read") == (Perm::GroupUi | Perm::FsRead));
}

TEST_CASE("parsePermissions: unknown tokens in a list don't poison the rest")
{
    // The known names still resolve; the unknown ones contribute 0.
    uint32_t got = parsePermissions("fs.read,nope,ui");
    CHECK((got & Perm::FsRead)  == Perm::FsRead);
    CHECK((got & Perm::GroupUi) == Perm::GroupUi);
}

// ─────────────────────────────────────────────────────────────────────
// Round-trip through permissionsToString
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("permissionsToString: None and All have stable special-case strings")
{
    CHECK(permissionsToString(Perm::None) == "none");
    CHECK(permissionsToString(Perm::All)  == "all");
    CHECK(parsePermissions("none") == 0u); // "none" is not in the table; round-trip
                                            // through "" form covers the round-trip itself.
}

TEST_CASE("permissionsToString: prefers group names when fully covered")
{
    // ui group expansion -> "ui" (not "ui.popup.create,ui.popup.destroy").
    // GroupUi is exactly UiPopupCreate | UiPopupDestroy, so the formatter
    // picks the group when both bits are present.
    CHECK(permissionsToString(Perm::GroupUi) == "ui");
}

TEST_CASE("permissionsToString: emits leftover individual bits after groups")
{
    // shell group + ui.focus (NOT in any group) → "shell,ui.focus".
    uint32_t bits = Perm::GroupShell | Perm::UiFocus;
    std::string s = permissionsToString(bits);
    CHECK(s.find("shell") != std::string::npos);
    CHECK(s.find("ui.focus") != std::string::npos);
    // No "shell.write" / "shell.commands" leakage.
    CHECK(s.find("shell.write") == std::string::npos);
    CHECK(s.find("shell.commands") == std::string::npos);
}

TEST_CASE("Round-trip: parse → format → parse yields the same bits")
{
    // Pairs of (input string, expected re-parsed bits). The
    // intermediate format string varies (groups vs individual bits)
    // but the final bits must be invariant.
    struct Case {
        const char* input;
        uint32_t bits;
    };
    Case cases[] = {
        { "fs.read",                          Perm::FsRead },
        { "fs",                               Perm::GroupFs },
        { "shell,clipboard",                  Perm::GroupShell | Perm::GroupClipboard },
        { "process.spawn,pane.read",          Perm::ProcessSpawn | Perm::PaneRead },
        { "ui.focus,layout.modify",           Perm::UiFocus | Perm::LayoutModify },
        { "process,fs",                       Perm::GroupProcess | Perm::GroupFs },
    };
    for (const auto& c : cases) {
        uint32_t parsed1 = parsePermissions(c.input);
        REQUIRE_MESSAGE(parsed1 == c.bits,
                        "input='", c.input, "' bits mismatch");
        std::string formatted = permissionsToString(parsed1);
        uint32_t parsed2 = parsePermissions(formatted);
        CHECK_MESSAGE(parsed2 == c.bits,
                      "round-trip diverged for input='", c.input,
                      "' formatted='", formatted, "'");
    }
}

// ─────────────────────────────────────────────────────────────────────
// Special tokens: "all" and "builtin"
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("parsePermissions(\"all\") yields every real permission bit but not BuiltIn")
{
    uint32_t got = parsePermissions("all");
    CHECK(got == Perm::All);
    // Every individual bit in the kPermNames table (except the BuiltIn
    // marker) must be set.
    CHECK((got & Perm::UiPopupCreate)    == Perm::UiPopupCreate);
    CHECK((got & Perm::UiPopupDestroy)   == Perm::UiPopupDestroy);
    CHECK((got & Perm::UiFocus)          == Perm::UiFocus);
    CHECK((got & Perm::IoFilterInput)    == Perm::IoFilterInput);
    CHECK((got & Perm::IoFilterOutput)   == Perm::IoFilterOutput);
    CHECK((got & Perm::IoInject)         == Perm::IoInject);
    CHECK((got & Perm::ShellWrite)       == Perm::ShellWrite);
    CHECK((got & Perm::ShellReadCommands)== Perm::ShellReadCommands);
    CHECK((got & Perm::ActionsInvoke)    == Perm::ActionsInvoke);
    CHECK((got & Perm::TabsCreate)       == Perm::TabsCreate);
    CHECK((got & Perm::TabsClose)        == Perm::TabsClose);
    CHECK((got & Perm::ScriptsLoad)      == Perm::ScriptsLoad);
    CHECK((got & Perm::ScriptsUnload)    == Perm::ScriptsUnload);
    CHECK((got & Perm::FsRead)           == Perm::FsRead);
    CHECK((got & Perm::FsWrite)          == Perm::FsWrite);
    CHECK((got & Perm::NetListenLocal)   == Perm::NetListenLocal);
    CHECK((got & Perm::ClipboardRead)    == Perm::ClipboardRead);
    CHECK((got & Perm::ClipboardWrite)   == Perm::ClipboardWrite);
    CHECK((got & Perm::PaneRead)         == Perm::PaneRead);
    CHECK((got & Perm::LayoutModify)     == Perm::LayoutModify);
    CHECK((got & Perm::ConfigModify)     == Perm::ConfigModify);
    CHECK((got & Perm::ProcessSpawn)     == Perm::ProcessSpawn);
    // Crucially, the BuiltIn marker MUST NOT be set — that would let
    // a user script trip the elevation gate just by passing "all".
    CHECK((got & Perm::BuiltIn) == 0u);
}

TEST_CASE("parsePermissions(\"builtin\") yields All | BuiltIn")
{
    uint32_t got = parsePermissions("builtin");
    CHECK((got & Perm::BuiltIn) == Perm::BuiltIn);
    CHECK((got & Perm::All)     == Perm::All);
    // "builtin" is exactly the union of those two — nothing else.
    CHECK(got == (Perm::All | Perm::BuiltIn));
}

TEST_CASE("parsePermissions(\"all,builtin\") composes to the same as \"builtin\"")
{
    // Order shouldn't matter — both tokens contribute to the OR. The
    // result is the same as "builtin" alone since "all" is a subset
    // of what "builtin" implies.
    CHECK(parsePermissions("all,builtin") == (Perm::All | Perm::BuiltIn));
    CHECK(parsePermissions("builtin,all") == (Perm::All | Perm::BuiltIn));
}

TEST_CASE("permissionsToString(All) round-trips through \"all\"")
{
    // permissionsToString emits "all" as a display shorthand for the
    // full bitmask; parsePermissions must return the same value when
    // fed "all" back. Without the parse-table entry this would yield
    // 0 + a warn, breaking allowlist round-trips.
    std::string s = permissionsToString(Perm::All);
    CHECK(s == "all");
    CHECK(parsePermissions(s) == Perm::All);
}

TEST_CASE("Perm::BuiltIn is bit 31 (high bit, visually distinct from real permissions)")
{
    CHECK(static_cast<uint32_t>(Perm::BuiltIn) == (1u << 31));
}

TEST_CASE("Perm::All does NOT include the BuiltIn marker bit")
{
    // Documented invariant: BuiltIn is an elevation request flag,
    // not a permission. Including it in All would let "all" be a
    // privilege-escalation vector (both the parse-table entry above
    // would compose to BuiltIn-set, and any future construction
    // like `Perm::All | foo` would inadvertently set the marker).
    CHECK((static_cast<uint32_t>(Perm::All) & Perm::BuiltIn) == 0u);
}

// ─────────────────────────────────────────────────────────────────────
// New bits introduced in this round of work
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("PaneRead is bit 19 (numeric stability of allowlist entries)")
{
    // The allowlist file persists raw bitmasks (via the "permissions"
    // string, which round-trips through parse/format). If PaneRead's
    // bit number changes, every cached allowlist entry that referenced
    // it would silently mean a different bit. The kAllowlistVersion
    // bump catches the case where the *meaning* changed; the bit
    // position itself should never move without a similar bump.
    CHECK(static_cast<uint32_t>(Perm::PaneRead) == (1u << 19));
}

TEST_CASE("ProcessSpawn is bit 23")
{
    CHECK(static_cast<uint32_t>(Perm::ProcessSpawn) == (1u << 23));
}

TEST_CASE("GroupProcess equals ProcessSpawn (single-bit group)")
{
    // Documented invariant: process is a single-bit group today.
    // If/when more process.* bits land, this check needs updating —
    // and that's the cue to also expand the d.ts MbPermission union.
    CHECK(Perm::GroupProcess == Perm::ProcessSpawn);
}

TEST_CASE("New bits don't collide with any prior bit")
{
    // Cheap sanity check: the bits we've added recently must not
    // overlap with anything else. Catches the "I copy-pasted a bit
    // number" class of bug where two enums share a position.
    constexpr uint32_t kNewlyAdded = Perm::PaneRead
                                   | Perm::ProcessSpawn
                                   | Perm::UiFocus
                                   | Perm::LayoutModify
                                   | Perm::ConfigModify
                                   | Perm::BuiltIn;
    constexpr uint32_t kPreExisting =
        Perm::UiPopupCreate    | Perm::UiPopupDestroy   |
        Perm::IoFilterInput    | Perm::IoFilterOutput   |
        Perm::IoInject         | Perm::ShellWrite       |
        Perm::ShellReadCommands| Perm::ActionsInvoke    |
        Perm::TabsCreate       | Perm::TabsClose        |
        Perm::ScriptsLoad      | Perm::ScriptsUnload    |
        Perm::FsRead           | Perm::FsWrite          |
        Perm::NetListenLocal   |
        Perm::ClipboardRead    | Perm::ClipboardWrite;
    // Disjoint: AND must be zero.
    static_assert((kNewlyAdded & kPreExisting) == 0u,
                  "permission bit collision: a recently-added bit overlaps "
                  "with a pre-existing one");
}

// ─────────────────────────────────────────────────────────────────────
// actionPermission helper
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("actionPermission: structural-mutation actions require their bits")
{
    CHECK(Script::actionPermission("NewTab")     == Perm::TabsCreate);
    CHECK(Script::actionPermission("SplitPane")  == Perm::TabsCreate);
    CHECK(Script::actionPermission("CloseTab")   == Perm::TabsClose);
    CHECK(Script::actionPermission("ClosePane")  == Perm::TabsClose);
}

TEST_CASE("actionPermission: safe actions return 0 (no extra perm)")
{
    CHECK(Script::actionPermission("CopySelection")     == 0u);
    CHECK(Script::actionPermission("ScrollUp")          == 0u);
    CHECK(Script::actionPermission("ShowScrollback")    == 0u);
    CHECK(Script::actionPermission("UnknownActionName") == 0u);
}

// ─────────────────────────────────────────────────────────────────────
// SHA-256
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("sha256Hex: known vectors")
{
    // Standard SHA-256 test vectors from FIPS 180-2 + RFC 6234.
    CHECK(Script::sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Script::sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Script::sha256Hex("The quick brown fox jumps over the lazy dog") ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}
