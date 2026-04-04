#include "TerminalEmulator.h"
#include "Utils.h"
#include "Log.h"
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
    unsigned long long machtime = mach_absolute_time();
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

unsigned long long TerminalEmulator::mono()
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
        DEBUG("OSC 1337: stbi_load_from_memory failed");
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

void TerminalEmulator::placeImageInGrid(uint32_t imageId, int cellCols, int cellRows)
{
    IGrid& g = grid();
    int fillCols = std::min(cellCols, mWidth);

    for (int r = 0; r < cellRows; ++r) {
        // Scroll if cursor is at the bottom
        if (mCursorY >= mHeight) {
            scrollUpInRegion(1);
            mCursorY = mHeight - 1;
        }

        // Fill cells with blanks to reserve visual space
        for (int c = 0; c < fillCols; ++c) {
            Cell& cell = g.cell(c, mCursorY);
            cell.wc = 0;
            cell.attrs.setWideSpacer(true);
        }

        // Place one extra at column 0 with image ID and row offset
        CellExtra& ex = g.ensureExtra(0, mCursorY);
        ex.imageId = imageId;
        ex.imageOffsetCol = 0;
        ex.imageOffsetRow = r;

        g.markRowDirty(mCursorY);
        mCursorY++;
    }

    if (mCursorY >= mHeight) {
        mCursorY = mHeight - 1;
    }
    mCursorX = 0;
}

// --- OSC processing ---
void TerminalEmulator::processStringSequence()
{
    if (mStringSequenceType != OSX) {
        WARN("Ignoring non-OSC string sequence type 0x%x (len=%zu)", mStringSequenceType, mStringSequence.size());
        return;
    }

    // mStringSequence format: "<number>;<payload>" or just "<number>"
    size_t semi = mStringSequence.find(';');
    if (semi == std::string::npos) return;

    int oscNum = 0;
    for (size_t i = 0; i < semi; ++i) {
        char c = mStringSequence[i];
        if (c < '0' || c > '9') return;
        oscNum = oscNum * 10 + (c - '0');
    }
    std::string_view payload(mStringSequence.data() + semi + 1, mStringSequence.size() - semi - 1);

    switch (oscNum) {
    case 0: processOSC_Title(payload, true); break;
    case 1:
        if (mCallbacks.onIconChanged) mCallbacks.onIconChanged(std::string(payload));
        break;
    case 2: processOSC_Title(payload, true); break;
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
    case 999:
        if (mCallbacks.onOSCMB) mCallbacks.onOSCMB(payload);
        break;
    case 1337: processOSC_iTerm(payload); break;
    default:
        WARN("Ignoring OSC %d", oscNum);
        break;
    }
}

void TerminalEmulator::processOSC_Title(std::string_view text, bool setTitle)
{
    if (setTitle) {
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
