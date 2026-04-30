// Lock-free decode phase. Ports the byte-level state machine from
// TerminalEmulator::injectData but emits Action values into a vector
// instead of mutating grid / mDocument / mState.
//
// State owned by this function (the only writer):
//   mParserState, mEscapeBuffer, mEscapeIndex, mUtf8Buffer, mUtf8Index,
//   mStringSequence, mStringSequenceType, mWasInStringSequence, mHold.
//
// State NOT touched here (apply phase owns it):
//   mState, grid, mDocument, mLastPrintedChar, mLastPrintedX/Y,
//   mGraphemeState, charset translation, wcwidth, grapheme combining.
//
// This file is included in the build but its function is dead code
// until phase 5 wires injectData to call it.

#include "TerminalEmulator.h"
#include "Utf8.h"
#include <spdlog/spdlog.h>
#include <cassert>
#include <cstring>
#include <algorithm>

static spdlog::logger& sLog()
{
    static auto l = spdlog::get("terminal");
    return l ? *l : *spdlog::default_logger();
}

namespace {

// Detect "CSI ?2026 h" (set sync output) and "CSI ?2026 l" (reset).
// `buf` is the just-completed CSI sequence (parameter+intermediate
// bytes excluding the leading '['); `len` includes the final byte.
// Returns +1 for set, -1 for reset, 0 for neither.
int detectSyncOutputCsi(const char* buf, int len)
{
    if (len < 2) return 0;
    char finalByte = buf[len - 1];
    if (finalByte != 'h' && finalByte != 'l') return 0;
    if (buf[0] != '?') return 0;  // not a private mode
    // Walk parameters between '?' and the final byte. We're looking for
    // exactly the parameter "2026" — could be alone, or in a list. For
    // the conservative implementation, only match the simple form.
    int param = 0;
    int digits = 0;
    for (int i = 1; i < len - 1; ++i) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            param = param * 10 + (c - '0');
            ++digits;
        } else if (c == ';') {
            if (digits > 0 && param == 2026) goto found;
            param = 0;
            digits = 0;
        } else {
            return 0;  // unexpected intermediate
        }
    }
    if (digits > 0 && param == 2026) goto found;
    return 0;
found:
    return finalByte == 'h' ? 1 : -1;
}

// Detect ESC c (RIS — full reset). Clears mHold per the design.
bool isRis(char finalByte) { return finalByte == 'c'; }

}  // namespace

size_t TerminalEmulator::parseToActions(const char* buf, size_t len_)
{
    using namespace ParserAction;

    const int len = static_cast<int>(len_);
    auto& out = mPendingActions;  // append to the caller's buffer

    auto resetEscape = [this]() {
        assert(mParserState == InEscape || mParserState == InStringSequence);
        mParserState = Normal;
        mEscapeIndex = 0;
#ifndef NDEBUG
        memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
#endif
    };

    int i = 0;
    for (; i < len; ++i) {
        switch (mParserState) {
        case Normal:
            switch (buf[i]) {
            case 0x1b: // ESC
                mParserState = InEscape;
                assert(mEscapeIndex == 0);
                break;
            case '\n':
                emit(out, Control{ControlCode::LF});
                break;
            case '\r':
                emit(out, Control{ControlCode::CR});
                break;
            case '\b':
                emit(out, Control{ControlCode::BS});
                break;
            case '\t':
                emit(out, Control{ControlCode::HT});
                break;
            case '\v':
                emit(out, Control{ControlCode::VT});
                break;
            case '\f':
                emit(out, Control{ControlCode::FF});
                break;
            case '\a':
                emit(out, Control{ControlCode::BEL});
                break;
            case 0x0E: // SO (LS1): invoke G1 into GL
                emit(out, Control{ControlCode::SO});
                break;
            case 0x0F: // SI (LS0): invoke G0 into GL
                emit(out, Control{ControlCode::SI});
                break;
            default:
                if (static_cast<unsigned char>(buf[i]) >= 0x80) {
                    // Start of UTF-8 multi-byte sequence
                    assert(mUtf8Index == 0);
                    mUtf8Buffer[mUtf8Index++] = buf[i];
                    mParserState = InUtf8;
                } else {
                    // ASCII printable. The parser does NOT do charset
                    // translation (DEC graphics / UK '#') — apply does
                    // that using its current mState->shiftOut +
                    // charsetG0/G1, which it already mutates in
                    // response to Control{SO}/Control{SI} and
                    // DesignateCharset actions emitted earlier in this
                    // batch.
                    emit(out, Print{static_cast<char32_t>(static_cast<unsigned char>(buf[i]))});
                }
                break;
            }
            break;

        case InUtf8: {
            assert(mUtf8Index > 0 && mUtf8Index < 6);
            if ((buf[i] & 0xC0) != 0x80) {
                // Not a continuation byte — bad UTF-8. Reset and
                // reprocess this byte from Normal.
                sLog().error("Bad utf8 sequence (non-continuation byte)");
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mParserState = Normal;
                --i;
                break;
            }
            mUtf8Buffer[mUtf8Index++] = buf[i];
            const int expected = utf8::seqLen(static_cast<uint8_t>(mUtf8Buffer[0]));
            if (mUtf8Index == expected) {
                int consumed = 0;
                char32_t cp = utf8::decode(mUtf8Buffer, expected, consumed);
                emit(out, Print{cp});
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mParserState = Normal;
            } else if (mUtf8Index >= 6 || mUtf8Index > expected) {
                sLog().error("Bad utf8 (overlong sequence)");
                mUtf8Index = 0;
#ifndef NDEBUG
                memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
#endif
                mParserState = Normal;
            }
            break;
        }

        case InEscape:
            // ESC arriving inside an in-progress escape cancels it.
            if (buf[i] == 0x1b) {
                mEscapeIndex = 0;
                break;
            }
            if (mEscapeIndex >= static_cast<int>(sizeof(mEscapeBuffer))) {
                sLog().error("Escape buffer overflow");
                resetEscape();
                break;
            }
            mEscapeBuffer[mEscapeIndex++] = buf[i];
            switch (mEscapeBuffer[0]) {
            case SS2:
            case SS3:
            case ST:
                if (mWasInStringSequence) {
                    emit(out, StringSequence{
                        mStringSequenceType,
                        std::move(mStringSequence)});
                    mStringSequence.clear();
                    mWasInStringSequence = false;
                }
                resetEscape();
                break;
            case DCS:
            case OSX:
            case SOS:
            case PM:
            case APC:
                mStringSequenceType = mEscapeBuffer[0];
                mStringSequence.clear();
                mWasInStringSequence = false;
                mParserState = InStringSequence;
                mEscapeIndex = 0;
                break;
            case CSI:
                if (mEscapeIndex > 1) {
                    if (buf[i] >= 0x40 && buf[i] <= 0x7e) {
                        // Complete CSI. mEscapeBuffer layout matches
                        // what processCSI expects: buf[0]='[', then
                        // params/intermediates, buf[len-1]=final byte.
                        auto csi = std::make_unique<ParserAction::CSI>();
                        // Detect 2026 set/reset on the params past the
                        // leading '['.
                        int sync = detectSyncOutputCsi(
                            mEscapeBuffer + 1, mEscapeIndex - 1);
                        std::memcpy(csi->buf.data(), mEscapeBuffer,
                                    static_cast<size_t>(mEscapeIndex));
                        csi->len = static_cast<uint8_t>(mEscapeIndex);
                        csi->finalByte = buf[i];
                        csi->isPrivate = (mEscapeIndex > 1 &&
                                          mEscapeBuffer[1] == '?');
                        emit(out, std::move(csi));
                        if (sync > 0)      mHold = true;
                        else if (sync < 0) mHold = false;
                        resetEscape();
                    } else if (mEscapeIndex >= static_cast<int>(sizeof(mEscapeBuffer))) {
                        sLog().error("CSI sequence is too long {}", sizeof(mEscapeBuffer));
                        resetEscape();
                    } else if (buf[i] < 0x20 || buf[i] > 0x3f) {
                        sLog().error("Invalid CSI sequence {:#04x} character",
                                     static_cast<unsigned char>(buf[i]));
                        resetEscape();
                    }
                }
                break;
            case RIS:
                emit(out, EscSimple{RIS});
                mHold = false;  // RIS clears sync-output hold
                resetEscape();
                break;
            case VB:
            case DECKPAM:
            case DECKPNM:
            case DECSC:
            case DECRC:
            case IND:
            case NEL:
            case HTS:
            case RI:
                emit(out, EscSimple{static_cast<char>(mEscapeBuffer[0])});
                resetEscape();
                break;
            case '(':  // G0 charset designation — ESC ( X
            case ')':  // G1 charset designation — ESC ) X
                if (mEscapeIndex >= 2) {
                    emit(out, DesignateCharset{
                        mEscapeBuffer[0],
                        mEscapeBuffer[1]});
                    resetEscape();
                }
                // else wait for the designator byte
                break;
            default:
                sLog().error("Unknown escape sequence {:#04x}",
                             static_cast<unsigned char>(mEscapeBuffer[0]));
                resetEscape();
                break;
            }
            break;

        case InStringSequence:
            if (buf[i] == '\x07') {
                // BEL terminator
                emit(out, ParserAction::StringSequence{
                    mStringSequenceType,
                    std::move(mStringSequence)});
                mStringSequence.clear();
                resetEscape();
            } else if (buf[i] == 0x1b) {
                // Possible ST (\x1b\\) — transition to InEscape
                mWasInStringSequence = true;
                mParserState = InEscape;
                mEscapeIndex = 0;
            } else if (mStringSequence.size() < MAX_STRING_SEQUENCE) {
                // Run-scan: consume contiguous payload bytes in one
                // append to avoid per-byte string growth (large win for
                // kitty-graphics base64 chunks).
                int runStart = i;
                size_t remaining = MAX_STRING_SEQUENCE - mStringSequence.size();
                int limit = std::min(len, static_cast<int>(runStart + remaining));
                while (i + 1 < limit &&
                       buf[i + 1] != '\x07' && buf[i + 1] != 0x1b) {
                    ++i;
                }
                mStringSequence.append(buf + runStart, i - runStart + 1);
            }
            break;
        }
    }

    return static_cast<size_t>(i);
}
