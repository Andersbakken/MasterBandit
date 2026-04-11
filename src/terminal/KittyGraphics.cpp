// Kitty graphics protocol (APC-based image transmission)
// Spec: https://sw.kovidgoyal.net/kitty/graphics-protocol/

#include "TerminalEmulator.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <stb_image.h>
#include <zlib.h>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace {

struct KittyGraphicsCommand {
    char action = 'T';           // a= (T=transmit+display, t=transmit, p=put, d=delete, q=query)
    char transmissionType = 'd'; // t= (d=direct)
    char compressed = 0;         // o= (z=zlib)
    char deleteAction = 0;       // d= (a=all, i=by id, etc.)
    uint32_t format = 32;        // f= (24=RGB, 32=RGBA, 100=PNG)
    uint32_t more = 0;           // m= (1=more chunks, 0=final)
    uint32_t id = 0;             // i= (client image ID)
    uint32_t placementId = 0;    // p=
    uint32_t quiet = 0;          // q= (0=always respond, 1=errors only, 2=silent)
    uint32_t dataWidth = 0;      // s= (pixel width of transmitted data)
    uint32_t dataHeight = 0;     // v= (pixel height of transmitted data)
    uint32_t xOffset = 0;        // x= (source rect x offset)
    uint32_t yOffset = 0;        // y= (source rect y offset)
    uint32_t width = 0;          // w= (source rect width, 0=full)
    uint32_t height = 0;         // h= (source rect height, 0=full)
    uint32_t cellCols = 0;       // c= (display columns)
    uint32_t cellRows = 0;       // r= (display rows)
    uint32_t cellXOffset = 0;    // X= (pixel offset within cell)
    uint32_t cellYOffset = 0;    // Y= (pixel offset within cell)
    int32_t zIndex = 0;          // z=
    uint32_t cursorMovement = 0; // C= (0=move, 1=don't move)
};

bool parseCommand(std::string_view control, KittyGraphicsCommand& cmd)
{
    // control is everything between 'G' and ';' (or end if no payload)
    // Format: key=value,key=value,...
    size_t pos = 0;
    while (pos < control.size()) {
        if (pos >= control.size()) break;
        char key = control[pos++];
        if (pos >= control.size() || control[pos] != '=') return false;
        pos++; // skip '='

        // Parse value: either a single char (flags) or a number
        size_t valStart = pos;
        while (pos < control.size() && control[pos] != ',')
            pos++;
        std::string_view val = control.substr(valStart, pos - valStart);
        if (pos < control.size()) pos++; // skip ','

        if (val.empty()) continue;

        // Parse integer value
        auto parseUint = [](std::string_view v) -> uint32_t {
            uint32_t r = 0;
            for (char c : v) {
                if (c < '0' || c > '9') return 0;
                r = r * 10 + (c - '0');
            }
            return r;
        };
        auto parseInt = [](std::string_view v) -> int32_t {
            bool neg = false;
            size_t start = 0;
            if (!v.empty() && v[0] == '-') { neg = true; start = 1; }
            int32_t r = 0;
            for (size_t i = start; i < v.size(); i++) {
                if (v[i] < '0' || v[i] > '9') return 0;
                r = r * 10 + (v[i] - '0');
            }
            return neg ? -r : r;
        };

        switch (key) {
        case 'a': cmd.action = val[0]; break;
        case 't': cmd.transmissionType = val[0]; break;
        case 'o': cmd.compressed = val[0]; break;
        case 'd': cmd.deleteAction = val[0]; break;
        case 'f': cmd.format = parseUint(val); break;
        case 'm': cmd.more = parseUint(val); break;
        case 'i': cmd.id = parseUint(val); break;
        case 'p': cmd.placementId = parseUint(val); break;
        case 'q': cmd.quiet = parseUint(val); break;
        case 's': cmd.dataWidth = parseUint(val); break;
        case 'v': cmd.dataHeight = parseUint(val); break;
        case 'x': cmd.xOffset = parseUint(val); break;
        case 'y': cmd.yOffset = parseUint(val); break;
        case 'w': cmd.width = parseUint(val); break;
        case 'h': cmd.height = parseUint(val); break;
        case 'c': cmd.cellCols = parseUint(val); break;
        case 'r': cmd.cellRows = parseUint(val); break;
        case 'X': cmd.cellXOffset = parseUint(val); break;
        case 'Y': cmd.cellYOffset = parseUint(val); break;
        case 'z': cmd.zIndex = parseInt(val); break;
        case 'C': cmd.cursorMovement = parseUint(val); break;
        // Ignored keys: I, U, P, Q, H, V, S, O — deferred features
        default: break;
        }
    }
    return true;
}

// Decompress zlib data
std::vector<uint8_t> zlibDecompress(const uint8_t* data, size_t len)
{
    // Start with 4x estimate, grow as needed
    std::vector<uint8_t> out(len * 4);
    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(len);

    if (inflateInit(&strm) != Z_OK)
        return {};

    size_t totalOut = 0;
    int ret;
    do {
        if (totalOut >= out.size())
            out.resize(out.size() * 2);
        strm.next_out = out.data() + totalOut;
        strm.avail_out = static_cast<uInt>(out.size() - totalOut);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            return {};
        }
        totalOut = strm.total_out;
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    out.resize(totalOut);
    return out;
}

} // anonymous namespace

void TerminalEmulator::processAPC()
{
    // APC format: G<control>;<payload>  or  G<control>
    // mStringSequence contains everything between ESC_ and ST
    if (mStringSequence.empty() || mStringSequence[0] != 'G') {
        // Not a graphics command — silently ignore (other APC protocols may exist)
        return;
    }

    std::string_view seq(mStringSequence);
    seq.remove_prefix(1); // skip 'G'

    // Split control data and payload at ';'
    std::string_view control;
    std::string_view payloadB64;
    size_t semi = seq.find(';');
    if (semi == std::string_view::npos) {
        control = seq;
    } else {
        control = seq.substr(0, semi);
        payloadB64 = seq.substr(semi + 1);
    }

    // Parse command parameters
    KittyGraphicsCommand cmd;
    if (!parseCommand(control, cmd)) {
        spdlog::debug("kitty graphics: failed to parse control data");
        return;
    }

    // Decode base64 payload for this chunk
    std::vector<uint8_t> chunkData;
    if (!payloadB64.empty()) {
        chunkData = base64::decode(payloadB64);
    }

    // Helper to send a response back to the application
    auto sendResponse = [&](uint32_t imgId, const char* msg) {
        if (cmd.quiet == 2) return;
        if (cmd.quiet == 1 && std::strncmp(msg, "OK", 2) == 0) return;
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "\x1b_Gi=%u;%s\x1b\\", imgId, msg);
        if (n > 0) writeToOutput(buf, static_cast<size_t>(n));
    };

    // --- Chunked transfer accumulation ---
    if (cmd.more == 1 || mKittyLoading.active) {
        if (!mKittyLoading.active) {
            // First chunk: save command params
            mKittyLoading = {};
            mKittyLoading.active = true;
            mKittyLoading.id = cmd.id;
            mKittyLoading.placementId = cmd.placementId;
            mKittyLoading.format = cmd.format;
            mKittyLoading.width = cmd.dataWidth;
            mKittyLoading.height = cmd.dataHeight;
            mKittyLoading.xOffset = cmd.xOffset;
            mKittyLoading.yOffset = cmd.yOffset;
            mKittyLoading.cropWidth = cmd.width;
            mKittyLoading.cropHeight = cmd.height;
            mKittyLoading.cellCols = cmd.cellCols;
            mKittyLoading.cellRows = cmd.cellRows;
            mKittyLoading.quiet = cmd.quiet;
            mKittyLoading.zIndex = cmd.zIndex;
            mKittyLoading.cursorMovement = cmd.cursorMovement;
            mKittyLoading.action = cmd.action;
            mKittyLoading.compressed = cmd.compressed;
        }

        // Append this chunk's data
        mKittyLoading.data.insert(mKittyLoading.data.end(), chunkData.begin(), chunkData.end());

        if (cmd.more == 1) {
            // More chunks coming — don't process yet, don't respond
            return;
        }

        // Final chunk (m=0): reconstruct full command from saved state
        chunkData = std::move(mKittyLoading.data);
        cmd.id = mKittyLoading.id;
        cmd.placementId = mKittyLoading.placementId;
        cmd.format = mKittyLoading.format;
        cmd.dataWidth = mKittyLoading.width;
        cmd.dataHeight = mKittyLoading.height;
        cmd.xOffset = mKittyLoading.xOffset;
        cmd.yOffset = mKittyLoading.yOffset;
        cmd.width = mKittyLoading.cropWidth;
        cmd.height = mKittyLoading.cropHeight;
        cmd.cellCols = mKittyLoading.cellCols;
        cmd.cellRows = mKittyLoading.cellRows;
        cmd.quiet = mKittyLoading.quiet;
        cmd.zIndex = mKittyLoading.zIndex;
        cmd.cursorMovement = mKittyLoading.cursorMovement;
        cmd.action = mKittyLoading.action;
        cmd.compressed = mKittyLoading.compressed;
        mKittyLoading.active = false;
    }

    // --- Dispatch on action ---
    switch (cmd.action) {
    case 'T': // transmit + display
    case 't': // transmit only
    case 'q': // query (validate but don't store)
        break;
    case 'p': {
        // Put (display a previously transmitted image)
        auto it = mImageRegistry.find(cmd.id);
        if (it == mImageRegistry.end()) {
            sendResponse(cmd.id, "ENOENT:image not found");
            return;
        }
        const auto& img = it->second;
        int cols = cmd.cellCols > 0 ? static_cast<int>(cmd.cellCols) : static_cast<int>(img.cellWidth);
        int rows = cmd.cellRows > 0 ? static_cast<int>(cmd.cellRows) : static_cast<int>(img.cellHeight);
        placeImageInGrid(cmd.id, cols, rows, cmd.cursorMovement == 0);
        sendResponse(cmd.id, "OK");
        return;
    }
    case 'd': // delete
    {
        char da = cmd.deleteAction ? cmd.deleteAction : 'a';
        switch (da) {
        case 'a': // delete all
        case 'A':
            mImageRegistry.clear();
            break;
        case 'i': // delete by image id
        case 'I':
            if (cmd.id > 0) mImageRegistry.erase(cmd.id);
            break;
        default:
            spdlog::debug("kitty graphics: unhandled delete action '{}'", da);
            break;
        }
        sendResponse(cmd.id, "OK");
        return;
    }
    default:
        // Unrecognized action — silently consume
        spdlog::debug("kitty graphics: unhandled action '{}'", cmd.action);
        sendResponse(cmd.id, "EINVAL:unsupported action");
        return;
    }

    // --- Image transmission (a=T, a=t, a=q) ---

    if (cmd.transmissionType != 'd') {
        // Only direct transmission supported for now
        spdlog::debug("kitty graphics: unsupported transmission type '{}'", cmd.transmissionType);
        sendResponse(cmd.id, "EINVAL:unsupported transmission type");
        return;
    }

    // Decompress if needed
    if (cmd.compressed == 'z') {
        auto decompressed = zlibDecompress(chunkData.data(), chunkData.size());
        if (decompressed.empty()) {
            sendResponse(cmd.id, "EINVAL:zlib decompression failed");
            return;
        }
        chunkData = std::move(decompressed);
    }

    // Decode to RGBA
    int imgW = 0, imgH = 0;
    std::vector<uint8_t> rgba;

    if (cmd.format == 100) {
        // PNG
        int channels;
        uint8_t* pixels = stbi_load_from_memory(
            chunkData.data(), static_cast<int>(chunkData.size()), &imgW, &imgH, &channels, 4);
        if (!pixels) {
            sendResponse(cmd.id, "EINVAL:PNG decode failed");
            return;
        }
        rgba.assign(pixels, pixels + imgW * imgH * 4);
        stbi_image_free(pixels);
    } else if (cmd.format == 32) {
        // RGBA
        imgW = static_cast<int>(cmd.dataWidth);
        imgH = static_cast<int>(cmd.dataHeight);
        if (imgW <= 0 || imgH <= 0) {
            sendResponse(cmd.id, "EINVAL:missing s= or v= for raw format");
            return;
        }
        size_t expected = static_cast<size_t>(imgW) * imgH * 4;
        if (chunkData.size() != expected) {
            sendResponse(cmd.id, "EINVAL:data size mismatch");
            return;
        }
        rgba = std::move(chunkData);
    } else if (cmd.format == 24) {
        // RGB -> RGBA
        imgW = static_cast<int>(cmd.dataWidth);
        imgH = static_cast<int>(cmd.dataHeight);
        if (imgW <= 0 || imgH <= 0) {
            sendResponse(cmd.id, "EINVAL:missing s= or v= for raw format");
            return;
        }
        size_t expected = static_cast<size_t>(imgW) * imgH * 3;
        if (chunkData.size() != expected) {
            sendResponse(cmd.id, "EINVAL:data size mismatch");
            return;
        }
        rgba.resize(static_cast<size_t>(imgW) * imgH * 4);
        for (size_t i = 0, j = 0; i < chunkData.size(); i += 3, j += 4) {
            rgba[j]     = chunkData[i];
            rgba[j + 1] = chunkData[i + 1];
            rgba[j + 2] = chunkData[i + 2];
            rgba[j + 3] = 255;
        }
    } else {
        sendResponse(cmd.id, "EINVAL:unsupported format");
        return;
    }

    // Query mode: just validate and respond, don't store
    if (cmd.action == 'q') {
        sendResponse(cmd.id, "OK");
        return;
    }

    // Calculate cell dimensions
    float cw = mCallbacks.cellPixelWidth ? mCallbacks.cellPixelWidth() : 0.0f;
    float ch = mCallbacks.cellPixelHeight ? mCallbacks.cellPixelHeight() : 0.0f;
    if (cw <= 0 || ch <= 0) {
        sendResponse(cmd.id, "EINVAL:cell dimensions unavailable");
        return;
    }

    int cellCols, cellRows;
    if (cmd.cellCols > 0) {
        cellCols = static_cast<int>(cmd.cellCols);
    } else {
        cellCols = std::max(1, static_cast<int>(std::ceil(static_cast<float>(imgW) / cw)));
    }
    if (cmd.cellRows > 0) {
        cellRows = static_cast<int>(cmd.cellRows);
    } else {
        cellRows = std::max(1, static_cast<int>(std::ceil(static_cast<float>(imgH) / ch)));
    }

    // Assign image ID if client didn't provide one
    uint32_t imageId = cmd.id;
    if (imageId == 0) {
        imageId = mNextImageId++;
    }

    // Store in registry
    ImageEntry entry;
    entry.id = imageId;
    entry.pixelWidth = static_cast<uint32_t>(imgW);
    entry.pixelHeight = static_cast<uint32_t>(imgH);
    entry.cellWidth = static_cast<uint32_t>(cellCols);
    entry.cellHeight = static_cast<uint32_t>(cellRows);
    entry.rgba = std::move(rgba);

    spdlog::debug("kitty graphics: image id={} {}x{} px, {}x{} cells, action={}",
                  imageId, imgW, imgH, cellCols, cellRows, cmd.action);
    mImageRegistry[imageId] = std::move(entry);

    // Place in grid if action is transmit+display
    if (cmd.action == 'T') {
        placeImageInGrid(imageId, cellCols, cellRows, cmd.cursorMovement == 0);
    }

    sendResponse(imageId, "OK");
}
