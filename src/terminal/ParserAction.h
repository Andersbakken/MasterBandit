#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// Action types emitted by parseToActions (lock-free decode phase) and
// consumed by applyActions (locked apply phase).
//
// All types live in `ParserAction::` to avoid colliding with the
// existing TerminalEmulator::Action nested struct (which represents a
// parsed CSI command for onAction).

namespace ParserAction {

// C0/C1 controls dispatched by applyControl. Values are arbitrary —
// the apply phase switches on the enum, not the original byte.
enum class ControlCode : uint8_t {
    BS,    // \b  0x08
    HT,    // \t  0x09
    LF,    // \n  0x0A
    VT,    // \v  0x0B
    FF,    // \f  0x0C
    CR,    // \r  0x0D
    BEL,   // \a  0x07
    SO,    //     0x0E  shift-out  (LS1: invoke G1 into GL)
    SI,    //     0x0F  shift-in   (LS0: invoke G0 into GL)
};

// One printable codepoint. Coalesced into PrintString during emit().
struct Print {
    char32_t cp;
};

// Run of printable codepoints. Emitted only by the coalescing emit()
// helper; the state machine never produces this directly.
struct PrintString {
    std::u32string cps;
};

struct Control {
    ControlCode code;
};

// ESC X with no parameters / no intermediates. Examples:
//   ESC 7 (DECSC), ESC 8 (DECRC), ESC = (DECKPAM), ESC c (RIS),
//   ESC D (IND), ESC E (NEL), ESC H (HTS), ESC M (RI).
struct EscSimple {
    char finalByte;
};

// ESC ( X (G0) or ESC ) X (G1). `slot` is the literal '(' or ')'
// byte; `charset` is the designator byte ('B'=ASCII, '0'=DEC graphics,
// 'A'=UK, etc.).
struct DesignateCharset {
    char slot;
    char charset;
};

// CSI sequence. `buf` holds the parameter+intermediate bytes (the
// existing escape buffer minus the leading '['). Bounded to 128 bytes
// by the existing limit. Boxed via std::unique_ptr inside the variant
// so sizeof(Action) stays small (an inline 128-byte buffer would 4×
// the variant size).
struct CSI {
    std::array<char, 128> buf;
    uint8_t len;
    char finalByte;
    bool isPrivate;   // first byte was '?'
};

// String sequence (OSC / DCS / SOS / PM / APC). `kind` is the
// introducer byte (']' OSC, 'P' DCS, 'X' SOS, '^' PM, '_' APC) —
// same value as mStringSequenceType today. `payload` is moved out
// of mStringSequence at terminator time, so no copy.
struct StringSequence {
    uint8_t kind;
    std::string payload;
};

using Action = std::variant<
    Print,
    PrintString,
    Control,
    EscSimple,
    DesignateCharset,
    std::unique_ptr<CSI>,
    StringSequence>;

// Target: <=48 bytes. The largest alternative is StringSequence
// (uint8_t kind + std::string payload = 40 bytes on libstdc++ x86_64
// with SSO), which pushes the variant to 48 once you add the
// discriminator and align. Boxing StringSequence to recover 16 bytes
// would add a heap alloc per OSC/DCS — not worth it. The soft cap
// exists to flag any future variant addition that pushes us past 48.
static_assert(sizeof(Action) <= 48, "Action variant should fit in 48 bytes");

// Append `action` to `dest`, coalescing consecutive Print into a
// single PrintString. Eager coalescing reduces a plain-ASCII flood
// from N actions to 1.
inline void emit(std::vector<Action>& dest, Action action)
{
    if (auto* p = std::get_if<Print>(&action)) {
        char32_t cp = p->cp;
        if (!dest.empty()) {
            if (auto* last = std::get_if<PrintString>(&dest.back())) {
                last->cps.push_back(cp);
                return;
            }
            if (auto* prior = std::get_if<Print>(&dest.back())) {
                std::u32string s;
                s.reserve(2);
                s.push_back(prior->cp);
                s.push_back(cp);
                dest.back() = PrintString{std::move(s)};
                return;
            }
        }
    }
    dest.push_back(std::move(action));
}

}  // namespace ParserAction
