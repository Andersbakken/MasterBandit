#include "TerminalEmulator.h"
#include "Config.h"
#include "ParserAction.h"
#include "Utils.h"
#include "Utf8.h"
#include "Wcwidth.h"
#include "Observability.h"
#include <spdlog/spdlog.h>

extern "C" {
#include <grapheme.h>
}

static spdlog::logger& sLog()
{
    static auto l = spdlog::get("terminal");
    return l ? *l : *spdlog::default_logger();
}

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <limits>
#include <algorithm>
#include <string_view>
#include <cmath>



// DEC Special Graphics character set translation table. Covers the remappable
// ASCII range 0x5F..0x7E (indices [0..0x1F]). Outside this range characters
// pass through unchanged. Matches the standard VT100/xterm mapping.
static constexpr char32_t kDecGraphics[0x20] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, // _ ` a b c d e f
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, // g h i j k l m n
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, // o p q r s t u v
    0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, // w x y z { | } ~
};

std::string toPrintable(const char *bytes, int len)
{
    std::string ret;
    ret.reserve(len);
    for (int i=0; i<len; ++i) {
        switch (bytes[i]) {
        case '\0': ret += "\\0"; break;
        case '\a': ret += "\\a"; break;
        case '\b': ret += "\\b"; break;
        case '\t': ret += "\\t"; break;
        case '\n': ret += "\\n"; break;
        case '\v': ret += "\\v"; break;
        case '\f': ret += "\\f"; break;
        case '\r': ret += "\\r"; break;
        default:
            if (std::isprint(bytes[i])) {
                ret += bytes[i];
            } else {
                char buf[16];
                const int w = snprintf(buf, sizeof(buf), "\\0%o", bytes[i]);
                ret.append(buf, w);
            }
        }
    }
    return ret;
}


void TerminalEmulator::color256ToRGB(int idx, uint8_t &r, uint8_t &g, uint8_t &b) const
{
    auto it = m256PaletteOverrides.find(idx);
    if (it != m256PaletteOverrides.end()) {
        r = it->second[0]; g = it->second[1]; b = it->second[2]; return;
    }
    if (idx < 16) {
        r = m16ColorPalette[idx][0];
        g = m16ColorPalette[idx][1];
        b = m16ColorPalette[idx][2];
    } else if (idx < 232) {
        // 6x6x6 color cube
        int v = idx - 16;
        int bi = v % 6; v /= 6;
        int gi = v % 6; v /= 6;
        int ri = v;
        r = ri ? static_cast<uint8_t>(55 + ri * 40) : 0;
        g = gi ? static_cast<uint8_t>(55 + gi * 40) : 0;
        b = bi ? static_cast<uint8_t>(55 + bi * 40) : 0;
    } else {
        // Grayscale ramp
        uint8_t v = static_cast<uint8_t>(8 + (idx - 232) * 10);
        r = g = b = v;
    }
}

TerminalEmulator::TerminalEmulator(TerminalCallbacks callbacks)
    : mCallbacks(std::move(callbacks))
{
    memset(mEscapeBuffer, 0, sizeof(mEscapeBuffer));
    memset(mUtf8Buffer, 0, sizeof(mUtf8Buffer));
    applyColorScheme(ColorScheme{}); // initialize from config defaults
}

TerminalEmulator::~TerminalEmulator()
{
}

void TerminalEmulator::publishTitleAtomic()
{
    // Caller holds mMutex (parser path).
    if (mTitleStack.empty()) {
        mTitleAtomic.store(nullptr, std::memory_order_release);
        return;
    }
    if (auto cur = mTitleAtomic.load(std::memory_order_acquire);
        cur && *cur == mTitleStack.back())
        return;
    mTitleAtomic.store(std::make_shared<const std::string>(mTitleStack.back()),
                       std::memory_order_release);
}

void TerminalEmulator::publishIconAtomic()
{
    if (mIconStack.empty()) {
        mIconAtomic.store(nullptr, std::memory_order_release);
        return;
    }
    if (auto cur = mIconAtomic.load(std::memory_order_acquire);
        cur && *cur == mIconStack.back())
        return;
    mIconAtomic.store(std::make_shared<const std::string>(mIconStack.back()),
                      std::memory_order_release);
}

void TerminalEmulator::resetScrollback(int scrollbackLines)
{
    mDocument = Document(mDocument.cols(), mDocument.rows(), scrollbackLines);
}

void TerminalEmulator::applyColorScheme(const ColorScheme& cs)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    const std::string* colors[] = {
        &cs.color0, &cs.color1, &cs.color2, &cs.color3,
        &cs.color4, &cs.color5, &cs.color6, &cs.color7,
        &cs.color8, &cs.color9, &cs.color10, &cs.color11,
        &cs.color12, &cs.color13, &cs.color14, &cs.color15
    };
    for (int i = 0; i < 16; ++i) {
        color::parseHex(*colors[i], m16ColorPalette[i][0], m16ColorPalette[i][1], m16ColorPalette[i][2]);
    }
    color::parseHex(cs.foreground, mDefaultColors.fgR, mDefaultColors.fgG, mDefaultColors.fgB);
    color::parseHex(cs.background, mDefaultColors.bgR, mDefaultColors.bgG, mDefaultColors.bgB);
    color::parseHex(cs.cursor, mDefaultColors.cursorR, mDefaultColors.cursorG, mDefaultColors.cursorB);
    // Save config-loaded values for OSC 104/110/111/112 reset
    memcpy(m16PaletteDefaults, m16ColorPalette, sizeof(m16ColorPalette));
    mConfigDefaultColors = mDefaultColors;
}

void TerminalEmulator::applyCursorConfig(const CursorConfig& cc)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    CursorShape shape;
    if (cc.shape == "underline") {
        shape = cc.blink ? CursorUnderline : CursorSteadyUnderline;
    } else if (cc.shape == "bar" || cc.shape == "beam") {
        shape = cc.blink ? CursorBar : CursorSteadyBar;
    } else {
        // "block" or anything unrecognized falls back to block
        shape = cc.blink ? CursorBlock : CursorSteadyBlock;
    }
    setDefaultCursorShape(shape);
    setDefaultCursorBlinkEnabled(cc.blink);
}

void TerminalEmulator::resize(int width, int height)
{
    // Fast no-op: skip the mMutex acquire entirely when dimensions are
    // unchanged. mWidth / mHeight are written only under mMutex by other
    // resize callers; reading without the lock is racy in the strict
    // sense but only for the no-op case where missing a concurrent
    // resize is harmless (the caller will re-arrive next tick if a real
    // change is pending). This matters because runLayoutIfDirty
    // cascades a resize through every pane on every tab activation,
    // and otherwise main blocks for the duration of a flooded pane's
    // parse batch just to discover the dimensions didn't change.
    if (mWidth == width && mHeight == height) return;

    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Re-check inside the lock to avoid losing a real concurrent change.
    if (mWidth == width && mHeight == height) return;
    int oldCols = mWidth;
    mWidth = width;
    mHeight = height;

    if (oldCols != width && !mUsingAltScreen) {
        // Column change on main screen: reflow with cursor tracking
        int oldHistSize = mDocument.historySize();
        Document::CursorTrack ct;
        ct.srcX = mState->cursorX;
        ct.srcY = oldHistSize + mState->cursorY;
        ct.dstX = 0;
        ct.dstY = 0;

        mDocument.resize(width, height, &ct);

        int newHistSize = mDocument.historySize();
        mState->cursorX = std::min(ct.dstX, width - 1);
        mState->cursorY = std::max(0, std::min(ct.dstY - newHistSize, height - 1));
    } else {
        int oldHistSize = mDocument.historySize();
        Document::CursorTrack ct;
        ct.srcX = mState->cursorX;
        ct.srcY = oldHistSize + mState->cursorY;
        ct.dstX = 0;
        ct.dstY = 0;
        mDocument.resize(width, height, &ct);
        if (oldCols == width) {
            // Height-only shrink: top rows pushed to history, adjust cursor to track content.
            // Height-only grow: don't adjust — the shell tracks its own cursor position and
            // the history rows reappearing at the top shouldn't shift the cursor.
            int histDelta = oldHistSize - mDocument.historySize();
            if (histDelta < 0)
                mState->cursorY += histDelta;
        }
        mState->cursorX = std::min(mState->cursorX, width - 1);
        mState->cursorY = std::max(0, std::min(mState->cursorY, height - 1));
    }

    mAltGrid.resize(width, height);

    // Tab stops: preserve existing stops within old range; seed defaults
    // (every 8 columns) for any newly exposed columns. A shrink just truncates.
    {
        int oldStopCols = static_cast<int>(mTabStops.size());
        mTabStops.resize(mWidth, 0);
        int firstNew = ((oldStopCols + 7) / 8) * 8;
        for (int x = firstNew; x < mWidth; x += 8)
            mTabStops[x] = 1;
    }

    mState->scrollTop = 0;
    mState->scrollBottom = height;
    mViewportOffset = std::clamp(mViewportOffset, 0, mDocument.historySize());
    mState->wrapPending = false;
    mState->savedCursorX = std::min(mState->savedCursorX, width - 1);
    mState->savedCursorY = std::min(mState->savedCursorY, height - 1);
    mState->savedWrapPending = false;
    // Selection anchors are line-id based, so they survive both height and
    // width reflow — Document::firstAbsOfLine/lastAbsOfLine resolve back to
    // the post-reflow rows. No clearSelection() needed here.
    pruneCommandRing();
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

void TerminalEmulator::scrollCursorUpToFitBelow(int rowsBelow)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (mUsingAltScreen) return;
    if (rowsBelow <= 0 || mHeight <= 0) return;
    int desired = mHeight - 1 - rowsBelow;
    if (desired < 0) desired = 0;
    int curY = mState->cursorY;
    if (curY <= desired) return;
    int n = curY - desired;
    scrollUpInRegion(n);
    mState->cursorY -= n;
}

void TerminalEmulator::scrollUpInRegion(int n)
{
    IGrid& g = grid();
    // Document::scrollUp pushes n rows into history when top == 0. If the
    // user is scrolled back at that moment, bump mViewportOffset by n so
    // they stay pinned to the same content (the viewport is positional,
    // not line-id-anchored — line-id anchoring breaks on soft-wrap chains
    // where inheritLineIdFromAbove leaves the same id across many rows).
    if (!mUsingAltScreen && mViewportOffset > 0 && mState->scrollTop == 0) {
        mViewportOffset += n;
    }
    g.scrollUp(mState->scrollTop, mState->scrollBottom, n);
    if (!mUsingAltScreen && mViewportOffset > 0) {
        mViewportOffset = std::min(mViewportOffset, mDocument.historySize());
    }
}

std::vector<TerminalEmulator::ViewAnchor>
TerminalEmulator::collectVisibleAnchors(const TerminalEmulator& term,
                                         int viewportOffset, int viewportRows)
{
    std::vector<ViewAnchor> out;
    std::vector<EmbeddedAnchor> anchors;
    term.collectEmbeddedAnchors(anchors);
    if (anchors.empty()) return out;
    const Document& doc = term.document();
    const int origin = doc.historySize() - viewportOffset;
    out.reserve(anchors.size());
    for (const auto& a : anchors) {
        int abs = doc.firstAbsOfLine(a.lineId);
        if (abs < 0) continue;
        int viewRow = abs - origin;
        if (viewRow < 0 || viewRow >= viewportRows) continue;
        out.push_back({viewRow, a.rows, a.lineId});
    }
    std::sort(out.begin(), out.end(),
              [](const ViewAnchor& a, const ViewAnchor& b) { return a.viewRow < b.viewRow; });
    return out;
}

bool TerminalEmulator::copyViewportRow(int viewRow, std::span<Cell> dst) const
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);

    assert(static_cast<int>(dst.size()) == mWidth);

    const Cell* src = nullptr;
    if (mViewportOffset == 0) {
        src = grid().row(viewRow);
    } else {
        int histSize = mDocument.historySize();
        int logicalRow = histSize - mViewportOffset + viewRow;
        if (logicalRow < histSize) {
            src = mDocument.historyRow(logicalRow);
        } else {
            src = grid().row(logicalRow - histSize);
        }
    }

    if (!src) return false;
    std::memcpy(dst.data(), src, sizeof(Cell) * static_cast<size_t>(mWidth));
    return true;
}

void TerminalEmulator::scrollViewport(int delta)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    int oldOffset = mViewportOffset;
    // delta > 0 = scroll INTO history, delta < 0 = toward live.
    mViewportOffset = std::clamp(mViewportOffset + delta, 0, mDocument.historySize());
    if (mViewportOffset != oldOffset) {
        grid().markAllDirty();
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
    }
}

void TerminalEmulator::resetViewport()
{
    // Fast no-op: every keypress calls this; on the typical case
    // (viewport already at live, offset == 0) avoid acquiring mMutex
    // entirely. Reading mViewportOffset without the lock is racy in
    // the strict sense but the no-op miss is harmless — the next
    // mutator path that actually needs the reset will fall through
    // to the locked branch. Without this, every keystroke pays the
    // parser's lock-hold duration (hundreds of ms during a flood).
    if (mViewportOffset == 0) return;
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (mViewportOffset != 0) {
        mViewportOffset = 0;
        grid().markAllDirty();
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
    }
}

void TerminalEmulator::scrollToPrompt(int direction, bool wrap)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    // Navigate via the command ring — same source Cmd+click uses, so both
    // paths agree on what "a prompt" is. Walking cell tags (as we used to)
    // could miss blocks where rowSemanticType's first-non-blank heuristic
    // picked a non-Prompt cell (shells that emit blank/continuation lines
    // with mixed tags, or where the prompt's visible text is short).
    if (mCommandRing.empty()) { resetViewport(); return; }

    int histSize = mDocument.historySize();
    // Navigation is relative to the currently selected command when one
    // exists. With no selection, anchor at the edge opposite to the
    // direction so the first press lands on the correct end of the command
    // list: Cmd+Up starts past the end → most recent command, Cmd+Down
    // starts before the start → first (oldest) command. Using viewport-top
    // instead would skip on-screen prompts when live-tailing.
    int currentAbsRow = (direction < 0) ? (histSize + mHeight) : -1;
    if (mSelectedCommandId) {
        if (const auto* rec = commandForId(*mSelectedCommandId)) {
            int abs = mDocument.firstAbsOfLine(rec->promptStartLineId);
            if (abs >= 0) currentAbsRow = abs;
        }
    }

    auto scrollTo = [&](int targetAbsRow) {
        int newOffset = histSize - targetAbsRow;
        if (newOffset <= 0) {
            resetViewport();
        } else {
            mViewportOffset = std::clamp(newOffset, 0, histSize);
            grid().markAllDirty();
            if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(ScrollbackChanged), nullptr);
        }
    };

    auto land = [&](const CommandRecord& r) {
        int abs = mDocument.firstAbsOfLine(r.promptStartLineId);
        if (abs < 0) return false;
        scrollTo(abs);
        setSelectedCommand(r.id);
        return true;
    };

    if (direction < 0) {
        // Walk ring backwards, land on first prompt strictly above currentAbsRow.
        for (auto it = mCommandRing.rbegin(); it != mCommandRing.rend(); ++it) {
            int abs = mDocument.firstAbsOfLine(it->promptStartLineId);
            if (abs >= 0 && abs < currentAbsRow) {
                scrollTo(abs);
                setSelectedCommand(it->id);
                return;
            }
        }
        if (wrap) {
            // No prompt above: wrap to the newest (last) command.
            for (auto it = mCommandRing.rbegin(); it != mCommandRing.rend(); ++it) {
                if (land(*it)) return;
            }
        }
    } else {
        for (const auto& r : mCommandRing) {
            int abs = mDocument.firstAbsOfLine(r.promptStartLineId);
            if (abs >= 0 && abs > currentAbsRow) {
                scrollTo(abs);
                setSelectedCommand(r.id);
                return;
            }
        }
        if (wrap) {
            // No prompt below: wrap to the oldest (first) command.
            for (const auto& r : mCommandRing) {
                if (land(r)) return;
            }
        }
    }
}

void TerminalEmulator::selectCommandOutput()
{
    // Keyboard entry point — operate on the currently highlighted OSC 133
    // command. If nothing is highlighted, no-op (caller should select first
    // via Cmd+click, Cmd+Up/Down, or the Cmd+double-click mouse binding).
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (!mSelectedCommandId) return;
    selectCommandOutputForRecord(commandForId(*mSelectedCommandId));
}

// ============================================================================
// OSC 133 / shell-integration state machine.
// ============================================================================

int TerminalEmulator::absoluteRowFromScreen(int screenRow) const
{
    if (mUsingAltScreen) return screenRow;      // alt screen has no scrollback
    return mDocument.historySize() + std::max(0, std::min(screenRow, mHeight - 1));
}

const TerminalEmulator::CommandRecord* TerminalEmulator::lastCommand() const
{
    // Walk backwards looking for the last completed record, skipping any
    // in-flight ones at the tail. (JS paneCommands does the same — having
    // consistent semantics across the two APIs avoids subtle caller bugs.)
    for (auto it = mCommandRing.rbegin(); it != mCommandRing.rend(); ++it) {
        if (it->complete) return &*it;
    }
    return nullptr;
}

TerminalEmulator::CommandRecord* TerminalEmulator::inProgressCommandMut()
{
    if (!mCommandInProgress || mCommandRing.empty()) return nullptr;
    CommandRecord& r = mCommandRing.back();
    return r.complete ? nullptr : &r;
}

void TerminalEmulator::startCommand(int absRow, int col)
{
    uint64_t lineId = mDocument.lineIdForAbs(absRow);
    if (CommandRecord* existing = inProgressCommandMut()) {
        if (existing->outputStartCol < 0) {
            // Collapse case: shell re-emits A before output has begun. This is a
            // prompt redraw (multi-line header collapses into one line) — relocate
            // the in-flight record instead of creating a new one.
            existing->promptStartLineId  = lineId;
            existing->promptStartCol     = col;
            existing->commandStartLineId = 0; existing->commandStartCol = -1;
            return;
        }
        // Output already started but no D seen — shell moved on (cancelled or
        // just missing the D). Finalize as complete without exit code, then
        // create a fresh record.
        finishCommand(absRow, col, std::nullopt);
    }
    CommandRecord r;
    r.id                 = mNextCommandId++;
    r.promptStartLineId  = lineId;
    r.promptStartCol     = col;
    r.cwd                = mCurrentCwd;
    mCommandRing.push_back(std::move(r));
    mCommandInProgress = true;
}

const TerminalEmulator::CommandRecord* TerminalEmulator::commandForLineId(uint64_t lineId) const
{
    auto it = std::upper_bound(
        mCommandRing.begin(), mCommandRing.end(), lineId,
        [](uint64_t id, const CommandRecord& r) { return id < r.promptStartLineId; });
    if (it == mCommandRing.begin()) return nullptr;
    --it;
    const CommandRecord& r = *it;
    if (r.complete)
        return lineId <= r.outputEndLineId ? &r : nullptr;
    // In-flight tail: accept any line at or past the prompt.
    return &r;
}

const TerminalEmulator::CommandRecord* TerminalEmulator::commandForId(uint64_t commandId) const
{
    // Ring is sorted by id (monotonic mNextCommandId++ at startCommand; only
    // append + front-pop), so binary search works.
    auto it = std::lower_bound(
        mCommandRing.begin(), mCommandRing.end(), commandId,
        [](const CommandRecord& r, uint64_t id) { return r.id < id; });
    if (it == mCommandRing.end() || it->id != commandId) return nullptr;
    return &*it;
}

void TerminalEmulator::selectCommandOutputForRecord(const CommandRecord* rec)
{
    if (!rec) return;
    std::lock_guard<std::recursive_mutex> _lk(mMutex);

    // Selection spans the same range the outline rectangle draws:
    // prompt start through output end. Resolve line ids to current absolute
    // rows. -1 means the line evicted past the archive cap.
    int startAbs = mDocument.firstAbsOfLine(rec->promptStartLineId);
    int endAbs;
    int endCol;
    if (rec->complete) {
        endAbs = mDocument.lastAbsOfLine(rec->outputEndLineId);
        endCol = rec->outputEndCol;
        // Shells typically emit D in precmd, i.e. at col 0 of the next prompt
        // row, so outputEndLineId points one row past the actual last output
        // line. Roll it back when that's the case. Same fixup as
        // TerminalSnapshot's SelectedCommandRegion.
        if (endCol == 0 && endAbs > startAbs) {
            endAbs -= 1;
            endCol = mWidth;
        }
    } else {
        // In-flight tail: extend to cursor.
        endAbs = mDocument.historySize() + cursorY();
        endCol = cursorX();
    }
    if (startAbs < 0 || endAbs < 0 || endAbs < startAbs) return;

    int startCol = std::max(0, rec->promptStartCol);

    // SelectCommand wants endCol *included* in the selection (mouse-style
    // semantics don't apply — there's no click). Use xRightHalf=true on
    // the end to advance the trailing boundary past endCol.
    startSelection(startCol, startAbs, /*xRightHalf=*/false);
    updateSelection(endCol, endAbs, /*xRightHalf=*/true);
    finalizeSelection();

    std::string text = selectedText();
    if (!text.empty() && mCallbacks.copyToClipboard) {
        mCallbacks.copyToClipboard(text);
    }
    if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
}

void TerminalEmulator::pruneCommandRing()
{
    // Drop records whose prompt line has evicted past the archive cap.
    // A line id that no longer resolves to any row is gone for good.
    while (!mCommandRing.empty()) {
        uint64_t promptId = mCommandRing.front().promptStartLineId;
        if (promptId == 0) break;
        if (mDocument.firstAbsOfLine(promptId) >= 0) break;
        mCommandRing.pop_front();
    }
    if (mSelectedCommandId) {
        bool stillPresent = false;
        for (const auto& r : mCommandRing) {
            if (r.id == *mSelectedCommandId) { stillPresent = true; break; }
        }
        if (!stillPresent) mSelectedCommandId.reset();
    }
}

void TerminalEmulator::setSelectedCommand(std::optional<uint64_t> commandId)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    if (commandId) {
        bool found = false;
        for (const auto& r : mCommandRing) {
            if (r.id == *commandId) { found = true; break; }
        }
        if (!found) commandId.reset();
    }
    if (mSelectedCommandId == commandId) return;
    mSelectedCommandId = commandId;
    if (mCallbacks.event) {
        mCallbacks.event(this, static_cast<int>(CommandSelectionChanged), nullptr);
        mCallbacks.event(this, static_cast<int>(Update), nullptr);
    }
}

void TerminalEmulator::markCommandInput(int absRow, int col)
{
    if (CommandRecord* r = inProgressCommandMut()) {
        r->commandStartLineId = mDocument.lineIdForAbs(absRow);
        r->commandStartCol    = col;
    }
}

void TerminalEmulator::markCommandOutput(int absRow, int col)
{
    CommandRecord* r = inProgressCommandMut();
    if (!r) return;
    r->outputStartLineId = mDocument.lineIdForAbs(absRow);
    r->outputStartCol    = col;
    r->startMs           = mono();
}

void TerminalEmulator::finishCommand(int absRow, int col, std::optional<int> exitCode)
{
    CommandRecord* r = inProgressCommandMut();
    if (!r) { mCommandInProgress = false; return; }
    r->outputEndLineId = mDocument.lineIdForAbs(absRow);
    r->outputEndCol    = col;
    r->endMs          = mono();
    r->exitCode       = exitCode;
    r->complete       = true;
    mCommandInProgress = false;
    if (mCallbacks.event) {
        mCallbacks.event(this, static_cast<int>(CommandComplete), r);
    }
}


std::string TerminalEmulator::serializeScrollback() const
{
    // Lock for the duration of the walk: history rows / archive /
    // viewport are all parse-mutated.
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    std::string result;
    int histSize = mDocument.historySize();

    auto emitCp = [&](char32_t cp) {
        if (cp < 0x80) {
            result += static_cast<char>(cp);
        } else {
            char buf[4];
            int n = utf8::encode(cp, buf);
            result.append(buf, n);
        }
    };

    // Serialize all rows: history first, then screen
    for (int i = 0; i < histSize + mHeight; ++i) {
        const Cell* row;
        const std::unordered_map<int, CellExtra>* extras;
        bool continued;
        if (i < histSize) {
            row = mDocument.historyRow(i);
            extras = mDocument.historyExtras(i);
            continued = mDocument.isHistoryRowContinued(i);
        } else {
            int screenRow = i - histSize;
            row = mDocument.row(screenRow);
            extras = nullptr; // screen-row extras pulled per-cell below
            continued = mDocument.isRowContinued(screenRow);
        }
        if (!row) continue;

        // Find effective width (trim trailing blank cells)
        int effectiveWidth = mWidth;
        while (effectiveWidth > 0 && row[effectiveWidth - 1].wc == 0) {
            effectiveWidth--;
        }

        // Convert cells to UTF-8
        for (int c = 0; c < effectiveWidth; ++c) {
            if (row[c].attrs.wideSpacer()) continue;
            char32_t cp = row[c].wc;
            if (cp == 0) cp = ' ';
            emitCp(cp);

            const CellExtra* ex = nullptr;
            if (extras) {
                auto it = extras->find(c);
                if (it != extras->end()) ex = &it->second;
            } else if (i >= histSize) {
                ex = mDocument.getExtra(c, i - histSize);
            }
            if (ex) {
                for (char32_t ccp : ex->combiningCps) emitCp(ccp);
            }
        }
        // Soft-wrapped rows join with their continuation so pager search
        // matches across the wrap boundary.
        if (!continued) result += '\n';
    }
    return result;
}

void TerminalEmulator::advanceCursorToNewLine()
{
    // Mark current row as continued (soft-wrapped) — main screen only
    if (!mUsingAltScreen && mState->cursorY >= 0 && mState->cursorY < mHeight) {
        mDocument.setRowContinued(mState->cursorY, true);
    }
    // Line wrap: move to column 0 of next line, scrolling if needed
    mState->cursorX = 0;
    mState->cursorY++;
    if (mState->cursorY >= mState->scrollBottom) {
        mState->cursorY = mState->scrollBottom - 1;
        scrollUpInRegion(1);
    }
    // Unify line ids across the autowrap: the new cursor row is a
    // continuation of the previous logical line. Without this, reflow that
    // un-wraps the content back into one physical row would lose the
    // trailing physical rows' ids.
    if (!mUsingAltScreen) {
        int curAbs = absoluteRowFromScreen(mState->cursorY);
        mDocument.inheritLineIdFromAbove(curAbs);
    }
}

void TerminalEmulator::lineFeed()
{
    // LF: move cursor down one line, column unchanged. Scroll if at bottom of scroll region.
    mState->cursorY++;
    if (mState->cursorY >= mState->scrollBottom) {
        mState->cursorY = mState->scrollBottom - 1;
        scrollUpInRegion(1);
    }
}

size_t TerminalEmulator::injectData(const char* buf, size_t len_, size_t /*byteBudget*/)
{
    if (sLog().should_log(spdlog::level::debug))
        sLog().debug("injectData: \"{}\"", toPrintable(buf, static_cast<int>(len_)));

    // Decode phase — lock-free. Operates on parser-state fields
    // (mParserState, mEscapeBuffer, mUtf8Buffer, mStringSequence,
    // mHold) plus the local action vector. Owns no grid / mState /
    // mDocument access.
    std::vector<ParserAction::Action> actions;
    parseToActions(buf, len_, actions);

    // Apply phase — under mMutex.
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    applyActions(actions);

    pruneCommandRing();

    // Suppress render updates during chunked image transfer to avoid
    // vsync-blocking the event loop while the PTY still has data to
    // deliver. Also suppress while a DEC mode 2026 sync-output block
    // is in flight (mHold) — render shouldn't see a partial frame.
    // Note: this only defers the Update notification; actions still
    // apply immediately under mMutex. A future improvement would have
    // parseToActions buffer actions across calls until the matching
    // reset arrives, so the render thread can't observe mid-sync state
    // even if it acquires mMutex on its own. Sufficient for the
    // 2026-emitting TUIs we've tested (vim, htop) which fence
    // their sync blocks within a single PTY read.
    if (!mKittyLoading.active && !mHold) {
        if (mCallbacks.event) mCallbacks.event(this, static_cast<int>(Update), nullptr);
    }
    obs::notifyParse(len_);
    return len_;
}


// ===== Apply phase =====
//
// applyActions walks the action list (produced by parseToActions) and
// drives grid / mDocument / mState mutations through per-variant
// helpers. Every helper here is a direct port of the inline mutation
// logic that lived inside injectData.

void TerminalEmulator::writePrintable(char32_t cp)
{
    IGrid& g = grid();

    if (cp < 0x80) {
        // Fast ASCII path. Charset translation only applies to ASCII —
        // DEC graphics maps 0x5F..0x7E and the UK charset maps '#'.
        // ASCII codepoints always start a new grapheme cluster and are
        // always single-width, so we can skip wcwidth + grapheme break.
        Charset active = mState->shiftOut ? mState->charsetG1 : mState->charsetG0;
        if (active == CharsetDECGraphics && cp >= 0x5F && cp <= 0x7E) {
            cp = kDecGraphics[cp - 0x5F];
        } else if (active == CharsetUK && cp == '#') {
            cp = 0x00A3; // £
        }
        mLastPrintedChar = cp;
        mGraphemeState = 0;
        if (mState->wrapPending) {
            advanceCursorToNewLine();
            mState->wrapPending = false;
        }
        if (mState->cursorX >= 0 && mState->cursorX < mWidth &&
            mState->cursorY >= 0 && mState->cursorY < mHeight) {
            if (mState->insertMode)
                g.insertChars(mState->cursorY, mState->cursorX, 1);
            mLastPrintedX = mState->cursorX;
            mLastPrintedY = mState->cursorY;
            g.cell(mState->cursorX, mState->cursorY) = Cell{cp, mState->currentAttrs};
            g.clearExtra(mState->cursorX, mState->cursorY);
            if (mActiveHyperlinkId || mState->currentUnderlineColor) {
                CellExtra& ex = g.ensureExtra(mState->cursorX, mState->cursorY);
                ex.hyperlinkId = mActiveHyperlinkId;
                ex.underlineColor = mState->currentUnderlineColor;
            }
            g.markRowDirty(mState->cursorY);
        }
        mState->cursorX++;
        if (mState->cursorX >= mWidth) {
            mState->cursorX = mWidth - 1;
            if (mState->autoWrap) mState->wrapPending = true;
        }
        return;
    }

    // Full path for non-ASCII codepoints. Direct port of the InUtf8
    // print branch in injectData.
    int w = wcwidth(cp);
    if (w < 0) w = 0;

    if (mLastPrintedChar != 0 && mLastPrintedX >= 0 && mLastPrintedY >= 0 &&
        mLastPrintedY < mHeight && mLastPrintedX < mWidth &&
        !grapheme_is_character_break(mLastPrintedChar, cp, &mGraphemeState)) {
        // Continuation of an existing grapheme cluster — append to the
        // base cell's combining-codepoints list.
        CellExtra& ex = g.ensureExtra(mLastPrintedX, mLastPrintedY);
        ex.combiningCps.push_back(cp);

        Cell& prevCell = g.cell(mLastPrintedX, mLastPrintedY);
        bool shouldBeWide = isWidenedEmoji(prevCell.wc) || cp == 0xFE0F;
        if (shouldBeWide && !prevCell.attrs.wide() &&
            mLastPrintedY == mState->cursorY && !mState->wrapPending &&
            mState->cursorX < mWidth) {
            prevCell.attrs.setWide(true);
            CellAttrs spacerAttrs = mState->currentAttrs;
            spacerAttrs.setWideSpacer(true);
            g.cell(mState->cursorX, mState->cursorY) = Cell{0, spacerAttrs};
            g.clearExtra(mState->cursorX, mState->cursorY);
            mState->cursorX++;
            if (mState->cursorX >= mWidth) {
                mState->cursorX = mWidth - 1;
                if (mState->autoWrap) mState->wrapPending = true;
            }
        }

        mLastPrintedChar = cp;
        g.markRowDirty(mLastPrintedY);
    } else if (w == 0) {
        // Standalone combining mark — nothing to display.
        mGraphemeState = 0;
    } else if (w == 2) {
        mLastPrintedChar = cp;
        mGraphemeState = 0;
        if (mState->wrapPending) {
            advanceCursorToNewLine();
            mState->wrapPending = false;
        }
        if (mState->cursorX + 1 >= mWidth) {
            if (mState->cursorX < mWidth && mState->cursorY >= 0 && mState->cursorY < mHeight) {
                g.cell(mState->cursorX, mState->cursorY) = Cell{' ', mState->currentAttrs};
                g.markRowDirty(mState->cursorY);
            }
            advanceCursorToNewLine();
        }
        if (mState->cursorX >= 0 && mState->cursorX + 1 < mWidth &&
            mState->cursorY >= 0 && mState->cursorY < mHeight) {
            if (mState->insertMode) g.insertChars(mState->cursorY, mState->cursorX, 2);
            mLastPrintedX = mState->cursorX;
            mLastPrintedY = mState->cursorY;
            CellAttrs wideAttrs = mState->currentAttrs;
            wideAttrs.setWide(true);
            g.cell(mState->cursorX, mState->cursorY) = Cell{cp, wideAttrs};
            g.clearExtra(mState->cursorX, mState->cursorY);
            if (mActiveHyperlinkId || mState->currentUnderlineColor) {
                CellExtra& ex = g.ensureExtra(mState->cursorX, mState->cursorY);
                ex.hyperlinkId = mActiveHyperlinkId;
                ex.underlineColor = mState->currentUnderlineColor;
            }
            CellAttrs spacerAttrs = mState->currentAttrs;
            spacerAttrs.setWideSpacer(true);
            g.cell(mState->cursorX + 1, mState->cursorY) = Cell{0, spacerAttrs};
            g.clearExtra(mState->cursorX + 1, mState->cursorY);
            g.markRowDirty(mState->cursorY);
        }
        mState->cursorX += 2;
        if (mState->cursorX >= mWidth) {
            mState->cursorX = mWidth - 1;
            if (mState->autoWrap) mState->wrapPending = true;
        }
    } else {
        mLastPrintedChar = cp;
        mGraphemeState = 0;
        if (mState->wrapPending) {
            advanceCursorToNewLine();
            mState->wrapPending = false;
        }
        if (mState->cursorX >= 0 && mState->cursorX < mWidth &&
            mState->cursorY >= 0 && mState->cursorY < mHeight) {
            if (mState->insertMode) g.insertChars(mState->cursorY, mState->cursorX, 1);
            mLastPrintedX = mState->cursorX;
            mLastPrintedY = mState->cursorY;
            g.cell(mState->cursorX, mState->cursorY) = Cell{cp, mState->currentAttrs};
            g.clearExtra(mState->cursorX, mState->cursorY);
            if (mActiveHyperlinkId || mState->currentUnderlineColor) {
                CellExtra& ex = g.ensureExtra(mState->cursorX, mState->cursorY);
                ex.hyperlinkId = mActiveHyperlinkId;
                ex.underlineColor = mState->currentUnderlineColor;
            }
            g.markRowDirty(mState->cursorY);
        }
        mState->cursorX++;
        if (mState->cursorX >= mWidth) {
            mState->cursorX = mWidth - 1;
            if (mState->autoWrap) mState->wrapPending = true;
        }
    }
}

void TerminalEmulator::applyControl(ParserAction::ControlCode code)
{
    using CC = ParserAction::ControlCode;
    switch (code) {
    case CC::LF:
    case CC::VT:
    case CC::FF:
        mState->wrapPending = false;
        lineFeed();
        break;
    case CC::CR:
        mState->cursorX = 0;
        mState->wrapPending = false;
        if (!mUsingAltScreen)
            mDocument.setRowContinued(mState->cursorY, false);
        break;
    case CC::BS:
        if (mState->cursorX > 0)
            --mState->cursorX;
        mState->wrapPending = false;
        break;
    case CC::HT: {
        int nextTab = mWidth - 1;
        for (int x = mState->cursorX + 1; x < mWidth; ++x) {
            if (mTabStops[x]) { nextTab = x; break; }
        }
        mState->cursorX = nextTab;
        break;
    }
    case CC::BEL:
        // No visible effect; visible bell is dispatched via VB EscSimple.
        break;
    case CC::SO:
        mState->shiftOut = true;
        break;
    case CC::SI:
        mState->shiftOut = false;
        break;
    }
}

void TerminalEmulator::applyEsc(char finalByte)
{
    IGrid& g = grid();
    switch (finalByte) {
    case RIS:
        // Full reset to initial state. Direct port of the inline RIS
        // handler in injectData.
        onFullReset();
        resetToDefault(mMainState);
        resetToDefault(mAltState);
        mState = &mMainState;
        if (mUsingAltScreen) {
            mUsingAltScreen = false;
            mUsingAltScreenAtomic.store(false, std::memory_order_release);
            mDocument.markAllDirty();
        }
        mImageRegistry.clear();
        mNextImageId = 1;
        mLastKittyImageId = 0;
        mDocument.clearHistory();
        mPointerShapeStackMain.clear();
        mPointerShapeStackAlt.clear();
        notifyPointerShapeChanged();
        mDocument.markAllDirty();
        mAltGrid.markAllDirty();
        for (int r = 0; r < mDocument.rows(); ++r) mDocument.clearRow(r);
        for (int r = 0; r < mAltGrid.rows(); ++r) mAltGrid.clearRow(r);
        clearSelection();
        mKittyFlags = 0;
        mKittyStackDepthMain = 0;
        mKittyStackDepthAlt = 0;
        memset(mKittyStackMain, 0, sizeof(mKittyStackMain));
        memset(mKittyStackAlt, 0, sizeof(mKittyStackAlt));
        mLastPrintedChar = 0;
        mLastPrintedX = -1;
        mLastPrintedY = -1;
        mGraphemeState = 0;
        mViewportOffset = 0;
        std::fill(mTabStops.begin(), mTabStops.end(), 0);
        for (int x = 0; x < mWidth; x += 8) mTabStops[x] = 1;
        break;
    case VB:
        if (mCallbacks.event)
            mCallbacks.event(this, static_cast<int>(VisibleBell), nullptr);
        break;
    case DECKPAM:
        mState->keypadMode = true;
        break;
    case DECKPNM:
        mState->keypadMode = false;
        break;
    case DECSC:
        mState->savedCursorX = mState->cursorX;
        mState->savedCursorY = mState->cursorY;
        mState->savedAttrs = mState->currentAttrs;
        mState->savedWrapPending = mState->wrapPending;
        mState->savedCharsetG0 = mState->charsetG0;
        mState->savedCharsetG1 = mState->charsetG1;
        mState->savedShiftOut = mState->shiftOut;
        mState->savedOriginMode = mState->originMode;
        break;
    case DECRC:
        mState->cursorX = mState->savedCursorX;
        mState->cursorY = mState->savedCursorY;
        mState->currentAttrs = mState->savedAttrs;
        mState->wrapPending = mState->savedWrapPending;
        mState->charsetG0 = mState->savedCharsetG0;
        mState->charsetG1 = mState->savedCharsetG1;
        mState->shiftOut = mState->savedShiftOut;
        mState->originMode = mState->savedOriginMode;
        break;
    case IND:
        mState->wrapPending = false;
        lineFeed();
        break;
    case HTS:
        if (mState->cursorX >= 0 && mState->cursorX < static_cast<int>(mTabStops.size()))
            mTabStops[mState->cursorX] = 1;
        break;
    case NEL:
        mState->wrapPending = false;
        mState->cursorX = 0;
        if (mState->cursorY == mState->scrollBottom - 1) {
            scrollUpInRegion(1);
        } else if (mState->cursorY < mHeight - 1) {
            mState->cursorY++;
        }
        if (!mUsingAltScreen) {
            if (mState->cursorY > 0)
                mDocument.setRowContinued(mState->cursorY - 1, false);
            mDocument.setRowContinued(mState->cursorY, false);
        }
        break;
    case RI:
        mState->wrapPending = false;
        if (mState->cursorY == mState->scrollTop) {
            g.scrollDown(mState->scrollTop, mState->scrollBottom, 1);
        } else if (mState->cursorY > 0) {
            mState->cursorY--;
        }
        break;
    default:
        sLog().error("applyEsc: unknown final byte {:#04x}",
                     static_cast<unsigned char>(finalByte));
        break;
    }
}

void TerminalEmulator::applyDesignateCharset(char slot, char charset)
{
    Charset& target = (slot == '(') ? mState->charsetG0 : mState->charsetG1;
    switch (charset) {
    case '0': target = CharsetDECGraphics; break;
    case 'A': target = CharsetUK; break;
    case 'B': target = CharsetASCII; break;
    default:  target = CharsetASCII; break;
    }
}

void TerminalEmulator::applyActions(std::vector<ParserAction::Action>& actions)
{
    for (auto& a : actions) {
        std::visit([this](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ParserAction::Print>) {
                writePrintable(x.cp);
            } else if constexpr (std::is_same_v<T, ParserAction::PrintString>) {
                for (char32_t cp : x.cps) writePrintable(cp);
            } else if constexpr (std::is_same_v<T, ParserAction::Control>) {
                applyControl(x.code);
            } else if constexpr (std::is_same_v<T, ParserAction::EscSimple>) {
                applyEsc(x.finalByte);
            } else if constexpr (std::is_same_v<T, ParserAction::DesignateCharset>) {
                applyDesignateCharset(x.slot, x.charset);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ParserAction::CSI>>) {
                processCSI(x->buf.data(), x->len);
            } else if constexpr (std::is_same_v<T, ParserAction::StringSequence>) {
                processStringSequence(x.kind, x.payload);
            }
        }, a);
    }
}

void TerminalEmulator::processCSI(const char* buf, int len)
{
    assert(len >= 1);

    if (sLog().should_log(spdlog::level::debug))
        sLog().debug("Processing CSI \"{}\"", toPrintable(buf, len));

    auto readCount = [buf, len](int def) {
        if (len == 2) // no digits
            return def;
        char *end;
        unsigned long l = strtoul(buf + 1, &end, 10);
        if (end != buf + len - 1 || l > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
            return -1;
        }
        return static_cast<int>(l);
    };

    // Check for prefix characters '?', '>', '=', '<'
    bool isPrivate = (len > 1 && buf[1] == '?');
    bool isSecondary = (len > 1 && buf[1] == '>');
    bool isEquals = (len > 1 && buf[1] == '=');
    bool isLess = (len > 1 && buf[1] == '<');

    Action action;
    char finalByte = buf[len - 1];

    switch (finalByte) {
    case CUU:
    case CUD:
    case CUF:
    case CUB:
    case CNL:
    case CPL:
    case CHA:
    case DCH:
    case ICH:
    case IL:
    case DL:
    case ECH:
    case VPA: {
        const int count = readCount(1);
        if (count == -1) {
            sLog().error("Invalid parameters for CSI {}", csiSequenceName(static_cast<CSISequence>(finalByte)));
            break;
        }
        action.count = count;
        switch (finalByte) {
        case CUU: action.type = Action::CursorUp; break;
        case CUD: action.type = Action::CursorDown; break;
        case CUF: action.type = Action::CursorForward; break;
        case CUB: action.type = Action::CursorBack; break;
        case CNL: action.type = Action::CursorNextLine; break;
        case CPL: action.type = Action::CursorPreviousLine; break;
        case CHA: action.type = Action::CursorHorizontalAbsolute; break;
        case DCH: action.type = Action::DeleteChars; break;
        case ICH: action.type = Action::InsertChars; break;
        case IL: action.type = Action::InsertLines; break;
        case DL: action.type = Action::DeleteLines; break;
        case ECH: action.type = Action::EraseChars; break;
        case VPA: action.type = Action::VerticalPositionAbsolute; break;
        }
        break; }
    case HVP:
    case CUP: {
        char *end;
        action.x = action.y = 1;
        action.type = Action::CursorPosition;
        const unsigned long x = strtoul(buf + 1, &end, 10);
        if (end == buf + len - 1) {
            if (len != 2) {
                if (!x) {
                    sLog().error("Invalid CSI CUP error 1");
                    action.type = Action::Invalid;
                } else {
                    action.x = x;
                }
            }
        } else if (*end == ';') {
            if (buf[1] != ';') {
                if (x == 0) {
                    sLog().error("Invalid CSI CUP error 2");
                    action.type = Action::Invalid;
                    break;
                }
                action.x = x;
            }
            if (end + 1 < buf + len - 1) {
                const unsigned long y = strtoul(end + 1, &end, 10);
                if (end != buf + len - 1 || !y) {
                    sLog().error("Invalid CSI CUP error 3");
                    action.type = Action::Invalid;
                    break;
                } else {
                    action.y = y;
                }
            }
        }
        break; }
    case ED:
        switch (readCount(0)) {
        case 0: action.type = Action::ClearToEndOfScreen; break;
        case 1: action.type = Action::ClearToBeginningOfScreen; break;
        case 2: action.type = Action::ClearScreen; break;
        case 3: // Clear screen + scrollback
            action.type = Action::ClearScreen;
            if (!mUsingAltScreen) mDocument.clearHistory();
            break;
        default: sLog().error("Invalid CSI ED {}", readCount(0)); break;
        }
        break;
    case EL:
        switch (readCount(0)) {
        case 0: action.type = Action::ClearToEndOfLine; break;
        case 1: action.type = Action::ClearToBeginningOfLine; break;
        case 2: action.type = Action::ClearLine; break;
        default: sLog().error("Invalid CSI EL {}", readCount(0)); break;
        }
        break;
    case SU: {
        const int n = readCount(1);
        if (n <= 0) {
            sLog().error("Invalid CSI SU {}", n);
        } else {
            action.type = Action::ScrollUp;
            action.count = n;
        }
        break; }
    case SD: {
        const int n = readCount(1);
        if (n <= 0) {
            sLog().error("Invalid CSI SD {}", n);
        } else {
            action.type = Action::ScrollDown;
            action.count = n;
        }
        break; }
    case SGR:
        processSGR(buf, len);
        break;
    case REP: {
        // Repeat preceding graphic character N times
        const int n = readCount(1);
        if (n > 0 && mLastPrintedChar != 0) {
            IGrid& g = grid();
            int w = wcwidth(mLastPrintedChar);
            if (w < 1) w = 1;
            for (int rep = 0; rep < n; ++rep) {
                if (mState->wrapPending) {
                    advanceCursorToNewLine();
                    mState->wrapPending = false;
                }
                if (w == 2) {
                    if (mState->cursorX + 1 >= mWidth) {
                        if (mState->cursorX < mWidth && mState->cursorY >= 0 && mState->cursorY < mHeight) {
                            g.cell(mState->cursorX, mState->cursorY) = Cell{' ', mState->currentAttrs};
                            g.markRowDirty(mState->cursorY);
                        }
                        advanceCursorToNewLine();
                    }
                    if (mState->cursorX >= 0 && mState->cursorX + 1 < mWidth && mState->cursorY >= 0 && mState->cursorY < mHeight) {
                        CellAttrs wideAttrs = mState->currentAttrs;
                        wideAttrs.setWide(true);
                        g.cell(mState->cursorX, mState->cursorY) = Cell{mLastPrintedChar, wideAttrs};
                        g.clearExtra(mState->cursorX, mState->cursorY);
                        CellAttrs spacerAttrs = mState->currentAttrs;
                        spacerAttrs.setWideSpacer(true);
                        g.cell(mState->cursorX + 1, mState->cursorY) = Cell{0, spacerAttrs};
                        g.clearExtra(mState->cursorX + 1, mState->cursorY);
                        g.markRowDirty(mState->cursorY);
                    }
                    mState->cursorX += 2;
                } else {
                    if (mState->cursorX >= 0 && mState->cursorX < mWidth && mState->cursorY >= 0 && mState->cursorY < mHeight) {
                        g.cell(mState->cursorX, mState->cursorY) = Cell{mLastPrintedChar, mState->currentAttrs};
                        g.clearExtra(mState->cursorX, mState->cursorY);
                        g.markRowDirty(mState->cursorY);
                    }
                    mState->cursorX++;
                }
                if (mState->cursorX >= mWidth) {
                    mState->cursorX = mWidth - 1;
                    if (mState->autoWrap) mState->wrapPending = true;
                }
            }
        }
        break; }
    case AUX:
        if (len != 3) {
            sLog().error("Invalid AUX CSI command ({})", len);
        } else if (buf[1] == '4') {
            action.type = Action::AUXPortOff;
        } else if (buf[1] == '5') {
            action.type = Action::AUXPortOn;
        } else {
            sLog().error("Invalid AUX CSI command ({:#04x})", static_cast<unsigned char>(buf[1]));
        }
        break;
    case DSR:
        if (isPrivate && len >= 4) {
            // CSI ? Ps n — private DSR queries
            char* end;
            int ps = static_cast<int>(strtoul(buf + 2, &end, 10));
            if (ps == 996) {
                // Color preference query: 1=dark, 2=light
                bool isDark = true;
                if (mCallbacks.isDarkMode) isDark = mCallbacks.isDarkMode();
                char response[16];
                int rlen = snprintf(response, sizeof(response), "\x1b[?997;%dn", isDark ? 1 : 2);
                writeToOutput(response, rlen);
            } else {
                sLog().warn("Unhandled private DSR {}", ps);
            }
        } else if (len == 3 && buf[1] == '5') {
            // Device status report: respond "OK"
            writeToOutput("\x1b[0n", 4);
        } else if (len == 3 && buf[1] == '6') {
            // Report cursor position: ESC [ row ; col R.
            // DECOM: report row relative to scrollTop.
            int reportY = mState->cursorY - (mState->originMode ? mState->scrollTop : 0);
            char response[32];
            int rlen = snprintf(response, sizeof(response), "\x1b[%d;%dR",
                                reportY + 1, mState->cursorX + 1);
            writeToOutput(response, rlen);
        } else {
            sLog().warn("Unhandled DSR: {}", toPrintable(buf, len));
        }
        break;
    case SCP:
        if (isPrivate) {
            // CSI ? Pm s — XTSAVE: save DEC private mode values
            std::vector<int> modes;
            const char* p = buf + 2;
            const char* end = buf + len - 1;
            while (p < end) {
                char* next;
                unsigned long v = strtoul(p, &next, 10);
                if (next == p) break;
                modes.push_back(static_cast<int>(v));
                p = next;
                if (p < end && *p == ';') ++p;
            }
            savePrivateModes(modes);
        } else if (len == 2) {
            action.type = Action::SaveCursorPosition;
        } else {
            sLog().warn("Ignoring CSI s variant: {}", toPrintable(buf, len));
        }
        break;
    case RCP:
        if (isPrivate) {
            // CSI ? u — query current keyboard flags
            kittyQueryFlags();
        } else if (isSecondary) {
            // CSI > flags u — push flags onto stack
            char *end;
            uint8_t flags = static_cast<uint8_t>(strtoul(buf + 2, &end, 10));
            kittyPushFlags(flags & 0x1F);
        } else if (isEquals) {
            // CSI = flags u — set flags
            // CSI = flags ; mode u — set flags with mode (1=replace, 2=OR, 3=AND NOT)
            char *end;
            uint8_t flags = static_cast<uint8_t>(strtoul(buf + 2, &end, 10));
            int mode = 1;
            if (*end == ';') {
                mode = static_cast<int>(strtoul(end + 1, &end, 10));
            }
            kittySetFlags(flags & 0x1F, mode);
        } else if (isLess) {
            // CSI < number u — pop N entries from stack
            char *end;
            int count = 1;
            if (len > 3) {
                count = static_cast<int>(strtoul(buf + 2, &end, 10));
                if (count < 1) count = 1;
            }
            kittyPopFlags(count);
        } else if (len == 2) {
            action.type = Action::RestoreCursorPosition;
        } else {
            sLog().warn("Ignoring CSI u variant: {}", toPrintable(buf, len));
        }
        break;
    case SM: // Set Mode
        if (isPrivate) {
            action.type = Action::SetMode;
            // Parse the mode number
            char *end;
            action.count = strtoul(buf + 2, &end, 10); // skip "[?"
        } else {
            char *end;
            unsigned long mode = strtoul(buf + 1, &end, 10); // skip "["
            if (mode == 4) {
                mState->insertMode = true;
            } else {
                sLog().warn("Ignoring non-private SM: {}", toPrintable(buf, len));
            }
        }
        break;
    case RM: // Reset Mode
        if (isPrivate) {
            action.type = Action::ResetMode;
            char *end;
            action.count = strtoul(buf + 2, &end, 10);
        } else {
            char *end;
            unsigned long mode = strtoul(buf + 1, &end, 10); // skip "["
            if (mode == 4) {
                mState->insertMode = false;
            } else {
                sLog().warn("Ignoring non-private RM: {}", toPrintable(buf, len));
            }
        }
        break;
    case DECSTBM: { // Set scrolling region (or XTRESTORE if private prefix)
        if (isPrivate) {
            // CSI ? Pm r — XTRESTORE: restore DEC private mode values
            std::vector<int> modes;
            const char* p = buf + 2;
            const char* end = buf + len - 1;
            while (p < end) {
                char* next;
                unsigned long v = strtoul(p, &next, 10);
                if (next == p) break;
                modes.push_back(static_cast<int>(v));
                p = next;
                if (p < end && *p == ';') ++p;
            }
            restorePrivateModes(modes);
            break;
        }
        // CSI Pt ; Pb r
        int top = 1, bottom = mHeight;
        char *end;
        unsigned long t = strtoul(buf + 1, &end, 10);
        if (end != buf + 1 && t > 0) top = static_cast<int>(t);
        if (*end == ';') {
            unsigned long b = strtoul(end + 1, &end, 10);
            if (b > 0) bottom = static_cast<int>(b);
        }
        mState->scrollTop = std::max(0, top - 1);
        mState->scrollBottom = std::min(mHeight, bottom);
        if (mState->scrollTop >= mState->scrollBottom) {
            mState->scrollTop = 0;
            mState->scrollBottom = mHeight;
        }
        mState->cursorX = 0;
        mState->cursorY = mState->scrollTop;
        break; }
    case 'c': // Device Attributes
        if (isSecondary) {
            // CSI > c — Secondary DA: VT500-class, xterm version 2500
            writeToOutput("\x1b[>64;2500;0c", 13);
        } else if (len == 2 || (len == 3 && buf[1] == '0')) {
            // CSI c or CSI 0 c — Primary DA: VT420 with common features
            writeToOutput("\x1b[?64;1;2;6;22c", 16);
        } else {
            sLog().warn("Ignoring DA variant: {}", toPrintable(buf, len));
        }
        break;
    case 'q':
        if (isSecondary) {
            // CSI > q — XTVERSION: report terminal name/version
            static const char xtver[] = "\x1bP>|MasterBandit(0.1)\x1b\\";
            writeToOutput(xtver, sizeof(xtver) - 1);
        } else {
            // CSI Ps SP q — DECSCUSR (Set Cursor Style)
            {
                int ps = 0;
                if (len > 3) {
                    // Parse parameter between '[' and ' q'
                    char* end;
                    ps = static_cast<int>(strtoul(buf + 1, &end, 10));
                }
                switch (ps) {
                case 0: mState->cursorShape = mDefaults.cursorShape; break;
                case 1: mState->cursorShape = CursorBlock; break;
                case 2: mState->cursorShape = CursorSteadyBlock; break;
                case 3: mState->cursorShape = CursorUnderline; break;
                case 4: mState->cursorShape = CursorSteadyUnderline; break;
                case 5: mState->cursorShape = CursorBar; break;
                case 6: mState->cursorShape = CursorSteadyBar; break;
                default: break;
                }
            }
        }
        break;
    case 'x': {
        // CSI Ps x with no intermediate is DECREQTPARM (Request Terminal Parameters).
        // With an intermediate ($, *, ' ...) it is a different command (DECFRA/DECSACE/etc).
        char prev = len >= 2 ? buf[len - 2] : 0;
        if (prev >= 0x20 && prev <= 0x2f) {
            sLog().warn("Ignoring CSI x with intermediate: {}", toPrintable(buf, len));
            break;
        }
        int ps = 0;
        if (len > 2) {
            char* end;
            ps = static_cast<int>(strtoul(buf + 1, &end, 10));
        }
        if (ps == 0 || ps == 1) {
            // Reply: CSI Psol;1;1;128;128;1;0 x
            //   Psol = 2 (unsolicited reply to req=0) or 3 (solicited reply to req=1)
            //   par=1 (no parity), nbits=1 (8 bits), xspeed=rspeed=128 (19200 baud),
            //   clkmul=1, flags=0
            int psol = (ps == 0) ? 2 : 3;
            char response[48];
            int rlen = snprintf(response, sizeof(response), "\x1b[%d;1;1;128;128;1;0x", psol);
            writeToOutput(response, rlen);
        }
        break; }
    case 't': {
        // XTWINOPS — handle push/pop title/icon (22/23 with Ps: 0=both, 1=icon, 2=title)
        char* end;
        int op = static_cast<int>(strtoul(buf + 1, &end, 10));
        int ps = 0; // default: both
        if (*end == ';') ps = static_cast<int>(strtoul(end + 1, &end, 10));
        const bool doIcon  = (ps == 0 || ps == 1);
        const bool doTitle = (ps == 0 || ps == 2);
        // Stack mutation runs under mMutex (held by injectData).
        // Republish the lock-free title/icon atomics on each top change
        // so per-tick consumers see the new value without locking.
        if (op == 22) {
            if (doTitle && !mTitleStack.empty() && mTitleStack.size() < TITLE_STACK_MAX)
                mTitleStack.push_back(mTitleStack.back());
            if (doIcon && !mIconStack.empty() && mIconStack.size() < ICON_STACK_MAX)
                mIconStack.push_back(mIconStack.back());
        } else if (op == 23) {
            if (doTitle) {
                if (mTitleStack.size() > 1) {
                    mTitleStack.pop_back();
                    publishTitleAtomic();
                    if (mCallbacks.onTitleChanged)
                        mCallbacks.onTitleChanged(std::optional<std::string>(mTitleStack.back()));
                } else if (!mTitleStack.empty()) {
                    mTitleStack.clear();
                    publishTitleAtomic();
                    // nullopt: no title left on the stack — distinct from
                    // OSC 2 "" (which fires Some("")).
                    if (mCallbacks.onTitleChanged)
                        mCallbacks.onTitleChanged(std::nullopt);
                }
            }
            if (doIcon) {
                if (mIconStack.size() > 1) {
                    mIconStack.pop_back();
                    publishIconAtomic();
                    if (mCallbacks.onIconChanged)
                        mCallbacks.onIconChanged(std::optional<std::string>(mIconStack.back()));
                } else if (!mIconStack.empty()) {
                    mIconStack.clear();
                    publishIconAtomic();
                    if (mCallbacks.onIconChanged)
                        mCallbacks.onIconChanged(std::nullopt);
                }
            }
        }
        break;
    }
    case 'p': {
        // CSI ? Ps $ p — DECRQM (Request Mode, private)
        // Response: CSI ? Ps ; Pm $ y
        //   Pm: 0=not recognized, 1=set, 2=reset
        if (isPrivate && len >= 5 &&
            buf[len - 2] == '$') {
            char* end;
            int ps = static_cast<int>(strtoul(buf + 2, &end, 10));
            int pm = 0; // not recognized
            switch (ps) {
            case 1:    pm = mState->cursorKeyMode ? 1 : 2; break;
            case 6:    pm = mState->originMode ? 1 : 2; break;
            case 7:    pm = mState->autoWrap ? 1 : 2; break;
            case 12:   pm = mState->cursorBlinkEnabled ? 1 : 2; break;
            case 25:   pm = mState->cursorVisible ? 1 : 2; break;
            case 1000: pm = mState->mouseMode1000 ? 1 : 2; break;
            case 1002: pm = mState->mouseMode1002 ? 1 : 2; break;
            case 1003: pm = mState->mouseMode1003 ? 1 : 2; break;
            case 1006: pm = mState->mouseMode1006 ? 1 : 2; break;
            case 1016: pm = mState->mouseMode1016 ? 1 : 2; break;
            case 1004: pm = mState->focusReporting ? 1 : 2; break;
            case 1049: pm = mUsingAltScreen ? 1 : 2; break;
            case 2004: pm = mState->bracketedPaste ? 1 : 2; break;
            case 2026: pm = mState->syncOutput ? 1 : 2; break;
            case 2027: pm = 3; break; // grapheme cluster mode — permanently set
            case 2031: pm = mState->colorPreferenceReporting ? 1 : 2; break;
            default:
                pm = 0;
                sLog().warn("DECRQM: unrecognized private mode {}", ps);
                break;
            }
            char response[32];
            int rlen = snprintf(response, sizeof(response),
                "\x1b[?%d;%d$y", ps, pm);
            writeToOutput(response, rlen);
        } else if (!isPrivate && !isSecondary && !isEquals && !isLess &&
                   len >= 3 &&
                   buf[len - 2] == '!') {
            // CSI ! p — DECSTR (Soft Terminal Reset). Start from defaults,
            // then restore the fields VT510/xterm say DECSTR must not touch:
            // cursor position & visible style, plus xterm extensions (mouse,
            // focus/paste, sync, color-pref). Screen contents, palette, and
            // tab-stop count keep on their separate reset paths.
            auto preserved = *mState;
            resetToDefault(*mState);
            mState->cursorX = preserved.cursorX;
            mState->cursorY = preserved.cursorY;
            mState->cursorShape = preserved.cursorShape;
            mState->cursorBlinkEnabled = preserved.cursorBlinkEnabled;
            mState->mouseMode1000 = preserved.mouseMode1000;
            mState->mouseMode1002 = preserved.mouseMode1002;
            mState->mouseMode1003 = preserved.mouseMode1003;
            mState->mouseMode1006 = preserved.mouseMode1006;
            mState->mouseMode1016 = preserved.mouseMode1016;
            mState->focusReporting = preserved.focusReporting;
            mState->bracketedPaste = preserved.bracketedPaste;
            mState->syncOutput = preserved.syncOutput;
            mState->colorPreferenceReporting = preserved.colorPreferenceReporting;
            // Tab stops: reset to defaults (every 8 columns).
            std::fill(mTabStops.begin(), mTabStops.end(), 0);
            for (int x = 0; x < mWidth; x += 8) mTabStops[x] = 1;
        } else {
            sLog().warn("Ignoring CSI p variant: \"{}\"", toPrintable(buf, len));
        }
        break;
    }
    case 'g': {
        // TBC (Tab Clear). Ps=0: clear stop at current column. Ps=3: clear all.
        // Ps=1/2 are per-line tab variants from old terminals — no-op here.
        int ps = readCount(0);
        if (ps == -1) ps = 0;
        if (ps == 0) {
            if (mState->cursorX >= 0 && mState->cursorX < static_cast<int>(mTabStops.size()))
                mTabStops[mState->cursorX] = 0;
        } else if (ps == 3) {
            std::fill(mTabStops.begin(), mTabStops.end(), 0);
        }
        break;
    }
    case 'I': {
        // CHT (Cursor Horizontal forward Tab): advance N tab stops.
        int n = readCount(1);
        if (n < 1) n = 1;
        for (int i = 0; i < n && mState->cursorX < mWidth - 1; ++i) {
            int nextTab = mWidth - 1;
            for (int x = mState->cursorX + 1; x < mWidth; ++x) {
                if (mTabStops[x]) { nextTab = x; break; }
            }
            mState->cursorX = nextTab;
        }
        break;
    }
    case 'Z': {
        // CBT (Cursor Backward Tabulation): retreat N tab stops.
        int n = readCount(1);
        if (n < 1) n = 1;
        for (int i = 0; i < n && mState->cursorX > 0; ++i) {
            int prevTab = 0;
            for (int x = mState->cursorX - 1; x >= 0; --x) {
                if (mTabStops[x]) { prevTab = x; break; }
            }
            mState->cursorX = prevTab;
        }
        break;
    }
    default:
        sLog().error("Unknown CSI final byte {:#x}: \"{}\"", static_cast<unsigned char>(finalByte), toPrintable(buf, len));
        break;
    }

    if (action.type != Action::Invalid)
        onAction(&action);
}

void TerminalEmulator::savePrivateModes(const std::vector<int>& modes)
{
    static constexpr int kKnownModes[] = {
        1, 7, 12, 25, 1000, 1002, 1003, 1004, 1006, 1016, 2004, 2026, 2031
    };
    auto saveOne = [this](int m) {
        switch (m) {
        case 1:    mSavedPrivateModes[1]    = mState->cursorKeyMode; break;
        case 7:    mSavedPrivateModes[7]    = mState->autoWrap; break;
        case 12:   mSavedPrivateModes[12]   = mState->cursorBlinkEnabled; break;
        case 25:   mSavedPrivateModes[25]   = mState->cursorVisible; break;
        case 1000: mSavedPrivateModes[1000] = mState->mouseMode1000; break;
        case 1002: mSavedPrivateModes[1002] = mState->mouseMode1002; break;
        case 1003: mSavedPrivateModes[1003] = mState->mouseMode1003; break;
        case 1004: mSavedPrivateModes[1004] = mState->focusReporting; break;
        case 1006: mSavedPrivateModes[1006] = mState->mouseMode1006; break;
        case 1016: mSavedPrivateModes[1016] = mState->mouseMode1016; break;
        case 2004: mSavedPrivateModes[2004] = mState->bracketedPaste; break;
        case 2026: mSavedPrivateModes[2026] = mState->syncOutput; break;
        case 2031: mSavedPrivateModes[2031] = mState->colorPreferenceReporting; break;
        default: break;
        }
    };
    if (modes.empty()) {
        for (int m : kKnownModes) saveOne(m);
    } else {
        for (int m : modes) saveOne(m);
    }
}

void TerminalEmulator::restorePrivateModes(const std::vector<int>& modes)
{
    auto restoreOne = [this](int m) {
        auto it = mSavedPrivateModes.find(m);
        if (it == mSavedPrivateModes.end()) return;
        bool v = it->second;
        switch (m) {
        case 1:    mState->cursorKeyMode             = v; break;
        case 7:    mState->autoWrap                  = v; break;
        case 12:   mState->cursorBlinkEnabled        = v; break;
        case 25:   mState->cursorVisible             = v; break;
        case 1000: mState->mouseMode1000             = v; break;
        case 1002: mState->mouseMode1002             = v; break;
        case 1003: mState->mouseMode1003             = v; break;
        case 1004: mState->focusReporting            = v; break;
        case 1006: mState->mouseMode1006             = v; break;
        case 1016: mState->mouseMode1016             = v; break;
        case 2004: mState->bracketedPaste            = v; break;
        case 2026: mState->syncOutput                = v; break;
        case 2031: mState->colorPreferenceReporting  = v; break;
        default: break;
        }
    };
    if (modes.empty()) {
        for (auto& kv : mSavedPrivateModes) restoreOne(kv.first);
    } else {
        for (int m : modes) restoreOne(m);
    }
}

void TerminalEmulator::onAction(const Action *action)
{
    assert(action);
    assert(action->type != Action::Invalid);
    if (sLog().should_log(spdlog::level::debug))
        sLog().debug("Got action {} {} {} {}", Action::typeName(action->type), action->count, action->x, action->y);

    // Any CSI action clears pending autowrap (save old state for SCP)
    bool savedWrapPending = mState->wrapPending;
    mState->wrapPending = false;

    IGrid& g = grid();

    switch (action->type) {
    case Action::CursorUp:
        mState->cursorY = std::max(0, mState->cursorY - action->count);
        break;
    case Action::CursorDown:
        mState->cursorY = std::min(mHeight - 1, mState->cursorY + action->count);
        break;
    case Action::CursorForward:
        mState->cursorX = std::min(mWidth - 1, mState->cursorX + action->count);
        break;
    case Action::CursorBack:
        mState->cursorX = std::max(0, mState->cursorX - action->count);
        break;
    case Action::CursorNextLine:
        mState->cursorY = std::min(mHeight - 1, mState->cursorY + action->count);
        mState->cursorX = 0;
        if (!mUsingAltScreen) {
            if (mState->cursorY > 0)
                mDocument.setRowContinued(mState->cursorY - 1, false);
            mDocument.setRowContinued(mState->cursorY, false);
        }
        break;
    case Action::CursorPreviousLine:
        mState->cursorY = std::max(0, mState->cursorY - action->count);
        mState->cursorX = 0;
        if (!mUsingAltScreen) {
            if (mState->cursorY > 0)
                mDocument.setRowContinued(mState->cursorY - 1, false);
            mDocument.setRowContinued(mState->cursorY, false);
        }
        break;
    case Action::CursorHorizontalAbsolute:
        mState->cursorX = std::clamp(action->count - 1, 0, mWidth - 1);
        break;
    case Action::CursorPosition: {
        // CUP: action.x = row (1-based), action.y = col (1-based) — note x/y are swapped vs screen coords.
        // DECOM: row is relative to scrollTop and clamped within [scrollTop, scrollBottom).
        int row = action->x - 1;
        int col = action->y - 1;
        if (mState->originMode) {
            row += mState->scrollTop;
            mState->cursorY = std::clamp(row, mState->scrollTop, mState->scrollBottom - 1);
        } else {
            mState->cursorY = std::clamp(row, 0, mHeight - 1);
        }
        mState->cursorX = std::clamp(col, 0, mWidth - 1);
        // Explicit positioning at col 0 of a row marks the start of a new
        // logical line there — break any soft-wrap chain the previous row
        // left set. Without this, programs like `top` that autowrap long
        // headers at narrow width and then CUP over the continuation rows
        // leave a stale continued=true on every header row; reflow at
        // wider width then joins the whole screen into one logical line.
        // Also clear the landing row's own flag — its old wrap-into-next
        // relationship is about to be made stale by the incoming overwrite.
        if (mState->cursorX == 0 && !mUsingAltScreen) {
            if (mState->cursorY > 0)
                mDocument.setRowContinued(mState->cursorY - 1, false);
            mDocument.setRowContinued(mState->cursorY, false);
        }
        break;
    }
    case Action::ClearScreen:
        for (int r = 0; r < g.rows(); ++r) g.clearRow(r);
        mState->cursorX = 0;
        mState->cursorY = 0;
        break;
    case Action::ClearToEndOfScreen:
        // Clear from cursor to end of line, then all lines below
        g.clearRow(mState->cursorY, mState->cursorX, mWidth);
        for (int r = mState->cursorY + 1; r < g.rows(); ++r) g.clearRow(r);
        break;
    case Action::ClearToBeginningOfScreen:
        // Clear from start to cursor, plus all lines above
        for (int r = 0; r < mState->cursorY; ++r) g.clearRow(r);
        g.clearRow(mState->cursorY, 0, mState->cursorX + 1);
        break;
    case Action::ClearLine:
        g.clearRow(mState->cursorY);
        break;
    case Action::ClearToEndOfLine:
        g.clearRow(mState->cursorY, mState->cursorX, mWidth);
        break;
    case Action::ClearToBeginningOfLine:
        g.clearRow(mState->cursorY, 0, mState->cursorX + 1);
        break;
    case Action::DeleteChars:
        g.deleteChars(mState->cursorY, mState->cursorX, action->count);
        break;
    case Action::InsertChars:
        g.insertChars(mState->cursorY, mState->cursorX, action->count);
        break;
    case Action::InsertLines:
        // IL: insert blank lines at cursor, pushing existing lines down within scroll region
        if (mState->cursorY >= mState->scrollTop && mState->cursorY < mState->scrollBottom) {
            g.scrollDown(mState->cursorY, mState->scrollBottom, action->count);
        }
        break;
    case Action::DeleteLines:
        // DL: delete lines at cursor, pulling lines up within scroll region
        if (mState->cursorY >= mState->scrollTop && mState->cursorY < mState->scrollBottom) {
            g.scrollUp(mState->cursorY, mState->scrollBottom, action->count);
        }
        break;
    case Action::EraseChars:
        // ECH: erase N chars at cursor without moving it
        g.clearRow(mState->cursorY, mState->cursorX, std::min(mState->cursorX + action->count, mWidth));
        break;
    case Action::VerticalPositionAbsolute: {
        int row = action->count - 1;
        if (mState->originMode) {
            row += mState->scrollTop;
            mState->cursorY = std::clamp(row, mState->scrollTop, mState->scrollBottom - 1);
        } else {
            mState->cursorY = std::clamp(row, 0, mHeight - 1);
        }
        // Same soft-wrap-chain break as CUP when VPA lands with cursor
        // already at col 0 — same reasoning re: top-style row overwrites.
        if (mState->cursorX == 0 && !mUsingAltScreen) {
            if (mState->cursorY > 0)
                mDocument.setRowContinued(mState->cursorY - 1, false);
            mDocument.setRowContinued(mState->cursorY, false);
        }
        break;
    }
    case Action::ScrollUp:
        g.scrollUp(mState->scrollTop, mState->scrollBottom, action->count);
        break;
    case Action::ScrollDown:
        g.scrollDown(mState->scrollTop, mState->scrollBottom, action->count);
        break;
    case Action::SaveCursorPosition:
        // CSI s — shares the DECSC save slot so a subsequent RCP or DECRC
        // restores the same state (xterm-equivalent behavior).
        mState->savedCursorX = mState->cursorX;
        mState->savedCursorY = mState->cursorY;
        mState->savedAttrs = mState->currentAttrs;
        mState->savedWrapPending = savedWrapPending;
        mState->savedCharsetG0 = mState->charsetG0;
        mState->savedCharsetG1 = mState->charsetG1;
        mState->savedShiftOut = mState->shiftOut;
        mState->savedOriginMode = mState->originMode;
        break;
    case Action::RestoreCursorPosition:
        mState->cursorX = mState->savedCursorX;
        mState->cursorY = mState->savedCursorY;
        mState->currentAttrs = mState->savedAttrs;
        mState->wrapPending = mState->savedWrapPending;
        mState->charsetG0 = mState->savedCharsetG0;
        mState->charsetG1 = mState->savedCharsetG1;
        mState->shiftOut = mState->savedShiftOut;
        mState->originMode = mState->savedOriginMode;
        break;
    case Action::SetMode:
        switch (action->count) {
        case 1: // DECCKM: application cursor keys
            mState->cursorKeyMode = true;
            break;
        case 3: // DECCOLM: 132-column mode. We don't actually resize — the
                // window size is user-controlled — but the spec side effects
                // (clear screen, home cursor, reset scroll region) still apply.
            mState->cursorX = 0;
            mState->cursorY = 0;
            mState->wrapPending = false;
            mState->scrollTop = 0;
            mState->scrollBottom = mHeight;
            {
                IGrid& g = grid();
                for (int r = 0; r < g.rows(); ++r) g.clearRow(r);
                g.markAllDirty();
            }
            break;
        case 6: // DECOM: origin mode — cursor goes to home within scroll region.
            mState->originMode = true;
            mState->cursorX = 0;
            mState->cursorY = mState->scrollTop;
            mState->wrapPending = false;
            break;
        case 7: // DECAWM: autowrap
            mState->autoWrap = true;
            break;
        case 12: // ATT610: cursor blink on
            mState->cursorBlinkEnabled = true;
            break;
        case 25: // DECTCEM: show cursor
            mState->cursorVisible = true;
            break;
        case 1000: mState->mouseMode1000 = true; break;
        case 1002: mState->mouseMode1002 = true; break;
        case 1003: mState->mouseMode1003 = true; break;
        case 1006: mState->mouseMode1006 = true; break;
        case 1016: mState->mouseMode1016 = true; break;
        case 1049: // Alt screen: swap to fresh alt state, clear grid
            // Main state is preserved intact in mMainState — on 1049 l we
            // just flip mState back, no field-by-field restore needed.
            resetToDefault(mAltState);
            mState = &mAltState;
            mUsingAltScreen = true;
            mUsingAltScreenAtomic.store(true, std::memory_order_release);
            // Kitty: switch to alt screen's stack
            mKittyFlags = (mKittyStackDepthAlt > 0) ? mKittyStackAlt[mKittyStackDepthAlt - 1] : 0;
            notifyPointerShapeChanged();  // alt stack is now active
            for (int r = 0; r < mAltGrid.rows(); ++r) mAltGrid.clearRow(r);
            mAltGrid.markAllDirty();
            clearSelection();
            // OSC 133 command selection is a main-screen concept; clear it
            // so scripts don't see a stale id while the alt screen is up.
            mSelectedCommandId.reset();
            break;
        case 1004: mState->focusReporting = true; break;
        case 2004: mState->bracketedPaste = true; break;
        case 2026: mState->syncOutput = true; break;
        case 2027: break; // grapheme cluster mode — always on, ignore
        case 2031: mState->colorPreferenceReporting = true; break;
        default:
            sLog().warn("Ignoring private mode set {}", action->count);
            break;
        }
        break;
    case Action::ResetMode:
        switch (action->count) {
        case 1: // DECCKM: normal cursor keys
            mState->cursorKeyMode = false;
            break;
        case 3: // DECCOLM: 80-column mode. Same side effects as the set case:
                // clear screen, home cursor, reset scroll region.
            mState->cursorX = 0;
            mState->cursorY = 0;
            mState->wrapPending = false;
            mState->scrollTop = 0;
            mState->scrollBottom = mHeight;
            {
                IGrid& g = grid();
                for (int r = 0; r < g.rows(); ++r) g.clearRow(r);
                g.markAllDirty();
            }
            break;
        case 6: // DECOM off: cursor goes to absolute home.
            mState->originMode = false;
            mState->cursorX = 0;
            mState->cursorY = 0;
            mState->wrapPending = false;
            break;
        case 7: // DECAWM: no autowrap
            mState->autoWrap = false;
            break;
        case 12: // ATT610: cursor blink off
            mState->cursorBlinkEnabled = false;
            break;
        case 25: // DECTCEM: hide cursor
            mState->cursorVisible = false;
            break;
        case 1000: mState->mouseMode1000 = false; break;
        case 1002: mState->mouseMode1002 = false; break;
        case 1003: mState->mouseMode1003 = false; break;
        case 1006: mState->mouseMode1006 = false; break;
        case 1016: mState->mouseMode1016 = false; break;
        case 1049: // Alt screen off: swap back to main — state restored implicitly
            mState = &mMainState;
            mUsingAltScreen = false;
            mUsingAltScreenAtomic.store(false, std::memory_order_release);
            // Kitty: switch back to main screen's stack
            mKittyFlags = (mKittyStackDepthMain > 0) ? mKittyStackMain[mKittyStackDepthMain - 1] : 0;
            notifyPointerShapeChanged();  // main stack is active again
            mDocument.markAllDirty();
            clearSelection();
            break;
        case 1004: mState->focusReporting = false; break;
        case 2004: mState->bracketedPaste = false; break;
        case 2026: mState->syncOutput = false; break;
        case 2027: break; // grapheme cluster mode — always on, ignore
        case 2031: mState->colorPreferenceReporting = false; break;
        default:
            sLog().warn("Ignoring private mode reset {}", action->count);
            break;
        }
        break;
    default:
        break;
    }
}

const char *TerminalEmulator::Action::typeName(Type type)
{
    switch (type) {
    case Invalid: return "Invalid";
    case CursorUp: return "CursorUp";
    case CursorDown: return "CursorDown";
    case CursorForward: return "CursorForward";
    case CursorBack: return "CursorBack";
    case CursorNextLine: return "CursorNextLine";
    case CursorPreviousLine: return "CursorPreviousLine";
    case CursorHorizontalAbsolute: return "CursorHorizontalAbsolute";
    case CursorPosition: return "CursorPosition";
    case ClearScreen: return "ClearScreen";
    case ClearToBeginningOfScreen: return "ClearToBeginningOfScreen";
    case ClearToEndOfScreen: return "ClearToEndOfScreen";
    case ClearLine: return "ClearLine";
    case ClearToBeginningOfLine: return "ClearToBeginningOfLine";
    case ClearToEndOfLine: return "ClearToEndOfLine";
    case DeleteChars: return "DeleteChars";
    case InsertChars: return "InsertChars";
    case InsertLines: return "InsertLines";
    case DeleteLines: return "DeleteLines";
    case EraseChars: return "EraseChars";
    case ScrollUp: return "ScrollUp";
    case ScrollDown: return "ScrollDown";
    case VerticalPositionAbsolute: return "VerticalPositionAbsolute";
    case SelectGraphicRendition: return "SelectGraphicRendition";
    case AUXPortOn: return "AUXPortOn";
    case AUXPortOff: return "AUXPortOff";
    case DeviceStatusReport: return "DeviceStatusReport";
    case SaveCursorPosition: return "SaveCursorPosition";
    case RestoreCursorPosition: return "RestoreCursorPosition";
    case SetMode: return "SetMode";
    case ResetMode: return "ResetMode";
    }
    abort();
    return nullptr;
}

const char *TerminalEmulator::escapeSequenceName(EscapeSequence seq)
{
    switch (seq) {
    case SS2: return "SS2";
    case SS3: return "SS3";
    case DCS: return "DCS";
    case CSI: return "CSI";
    case ST: return "ST";
    case OSX: return "OSX";
    case SOS: return "SOS";
    case PM: return "PM";
    case APC: return "APC";
    case RIS: return "RIS";
    case VB: return "VB";
    case DECKPAM: return "DECKPAM";
    case DECKPNM: return "DECKPNM";
    case DECSC: return "DECSC";
    case DECRC: return "DECRC";
    case IND: return "IND";
    case NEL: return "NEL";
    case HTS: return "HTS";
    case RI: return "RI";
    }
    abort();
    return nullptr;
}

const char *TerminalEmulator::csiSequenceName(CSISequence seq)
{
    switch (seq) {
    case CUU: return "CUU";
    case CUD: return "CUD";
    case CUF: return "CUF";
    case CUB: return "CUB";
    case CNL: return "CNL";
    case CPL: return "CPL";
    case CHA: return "CHA";
    case CUP: return "CUP";
    case ED: return "ED";
    case EL: return "EL";
    case SU: return "SU";
    case SD: return "SD";
    case HVP: return "HVP";
    case SGR: return "SGR";
    case REP: return "REP";
    case AUX: return "AUX";
    case DSR: return "DSR";
    case SCP: return "SCP";
    case IL: return "IL";
    case DL: return "DL";
    case ECH: return "ECH";
    case VPA: return "VPA";
    case RCP: return "RCP";
    case DCH: return "DCH";
    case ICH: return "ICH";
    case SM: return "SM";
    case RM: return "RM";
    case DECSTBM: return "DECSTBM";
    }
    return nullptr;
}
