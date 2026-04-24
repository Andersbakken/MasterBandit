#include "TerminalEmulator.h"
#include "Utils.h"
#include <spdlog/spdlog.h>

#include <sys/time.h>
#include <string.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_set>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

static inline bool gettime(timeval *time)
{
#if defined(__APPLE__)
    static mach_timebase_info_data_t info;
    static bool first = true;
    uint64_t machtime = mach_absolute_time();
    if (first) {
        first = false;
        mach_timebase_info(&info);
    }
    machtime = machtime * info.numer / (info.denom * 1000);
    time->tv_sec = machtime / 1000000;
    time->tv_usec = machtime % 1000000;
#elif defined(__linux__)
    timespec spec;
    const clockid_t cid = CLOCK_MONOTONIC_RAW;
    const int ret = ::clock_gettime(cid, &spec);
    if (ret == -1) {
        memset(time, 0, sizeof(timeval));
        return false;
    }
    time->tv_sec = spec.tv_sec;
    time->tv_usec = spec.tv_nsec / 1000;
#else
#error No gettime() implementation
#endif
    return true;
}

uint64_t TerminalEmulator::mono()
{
    timeval time;
    if (gettime(&time)) {
        return (time.tv_sec * static_cast<uint64_t>(1000)) + (time.tv_usec / static_cast<uint64_t>(1000));
    }
    return 0;
}

// --- OSC 1337 iTerm2 inline images ---
namespace {

// Parsed "width=" / "height=" value. iTerm accepts: N (cells), Npx (pixels),
// N% (percent of session extent), or "auto" / unspecified (natural size).
enum class ITermDimUnit { Auto, Cells, Pixels, Percent };
struct ITermDim {
    ITermDimUnit unit { ITermDimUnit::Auto };
    float value { 0.0f };
};

bool parseFloat(std::string_view s, float& out)
{
    if (s.empty()) return false;
    try {
        size_t consumed = 0;
        float v = std::stof(std::string(s), &consumed);
        if (consumed != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

ITermDim parseITermDim(std::string_view s)
{
    ITermDim d;
    if (s.empty() || s == "auto") return d;
    if (s.back() == '%') {
        if (!parseFloat(s.substr(0, s.size() - 1), d.value) || d.value < 0)
            return {};
        d.unit = ITermDimUnit::Percent;
        return d;
    }
    if (s.size() > 2 && s.substr(s.size() - 2) == "px") {
        if (!parseFloat(s.substr(0, s.size() - 2), d.value) || d.value < 0)
            return {};
        d.unit = ITermDimUnit::Pixels;
        return d;
    }
    if (!parseFloat(s, d.value) || d.value < 0)
        return {};
    d.unit = ITermDimUnit::Cells;
    return d;
}

// Resolve a width/height spec to a cell count. Returns 0 if the spec is Auto
// (caller should fall back to natural or aspect-derived size).
int resolveDimToCells(ITermDim dim, float cellPixels, int terminalCells)
{
    switch (dim.unit) {
    case ITermDimUnit::Auto:    return 0;
    case ITermDimUnit::Cells:   return std::max(1, static_cast<int>(dim.value));
    case ITermDimUnit::Pixels:
        if (cellPixels <= 0) return 0;
        return std::max(1, static_cast<int>(std::ceil(dim.value / cellPixels)));
    case ITermDimUnit::Percent:
        if (terminalCells <= 0) return 0;
        return std::max(1, static_cast<int>(std::ceil(dim.value * terminalCells / 100.0f)));
    }
    return 0;
}

int naturalCells(int pixels, float cellPixels)
{
    if (cellPixels <= 0) return 1;
    return std::max(1, static_cast<int>(std::ceil(static_cast<float>(pixels) / cellPixels)));
}

} // namespace

void TerminalEmulator::processOSC_iTerm(std::string_view payload)
{
    // Format: "File=[params]:base64data". Params are ";"-separated name=value
    // pairs. Per iTerm spec, inline=1 is required for display (otherwise the
    // sequence is a "download to disk" request, which we don't implement).
    if (payload.substr(0, 5) != "File=") return;

    size_t colonPos = payload.find(':');
    if (colonPos == std::string_view::npos) return;

    std::string_view paramStr = payload.substr(5, colonPos - 5);
    std::string_view b64data  = payload.substr(colonPos + 1);

    bool      isInline           = false;
    bool      preserveAspect     = true;   // iTerm default
    ITermDim  widthSpec, heightSpec;
    std::string_view nameB64;

    std::string_view::size_type pos = 0;
    while (pos < paramStr.size()) {
        auto eq = paramStr.find('=', pos);
        if (eq == std::string_view::npos) break;
        auto semi = paramStr.find(';', eq);
        if (semi == std::string_view::npos) semi = paramStr.size();

        std::string_view key = paramStr.substr(pos, eq - pos);
        std::string_view val = paramStr.substr(eq + 1, semi - eq - 1);

        if      (key == "inline" && val == "1") isInline = true;
        else if (key == "width")  widthSpec  = parseITermDim(val);
        else if (key == "height") heightSpec = parseITermDim(val);
        else if (key == "preserveAspectRatio") preserveAspect = (val != "0");
        else if (key == "name")   nameB64 = val;
        // size=, type= and other iTerm params: accepted but not used.

        pos = semi + 1;
    }

    if (!isInline) return;

    std::vector<uint8_t> imageBytes = base64::decode(b64data);
    if (imageBytes.empty()) return;

    int w, h, channels;
    uint8_t* pixels = stbi_load_from_memory(
        imageBytes.data(), static_cast<int>(imageBytes.size()), &w, &h, &channels, 4);
    if (!pixels) {
        spdlog::debug("OSC 1337: stbi_load_from_memory failed");
        return;
    }

    float cw = mCallbacks.cellPixelWidth  ? mCallbacks.cellPixelWidth()  : 0.0f;
    float ch = mCallbacks.cellPixelHeight ? mCallbacks.cellPixelHeight() : 0.0f;
    if (cw <= 0 || ch <= 0) {
        stbi_image_free(pixels);
        return;
    }

    // Resolve requested cell extents. Zero = unspecified; fill in below.
    int reqCols = resolveDimToCells(widthSpec,  cw, mWidth);
    int reqRows = resolveDimToCells(heightSpec, ch, mHeight);

    // The image's aspect ratio expressed in cell units (accounts for the fact
    // that a cell is usually taller than it is wide).
    const float imgPxAspect  = static_cast<float>(w) / static_cast<float>(h);
    const float cellAspect   = ch / cw;              // >1 when cells are tall
    const float cellsAspect  = imgPxAspect * cellAspect;  // cols-per-row ratio

    int cellCols = 0, cellRows = 0;
    if (reqCols == 0 && reqRows == 0) {
        // No dims given: natural size.
        cellCols = naturalCells(w, cw);
        cellRows = naturalCells(h, ch);
    } else if (!preserveAspect) {
        cellCols = reqCols > 0 ? reqCols : naturalCells(w, cw);
        cellRows = reqRows > 0 ? reqRows : naturalCells(h, ch);
    } else if (reqCols > 0 && reqRows > 0) {
        // Fit inside the requested box, preserving aspect.
        const int derivedRows = std::max(1, static_cast<int>(std::round(reqCols / cellsAspect)));
        if (derivedRows <= reqRows) {
            cellCols = reqCols;
            cellRows = derivedRows;
        } else {
            cellCols = std::max(1, static_cast<int>(std::round(reqRows * cellsAspect)));
            cellRows = reqRows;
        }
    } else if (reqCols > 0) {
        cellCols = reqCols;
        cellRows = std::max(1, static_cast<int>(std::round(reqCols / cellsAspect)));
    } else { // reqRows > 0
        cellRows = reqRows;
        cellCols = std::max(1, static_cast<int>(std::round(reqRows * cellsAspect)));
    }

    ImageEntry entry;
    entry.id         = mNextImageId++;
    entry.pixelWidth  = w;
    entry.pixelHeight = h;
    entry.cellWidth   = cellCols;
    entry.cellHeight  = cellRows;
    entry.rgba.assign(pixels, pixels + w * h * 4);
    stbi_image_free(pixels);

    if (!nameB64.empty()) {
        std::vector<uint8_t> decoded = base64::decode(nameB64);
        if (!decoded.empty())
            entry.name.assign(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    }

    const uint32_t imageId = entry.id;
    spdlog::info("OSC 1337: image id={} {}x{} px, {}x{} cells name=\"{}\"",
                 imageId, w, h, cellCols, cellRows, entry.name);
    mImageRegistry[imageId] = std::make_shared<ImageEntry>(std::move(entry));

    placeImageInGrid(imageId, 0, cellCols, cellRows);
}

void TerminalEmulator::placeImageInGrid(uint32_t imageId, uint32_t placementId,
                                         int cellCols, int cellRows, bool moveCursor)
{
    IGrid& g = grid();
    int startCol = mState->cursorX;
    int fillCols = std::min(cellCols, mWidth - startCol);

    // If this placement ID already has cells on screen, clear them first
    if (placementId > 0) {
        for (int r = 0; r < mHeight; ++r) {
            for (int c = 0; c < mWidth; ++c) {
                const CellExtra* cex = g.getExtra(c, r);
                if (cex && cex->imageId == imageId && cex->imagePlacementId == placementId) {
                    g.clearExtra(c, r);
                    g.markRowDirty(r);
                }
            }
        }
    }

    // Collect old image IDs being overwritten so we can clean them up
    std::unordered_set<uint32_t> overwrittenIds;
    int savedX = mState->cursorX, savedY = mState->cursorY;

    for (int r = 0; r < cellRows; ++r) {
        // Scroll if cursor is at the bottom
        if (mState->cursorY >= mHeight) {
            scrollUpInRegion(1);
            mState->cursorY = mHeight - 1;
            savedY--;  // track scroll for restore
        }

        // Track old image being overwritten
        const CellExtra* oldEx = g.getExtra(startCol, mState->cursorY);
        if (oldEx && oldEx->imageId != 0 && oldEx->imageId != imageId)
            overwrittenIds.insert(oldEx->imageId);

        // Place extra at startCol with image ID, placement ID, start column, and row offset
        CellExtra& ex = g.ensureExtra(startCol, mState->cursorY);
        ex.imageId = imageId;
        ex.imagePlacementId = placementId;
        ex.imageStartCol = static_cast<uint32_t>(startCol);
        ex.imageOffsetRow = r;

        g.markRowDirty(mState->cursorY);
        mState->cursorY++;
    }

    // Clean up overwritten images that have no remaining placements on screen
    for (uint32_t oldId : overwrittenIds) {
        bool stillReferenced = false;
        for (int r = 0; r < mHeight && !stillReferenced; ++r) {
            for (int c = 0; c < mWidth && !stillReferenced; ++c) {
                const CellExtra* cex = g.getExtra(c, r);
                if (cex && cex->imageId == oldId) stillReferenced = true;
            }
        }
        if (!stillReferenced) mImageRegistry.erase(oldId);
    }

    if (!moveCursor) {
        mState->cursorX = std::max(0, savedX);
        mState->cursorY = std::max(0, savedY);
    } else {
        // Match kitty: cursor goes to last row of image, column past right edge.
        mState->cursorY--;
        if (mState->cursorY >= mHeight) {
            mState->cursorY = mHeight - 1;
        }
        mState->cursorX = std::min(startCol + fillCols, mWidth - 1);
    }
}

// --- OSC processing ---
void TerminalEmulator::processStringSequence()
{
    if (mStringSequenceType == DCS) {
        processDCS();
        return;
    }
    if (mStringSequenceType == APC) {
        processAPC();
        return;
    }
    if (mStringSequenceType != OSX) {
        spdlog::warn("Ignoring non-OSC string sequence type {:#x} (len={})", mStringSequenceType, mStringSequence.size());
        return;
    }

    // mStringSequence format: "<number>;<payload>" or just "<number>" (no-payload form)
    size_t semi = mStringSequence.find(';');
    int oscNum = 0;
    std::string_view payload;
    if (semi == std::string::npos) {
        // No semicolon: number-only form (e.g. OSC 104 ST for palette reset-all)
        for (char c : mStringSequence) {
            if (c < '0' || c > '9') return;
            oscNum = oscNum * 10 + (c - '0');
        }
    } else {
        for (size_t i = 0; i < semi; ++i) {
            char c = mStringSequence[i];
            if (c < '0' || c > '9') return;
            oscNum = oscNum * 10 + (c - '0');
        }
        payload = std::string_view(mStringSequence.data() + semi + 1, mStringSequence.size() - semi - 1);
    }

    switch (oscNum) {
    case 0: processOSC_Title(payload, true); break;
    case 1:
        if (mIconStack.empty())
            mIconStack.emplace_back(payload);
        else
            mIconStack.back() = std::string(payload);
        if (mCallbacks.onIconChanged)
            mCallbacks.onIconChanged(std::optional<std::string>(std::string(payload)));
        break;
    case 2: processOSC_Title(payload, true); break;
    case 22: processOSC_PointerShape(payload); break;
    case 133: { // Shell integration (FinalTerm / semantic prompts / Per-Bothner spec)
        // OSC 133;<letter>[;<arg>][;<k>=<v>...]
        //   A = start new prompt (options: aid, cl)
        //   B = end prompt / start of input
        //   C = end of input / start of output
        //   D[;<exit>[;err=<v>]] = command finished
        //   N = like A but closes matching aid'd command (options: aid, cl)
        //   P = explicit prompt start (option: k=i|r|c|s)
        //   I = end prompt / start input (ends at EOL)
        //   L = fresh-line (emit \r\n if not at column 0)
        // Legacy: also records per-row PromptKind for scrollToPrompt until zones land.
        if (payload.empty()) break;
        char kind = payload[0];

        // Parse semicolon-delimited tokens after the letter. First may be a bare
        // value (e.g. exit code on D); rest are key=value options.
        std::string_view rest = payload.size() > 1 && payload[1] == ';'
            ? payload.substr(2) : std::string_view{};
        std::optional<int> firstArgInt;
        bool isSecondary = false;
        // `k=s` (continuation/secondary prompt) in any position of the options.
        // Per spec: also `k=c` (editable continuation) and `k=r` (right prompt);
        // we currently conflate c/s as "secondary" and don't do anything with r.
        size_t pos = 0;
        int tokenIndex = 0;
        while (pos <= rest.size()) {
            size_t semi = rest.find(';', pos);
            std::string_view tok = rest.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
            if (!tok.empty()) {
                auto eq = tok.find('=');
                if (eq == std::string_view::npos) {
                    if (tokenIndex == 0) {
                        // bare value — only meaningful on D (exit code)
                        try { firstArgInt = std::stoi(std::string(tok)); } catch (...) {}
                    }
                } else {
                    auto key = tok.substr(0, eq);
                    auto val = tok.substr(eq + 1);
                    if (key == "k" && (val == "s" || val == "c")) isSecondary = true;
                    else if (key == "err" && !val.empty()) {
                        // Non-empty err= wins over bare exit code per the spec.
                        int v = 0; try { v = std::stoi(std::string(val)); } catch (...) { v = 1; }
                        firstArgInt = v;
                    }
                }
            }
            if (semi == std::string_view::npos) break;
            pos = semi + 1;
            ++tokenIndex;
        }

        int absRow = absoluteRowFromScreen(mState->cursorY);

        (void)isSecondary; // currently informational; could tag CommandRecord later
        switch (kind) {
        case 'A':
        case 'N':
            if (kind == 'N' && mCommandInProgress) {
                // Implicit close of the in-flight command (no exit code available).
                finishCommand(absRow, mState->cursorX, std::nullopt);
            }
            startCommand(absRow, mState->cursorX);
            mSemanticMode = SemanticMode::Prompt;
            mState->currentAttrs.setSemanticType(CellAttrs::Prompt);
            break;
        case 'P':
            // Explicit prompt start; behave like A if no command is in progress.
            if (!mCommandInProgress) startCommand(absRow, mState->cursorX);
            mSemanticMode = SemanticMode::Prompt;
            mState->currentAttrs.setSemanticType(CellAttrs::Prompt);
            break;
        case 'B':
        case 'I':
            markCommandInput(absRow, mState->cursorX);
            mSemanticMode = SemanticMode::Input;
            mState->currentAttrs.setSemanticType(CellAttrs::Input);
            break;
        case 'C':
            markCommandOutput(absRow, mState->cursorX);
            mSemanticMode = SemanticMode::Output;
            mState->currentAttrs.setSemanticType(CellAttrs::Output);
            break;
        case 'D':
            finishCommand(absRow, mState->cursorX, firstArgInt);
            mSemanticMode = SemanticMode::Inactive;
            // Reset the pen to Output — cells written between D and the next A are
            // output-by-default. Matches WezTerm's convention.
            mState->currentAttrs.setSemanticType(CellAttrs::Output);
            break;
        case 'L':
            // Fresh-line: emit \r\n if cursor isn't already at column 0.
            // Just synthesize a newline through the normal path.
            if (mState->cursorX > 0) {
                lineFeed();
                mState->cursorX = 0;
            }
            break;
        default:
            // Unknown letter; ignore silently per spec ("terminal must ignore unknown options").
            break;
        }
        break;
    }
    case 4:  // Set/query individual ANSI palette entry
        processOSC_Palette(payload);
        break;
    case 10: // Default foreground color
    case 11: // Default background color
    case 12: // Cursor color
    case 110: // Reset fg to config default
    case 111: // Reset bg to config default
    case 112: // Reset cursor to config default
        processOSC_Color(oscNum, payload);
        break;
    case 104: // Reset palette entries (no args = all; arg = specific index)
        processOSC_PaletteReset(payload);
        break;
    case 9: {
        // OSC 9;4 progress: "9;4;state" or "9;4;state;pct"
        // Split mStringSequence on ';' after the "9" prefix
        std::vector<std::string_view> parts;
        {
            std::string_view sv(mStringSequence);
            size_t pos = 0;
            while (pos <= sv.size()) {
                size_t sep = sv.find(';', pos);
                if (sep == std::string_view::npos) {
                    parts.push_back(sv.substr(pos));
                    break;
                }
                parts.push_back(sv.substr(pos, sep - pos));
                pos = sep + 1;
            }
        }
        if (parts.size() >= 3 && parts[0] == "9" && parts[1] == "4") {
            int state = 0;
            int pct = 0;
            try { state = std::stoi(std::string(parts[2])); } catch (...) {}
            if (parts.size() >= 4) {
                try { pct = std::stoi(std::string(parts[3])); } catch (...) {}
            }
            if (mCallbacks.onProgressChanged) mCallbacks.onProgressChanged(state, pct);
        }
        break;
    }
    case 7: // CWD reporting: file://hostname/path
        {
            std::string_view url = payload;
            // Strip file:// prefix and optional hostname
            if (url.substr(0, 7) == "file://") {
                url.remove_prefix(7);
                auto slash = url.find('/');
                if (slash != std::string_view::npos)
                    url.remove_prefix(slash);
            }
            // Remember for attaching to subsequent OSC 133;A records.
            mCurrentCwd.assign(url.data(), url.size());
            if (mCallbacks.onCWDChanged) mCallbacks.onCWDChanged(std::string(url));
        }
        break;
    case 8: // Hyperlinks: "params;uri" to start, ";;" to end
        {
            auto semi2 = payload.find(';');
            if (semi2 == std::string_view::npos) break;
            std::string_view params = payload.substr(0, semi2);
            std::string_view uri = payload.substr(semi2 + 1);
            if (uri.empty()) {
                // End hyperlink
                mActiveHyperlinkId = 0;
            } else {
                // Start hyperlink — extract optional id= param
                std::string linkId;
                size_t idPos = params.find("id=");
                if (idPos != std::string_view::npos) {
                    auto idEnd = params.find(';', idPos);
                    if (idEnd == std::string_view::npos) idEnd = params.size();
                    linkId = std::string(params.substr(idPos + 3, idEnd - idPos - 3));
                }
                // Check if same id already registered
                uint32_t foundId = 0;
                if (!linkId.empty()) {
                    for (auto& [k, v] : mHyperlinkRegistry) {
                        if (v.id == linkId && v.uri == std::string(uri)) {
                            foundId = k;
                            break;
                        }
                    }
                }
                if (foundId) {
                    mActiveHyperlinkId = foundId;
                } else {
                    uint32_t id = mNextHyperlinkId++;
                    mHyperlinkRegistry[id] = {std::string(uri), linkId};
                    mActiveHyperlinkId = id;
                }
            }
        }
        break;
    case 52: processOSC_Clipboard(payload); break;
    case 99: // Desktop notifications (kitty protocol)
        {
            // Format: "metadata;content" where metadata is colon-separated key=value pairs
            // Keys: i=identifier, p=title|body, d=0|1 (done)
            auto payloadSemi = payload.find(';');
            std::string_view metadata = (payloadSemi != std::string_view::npos)
                ? payload.substr(0, payloadSemi) : payload;
            std::string text = (payloadSemi != std::string_view::npos)
                ? std::string(payload.substr(payloadSemi + 1)) : std::string{};

            std::string pType;
            std::string nId;
            bool done = false;

            std::string_view sv = metadata;
            while (!sv.empty()) {
                auto eq = sv.find('=');
                if (eq == std::string_view::npos) break;
                auto key = sv.substr(0, eq);
                sv.remove_prefix(eq + 1);
                auto colon = sv.find(':');
                auto val = (colon != std::string_view::npos) ? sv.substr(0, colon) : sv;
                if (colon != std::string_view::npos) sv.remove_prefix(colon + 1);
                else sv = {};

                if (key == "i") nId = std::string(val);
                else if (key == "d" && val == "1") done = true;
                else if (key == "p") pType = std::string(val);
            }

            if (!nId.empty()) mNotifyId = nId;
            if (pType == "title") mNotifyTitle = text;
            else if (pType == "body") mNotifyBody = text;

            if (done) {
                if (mCallbacks.onDesktopNotification)
                    mCallbacks.onDesktopNotification(mNotifyTitle, mNotifyBody, mNotifyId);
                mNotifyId.clear();
                mNotifyTitle.clear();
                mNotifyBody.clear();
            }
        }
        break;
    case 1337: processOSC_iTerm(payload); break;
    default:
        if (mCallbacks.onOSC) mCallbacks.onOSC(oscNum, payload);
        else spdlog::warn("Ignoring OSC {}", oscNum);
        break;
    }
}

void TerminalEmulator::processOSC_Title(std::string_view text, bool setTitle)
{
    if (setTitle) {
        if (mTitleStack.empty())
            mTitleStack.emplace_back(text);
        else
            mTitleStack.back() = std::string(text);
        if (mCallbacks.onTitleChanged)
            mCallbacks.onTitleChanged(std::optional<std::string>(std::string(text)));
    }
}

void TerminalEmulator::processOSC_Clipboard(std::string_view payload)
{
    // Format: "c;base64data" or "c;?"
    size_t semi = payload.find(';');
    if (semi == std::string_view::npos) return;

    std::string_view data = payload.substr(semi + 1);
    if (data == "?") {
        // Query clipboard
        std::string clip = mCallbacks.pasteFromClipboard ? mCallbacks.pasteFromClipboard() : std::string{};
        std::string encoded = base64::encode(
            reinterpret_cast<const uint8_t*>(clip.data()), clip.size());
        std::string response = "\x1b]52;c;" + encoded + "\x1b\\";
        writeToOutput(response.data(), response.size());
    } else {
        // Set clipboard
        std::vector<uint8_t> decoded = base64::decode(data);
        std::string text(decoded.begin(), decoded.end());
        if (mCallbacks.copyToClipboard) mCallbacks.copyToClipboard(text);
    }
}

// Parse X11 color spec: "rgb:RR/GG/BB", "rgb:RRRR/GGGG/BBBB", or "#RRGGBB"
static bool parseX11Color(std::string_view spec, uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (spec.size() == 7 && spec[0] == '#') {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int rr = hex(spec[1]) * 16 + hex(spec[2]);
        int gg = hex(spec[3]) * 16 + hex(spec[4]);
        int bb = hex(spec[5]) * 16 + hex(spec[6]);
        if (rr < 0 || gg < 0 || bb < 0) return false;
        r = static_cast<uint8_t>(rr);
        g = static_cast<uint8_t>(gg);
        b = static_cast<uint8_t>(bb);
        return true;
    }
    if (spec.substr(0, 4) == "rgb:") {
        // rgb:R/G/B or rgb:RR/GG/BB or rgb:RRRR/GGGG/BBBB
        auto rest = spec.substr(4);
        auto s1 = rest.find('/');
        if (s1 == std::string_view::npos) return false;
        auto s2 = rest.find('/', s1 + 1);
        if (s2 == std::string_view::npos) return false;
        auto parseComponent = [](std::string_view s) -> int {
            uint32_t v = 0;
            for (char c : s) {
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else return -1;
                v = v * 16 + d;
            }
            // Scale to 8-bit: 1-digit=*17, 2-digit=as-is, 3-digit=>>4, 4-digit=>>8
            if (s.size() == 1) return v * 17;
            if (s.size() == 2) return v;
            if (s.size() == 3) return v >> 4;
            if (s.size() == 4) return v >> 8;
            return -1;
        };
        int rr = parseComponent(rest.substr(0, s1));
        int gg = parseComponent(rest.substr(s1 + 1, s2 - s1 - 1));
        int bb = parseComponent(rest.substr(s2 + 1));
        if (rr < 0 || gg < 0 || bb < 0) return false;
        r = static_cast<uint8_t>(rr);
        g = static_cast<uint8_t>(gg);
        b = static_cast<uint8_t>(bb);
        return true;
    }
    return false;
}

void TerminalEmulator::processOSC_Color(int oscNum, std::string_view payload)
{
    // OSC 110/111/112: reset to config-loaded defaults (payload ignored)
    if (oscNum == 110) {
        mDefaultColors.fgR = mConfigDefaultColors.fgR;
        mDefaultColors.fgG = mConfigDefaultColors.fgG;
        mDefaultColors.fgB = mConfigDefaultColors.fgB;
        return;
    }
    if (oscNum == 111) {
        mDefaultColors.bgR = mConfigDefaultColors.bgR;
        mDefaultColors.bgG = mConfigDefaultColors.bgG;
        mDefaultColors.bgB = mConfigDefaultColors.bgB;
        return;
    }
    if (oscNum == 112) {
        mDefaultColors.cursorR = mConfigDefaultColors.cursorR;
        mDefaultColors.cursorG = mConfigDefaultColors.cursorG;
        mDefaultColors.cursorB = mConfigDefaultColors.cursorB;
        return;
    }

    uint8_t* r; uint8_t* g; uint8_t* b;
    switch (oscNum) {
    case 10: r = &mDefaultColors.fgR; g = &mDefaultColors.fgG; b = &mDefaultColors.fgB; break;
    case 11: r = &mDefaultColors.bgR; g = &mDefaultColors.bgG; b = &mDefaultColors.bgB; break;
    case 12: r = &mDefaultColors.cursorR; g = &mDefaultColors.cursorG; b = &mDefaultColors.cursorB; break;
    default: return;
    }

    if (payload == "?") {
        // Query: respond with rgb:RRRR/GGGG/BBBB (16-bit per channel)
        char response[64];
        int len = snprintf(response, sizeof(response),
            "\x1b]%d;rgb:%04x/%04x/%04x\x1b\\",
            oscNum,
            static_cast<unsigned>(*r) * 257,
            static_cast<unsigned>(*g) * 257,
            static_cast<unsigned>(*b) * 257);
        writeToOutput(response, len);
    } else {
        // Set color
        uint8_t nr, ng, nb;
        if (parseX11Color(payload, nr, ng, nb)) {
            *r = nr; *g = ng; *b = nb;
        }
    }
}

// OSC 4: set/query individual palette entries.
// Payload format: "index;colorspec[;index;colorspec...]"
// colorspec "?" = query, otherwise an X11 color spec.
void TerminalEmulator::processOSC_Palette(std::string_view payload)
{
    // Split payload into tokens on ';'
    std::vector<std::string_view> tokens;
    size_t pos = 0;
    while (pos <= payload.size()) {
        size_t sep = payload.find(';', pos);
        if (sep == std::string_view::npos) {
            tokens.push_back(payload.substr(pos));
            break;
        }
        tokens.push_back(payload.substr(pos, sep - pos));
        pos = sep + 1;
    }

    // Process pairs: tokens[0]=index, tokens[1]=colorspec, tokens[2]=index, ...
    for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
        // Parse index
        int idx = 0;
        for (char c : tokens[i]) {
            if (c < '0' || c > '9') { idx = -1; break; }
            idx = idx * 10 + (c - '0');
        }
        if (idx < 0 || idx > 255) continue;

        std::string_view spec = tokens[i + 1];
        if (spec == "?") {
            // Query: respond with current color for this index
            uint8_t r, g, b;
            color256ToRGB(idx, r, g, b);
            char response[64];
            int len = snprintf(response, sizeof(response),
                "\x1b]4;%d;rgb:%04x/%04x/%04x\x1b\\",
                idx,
                static_cast<unsigned>(r) * 257,
                static_cast<unsigned>(g) * 257,
                static_cast<unsigned>(b) * 257);
            writeToOutput(response, len);
        } else {
            // Set: parse and store override
            uint8_t r, g, b;
            if (parseX11Color(spec, r, g, b)) {
                m256PaletteOverrides[idx] = { r, g, b };
                if (idx < 16) {
                    // Also update m16ColorPalette so direct palette reads stay consistent
                    m16ColorPalette[idx][0] = r;
                    m16ColorPalette[idx][1] = g;
                    m16ColorPalette[idx][2] = b;
                }
            }
        }
    }
}

// OSC 104: reset palette entries to config defaults.
// Empty payload = reset all; otherwise payload is a single index.
void TerminalEmulator::processOSC_PaletteReset(std::string_view payload)
{
    if (payload.empty()) {
        // Reset all overrides
        m256PaletteOverrides.clear();
        memcpy(m16ColorPalette, m16PaletteDefaults, sizeof(m16ColorPalette));
    } else {
        // Parse a single index
        int idx = 0;
        for (char c : payload) {
            if (c < '0' || c > '9') return; // invalid
            idx = idx * 10 + (c - '0');
        }
        if (idx < 0 || idx > 255) return;
        m256PaletteOverrides.erase(idx);
        if (idx < 16) {
            m16ColorPalette[idx][0] = m16PaletteDefaults[idx][0];
            m16ColorPalette[idx][1] = m16PaletteDefaults[idx][1];
            m16ColorPalette[idx][2] = m16PaletteDefaults[idx][2];
        }
    }
}

bool TerminalEmulator::isKnownPointerShape(std::string_view name)
{
    // CSS3 cursor names plus the X11 / kitty aliases. Kept sorted by length
    // then alpha for readability — runtime cost is negligible.
    static constexpr std::string_view kNames[] = {
        // CSS3 canonical
        "default", "none", "context-menu", "help", "pointer", "progress",
        "wait", "cell", "crosshair", "text", "vertical-text", "alias",
        "copy", "move", "no-drop", "not-allowed", "grab", "grabbing",
        "e-resize", "n-resize", "ne-resize", "nw-resize", "s-resize",
        "se-resize", "sw-resize", "w-resize", "ew-resize", "ns-resize",
        "nesw-resize", "nwse-resize", "col-resize", "row-resize",
        "all-scroll", "zoom-in", "zoom-out",
        // X11 / kitty aliases
        "left_ptr", "xterm", "ibeam", "pointing_hand", "hand2", "hand",
        "question_arrow", "whats_this", "clock", "watch", "half-busy",
        "left_ptr_watch", "tcross", "plus", "cross", "fleur",
        "pointer-move", "right_side", "top_right_corner", "top_left_corner",
        "top_side", "bottom_right_corner", "bottom_left_corner",
        "bottom_side", "left_side", "sb_h_double_arrow", "split_h",
        "sb_v_double_arrow", "split_v", "size_bdiag", "size-bdiag",
        "size_fdiag", "size-fdiag", "zoom_in", "zoom_out",
        "dnd-link", "dnd-copy", "forbidden", "crossed_circle",
        "dnd-no-drop", "openhand", "hand1", "closedhand", "dnd-none",
    };
    for (auto n : kNames) {
        if (n == name) return true;
    }
    return false;
}

void TerminalEmulator::notifyPointerShapeChanged()
{
    if (mCallbacks.onMouseCursorShape)
        mCallbacks.onMouseCursorShape(currentPointerShape());
}

void TerminalEmulator::processOSC_PointerShape(std::string_view payload)
{
    // OSC 22 ; [op]name[,name...] ST
    //   '=' set top of stack (default if op omitted)
    //   '>' push name(s) onto stack
    //   '<' pop one entry
    //   '?' query: respond OSC 22 ; csv-of-1/0/named ST
    char op = '=';
    if (!payload.empty() &&
        (payload.front() == '=' || payload.front() == '>' ||
         payload.front() == '<' || payload.front() == '?')) {
        op = payload.front();
        payload.remove_prefix(1);
    }

    std::vector<std::string_view> names;
    if (!payload.empty()) {
        size_t pos = 0;
        while (pos <= payload.size()) {
            size_t comma = payload.find(',', pos);
            if (comma == std::string_view::npos) {
                names.push_back(payload.substr(pos));
                break;
            }
            names.push_back(payload.substr(pos, comma - pos));
            pos = comma + 1;
        }
    }

    auto& stack = mUsingAltScreen ? mPointerShapeStackAlt : mPointerShapeStackMain;

    if (op == '?') {
        // Query each name; respond with CSV of validity / resolved names.
        std::string resp = "\x1b]22;";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) resp += ',';
            std::string_view n = names[i];
            if (n == "__current__") {
                resp += stack.empty() ? std::string{} : stack.back();
            } else if (n == "__default__") {
                // We have no separate "config default" pointer shape; report
                // the platform default ("default" per CSS).
                resp += "default";
            } else if (n.empty() || isKnownPointerShape(n)) {
                resp += "1";
            } else {
                resp += "0";
            }
        }
        resp += "\x1b\\";
        writeToOutput(resp.data(), resp.size());
        return;
    }

    bool changed = false;

    if (op == '<') {
        // Pop one entry; comma list (if any) is ignored, matching kitty.
        if (!stack.empty()) {
            stack.pop_back();
            changed = true;
        }
    } else {
        // '=' with no names → reset to default (clear stack).
        if (op == '=' && names.empty()) {
            if (!stack.empty()) { stack.clear(); changed = true; }
        }
        // '=' or '>': iterate names.
        for (auto n : names) {
            // Empty name with '=' resets to default (clear stack).
            if (n.empty() && op == '=') {
                if (!stack.empty()) { stack.clear(); changed = true; }
                continue;
            }
            if (n.empty()) continue;
            if (op == '=') {
                if (stack.empty()) stack.emplace_back(n);
                else stack.back().assign(n);
            } else { // '>'
                if (stack.size() >= MAX_POINTER_SHAPE_STACK)
                    stack.erase(stack.begin());  // drop oldest
                stack.emplace_back(n);
            }
            changed = true;
        }
    }

    if (changed) notifyPointerShapeChanged();
}
