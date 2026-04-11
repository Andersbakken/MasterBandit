#include "TerminalEmulator.h"
#include "Utils.h"
#include <spdlog/spdlog.h>

#include <sys/time.h>
#include <string.h>
#include <cmath>
#include <algorithm>
#include <vector>

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
void TerminalEmulator::processOSC_iTerm(std::string_view payload)
{
    // Format: "File=[params]:base64data"
    if (payload.substr(0, 5) != "File=") return;

    size_t colonPos = payload.find(':');
    if (colonPos == std::string_view::npos) return;

    std::string_view paramStr = payload.substr(5, colonPos - 5);
    std::string_view b64data = payload.substr(colonPos + 1);

    // Parse params
    bool isInline = false;
    std::string_view::size_type pos = 0;
    while (pos < paramStr.size()) {
        auto eq = paramStr.find('=', pos);
        if (eq == std::string_view::npos) break;
        auto semi = paramStr.find(';', eq);
        if (semi == std::string_view::npos) semi = paramStr.size();

        std::string_view key = paramStr.substr(pos, eq - pos);
        std::string_view val = paramStr.substr(eq + 1, semi - eq - 1);

        if (key == "inline" && val == "1") isInline = true;
        pos = semi + 1;
    }

    if (!isInline) return;

    // Decode base64
    std::vector<uint8_t> imageBytes = base64::decode(b64data);
    if (imageBytes.empty()) return;

    // Decode image
    int w, h, channels;
    uint8_t* pixels = stbi_load_from_memory(
        imageBytes.data(), static_cast<int>(imageBytes.size()), &w, &h, &channels, 4);
    if (!pixels) {
        spdlog::debug("OSC 1337: stbi_load_from_memory failed");
        return;
    }

    float cw = mCallbacks.cellPixelWidth ? mCallbacks.cellPixelWidth() : 0.0f;
    float ch = mCallbacks.cellPixelHeight ? mCallbacks.cellPixelHeight() : 0.0f;
    if (cw <= 0 || ch <= 0) {
        stbi_image_free(pixels);
        return;
    }

    int cellCols = std::max(1, static_cast<int>(std::ceil(static_cast<float>(w) / cw)));
    int cellRows = std::max(1, static_cast<int>(std::ceil(static_cast<float>(h) / ch)));

    ImageEntry entry;
    entry.id = mNextImageId++;
    entry.pixelWidth = w;
    entry.pixelHeight = h;
    entry.cellWidth = cellCols;
    entry.cellHeight = cellRows;
    entry.rgba.assign(pixels, pixels + w * h * 4);
    stbi_image_free(pixels);

    uint32_t imageId = entry.id;
    spdlog::warn("OSC 1337: image id={} {}x{} px, {}x{} cells",
                 imageId, w, h, cellCols, cellRows);
    mImageRegistry[imageId] = std::move(entry);

    placeImageInGrid(imageId, cellCols, cellRows);
}

void TerminalEmulator::placeImageInGrid(uint32_t imageId, int cellCols, int cellRows, bool moveCursor)
{
    IGrid& g = grid();
    int fillCols = std::min(cellCols, mWidth);

    int savedX = mCursorX, savedY = mCursorY;

    for (int r = 0; r < cellRows; ++r) {
        // Scroll if cursor is at the bottom
        if (mCursorY >= mHeight) {
            scrollUpInRegion(1);
            mCursorY = mHeight - 1;
            savedY--;  // track scroll for restore
        }

        // Fill cells with blanks to reserve visual space
        for (int c = 0; c < fillCols; ++c) {
            Cell& cell = g.cell(c, mCursorY);
            cell.wc = 0;
            cell.attrs = CellAttrs{};
        }

        // Place one extra at column 0 with image ID and row offset
        CellExtra& ex = g.ensureExtra(0, mCursorY);
        ex.imageId = imageId;
        ex.imageOffsetCol = 0;
        ex.imageOffsetRow = r;

        g.markRowDirty(mCursorY);
        mCursorY++;
    }

    if (!moveCursor) {
        mCursorX = std::max(0, savedX);
        mCursorY = std::max(0, savedY);
    } else {
        // Match kitty: cursor goes to last row of image, column past right edge.
        // The caller (or newline from the app) handles advancing to the next row.
        mCursorY--;
        if (mCursorY >= mHeight) {
            mCursorY = mHeight - 1;
        }
        mCursorX = std::min(fillCols, mWidth - 1);
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
        if (mCallbacks.onIconChanged) mCallbacks.onIconChanged(std::string(payload));
        break;
    case 2: processOSC_Title(payload, true); break;
    case 133: { // Shell integration (FinalTerm / semantic prompts)
        // OSC 133;X where X is: A=prompt start, B=command start, C=output start, D=command done
        // Optional params after X separated by ; with key=value pairs
        if (!payload.empty()) {
            char kind = payload[0];
            bool isSecondary = false;
            // Check for k=s (secondary prompt) in params
            if (payload.size() > 1 && payload[1] == ';') {
                auto params = payload.substr(2);
                if (params.find("k=s") != std::string_view::npos) isSecondary = true;
            }
            IGrid& g = grid();
            switch (kind) {
            case 'A': // Prompt start
                if (mCursorY >= 0 && mCursorY < mHeight) {
                    mDocument.setRowPromptKind(mCursorY,
                        isSecondary ? Document::SecondaryPrompt : Document::PromptStart);
                }
                break;
            case 'B': // Command input start (after prompt)
                if (mCursorY >= 0 && mCursorY < mHeight) {
                    mDocument.setRowPromptKind(mCursorY, Document::CommandStart);
                }
                break;
            case 'C': // Command output start
                if (mCursorY >= 0 && mCursorY < mHeight) {
                    mDocument.setRowPromptKind(mCursorY, Document::OutputStart);
                }
                break;
            case 'D': // Command finished (with exit code)
                // Could store exit code from params, for now just ignore
                break;
            }
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
        if (mCallbacks.onCWDChanged) {
            std::string_view url = payload;
            // Strip file:// prefix and optional hostname
            if (url.substr(0, 7) == "file://") {
                url.remove_prefix(7);
                auto slash = url.find('/');
                if (slash != std::string_view::npos)
                    url.remove_prefix(slash);
            }
            mCallbacks.onCWDChanged(std::string(url));
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
        if (mCallbacks.onTitleChanged) mCallbacks.onTitleChanged(std::string(text));
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
