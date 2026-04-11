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
#include <unordered_set>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

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
    int32_t zIndex = 0;          // z= (also gap in ms for a=f)
    uint32_t cursorMovement = 0; // C= (0=move, 1=don't move)
    uint32_t imageNumber = 0;    // I= (non-unique image number)
    // Note: for animation commands, keys are reused contextually:
    //   a=f: z=gap(ms), r=frame_number, s=data_width, v=data_height
    //   a=a: s=animation_state(1/2/3), v=loop_count, r=frame_number, z=gap
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
        case 'I': cmd.imageNumber = parseUint(val); break;
        // Ignored keys: U, P, Q, H, V, S, O — deferred features
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

// Decode raw/compressed image data to RGBA. Returns empty on failure.
struct DecodeResult {
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    std::string error;
};

DecodeResult decodeImageData(std::vector<uint8_t>& data, char compressed,
                              uint32_t format, uint32_t dataWidth, uint32_t dataHeight)
{
    DecodeResult result;

    // Decompress if needed
    if (compressed == 'z') {
        auto decompressed = zlibDecompress(data.data(), data.size());
        if (decompressed.empty()) {
            result.error = "EINVAL:zlib decompression failed";
            return result;
        }
        data = std::move(decompressed);
    }

    if (format == 100) {
        int channels;
        uint8_t* pixels = stbi_load_from_memory(
            data.data(), static_cast<int>(data.size()), &result.width, &result.height, &channels, 4);
        if (!pixels) {
            result.error = "EINVAL:PNG decode failed";
            return result;
        }
        result.rgba.assign(pixels, pixels + result.width * result.height * 4);
        stbi_image_free(pixels);
    } else if (format == 32) {
        result.width = static_cast<int>(dataWidth);
        result.height = static_cast<int>(dataHeight);
        if (result.width <= 0 || result.height <= 0) {
            result.error = "EINVAL:missing s= or v= for raw format";
            return result;
        }
        size_t expected = static_cast<size_t>(result.width) * result.height * 4;
        if (data.size() != expected) {
            result.error = "EINVAL:data size mismatch";
            return result;
        }
        result.rgba = std::move(data);
    } else if (format == 24) {
        result.width = static_cast<int>(dataWidth);
        result.height = static_cast<int>(dataHeight);
        if (result.width <= 0 || result.height <= 0) {
            result.error = "EINVAL:missing s= or v= for raw format";
            return result;
        }
        size_t expected = static_cast<size_t>(result.width) * result.height * 3;
        if (data.size() != expected) {
            result.error = "EINVAL:data size mismatch";
            return result;
        }
        result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4);
        for (size_t i = 0, j = 0; i < data.size(); i += 3, j += 4) {
            result.rgba[j]     = data[i];
            result.rgba[j + 1] = data[i + 1];
            result.rgba[j + 2] = data[i + 2];
            result.rgba[j + 3] = 255;
        }
    } else {
        result.error = "EINVAL:unsupported format";
    }
    return result;
}

} // anonymous namespace

// Find an image by image number (I=). Returns the most recently created match.
uint32_t TerminalEmulator::findImageByNumber(uint32_t number) const
{
    uint32_t bestId = 0;
    for (const auto& [id, img] : mImageRegistry) {
        if (img.imageNumber == number && id > bestId)
            bestId = id;
    }
    return bestId;
}

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
    // Response logic matching kitty's finish_command_response():
    // - No id → no response
    // - q>=1: suppress OK responses
    // - q>=2: suppress all responses
    // - OK only sent if dataLoaded is true (a=a doesn't load data → no OK)
    auto sendResponse = [&](uint32_t imgId, const char* msg, bool dataLoaded = true) {
        if (imgId == 0) return;
        bool isOk = std::strncmp(msg, "OK", 2) == 0;
        if (cmd.quiet >= 2) return;
        if (cmd.quiet >= 1 && isOk) return;
        if (isOk && !dataLoaded) return;
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

    // --- Resolve payload for file/shm transmission types ---
    if (cmd.transmissionType == 'f' || cmd.transmissionType == 't') {
        std::string path(chunkData.begin(), chunkData.end());
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            sendResponse(cmd.id, "ENOENT:cannot open file");
            return;
        }
        struct stat st;
        if (fstat(fd, &st) < 0 || st.st_size <= 0) {
            close(fd);
            sendResponse(cmd.id, "EINVAL:cannot stat file");
            return;
        }
        chunkData.resize(static_cast<size_t>(st.st_size));
        ssize_t n = read(fd, chunkData.data(), chunkData.size());
        close(fd);
        if (cmd.transmissionType == 't') unlink(path.c_str());
        if (n <= 0) {
            sendResponse(cmd.id, "EINVAL:cannot read file");
            return;
        }
        chunkData.resize(static_cast<size_t>(n));
    } else if (cmd.transmissionType == 's') {
        std::string name(chunkData.begin(), chunkData.end());
        int fd = shm_open(name.c_str(), O_RDONLY, 0);
        if (fd < 0) {
            sendResponse(cmd.id, "ENOENT:cannot open shared memory");
            return;
        }
        struct stat st;
        if (fstat(fd, &st) < 0 || st.st_size <= 0) {
            close(fd);
            sendResponse(cmd.id, "EINVAL:cannot stat shared memory");
            return;
        }
        size_t mapSize = static_cast<size_t>(st.st_size);
        void* ptr = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        shm_unlink(name.c_str());
        if (ptr == MAP_FAILED) {
            sendResponse(cmd.id, "EINVAL:mmap failed");
            return;
        }
        chunkData.assign(static_cast<uint8_t*>(ptr), static_cast<uint8_t*>(ptr) + mapSize);
        munmap(ptr, mapSize);
    }

    // Reject if both i= and I= are set (kitty does the same)
    if (cmd.id > 0 && cmd.imageNumber > 0) {
        sendResponse(cmd.id, "EINVAL:must not specify both i and I");
        return;
    }

    // Resolve image ID: i= takes priority, then I= (by number lookup), then last image
    auto resolveId = [&]() -> uint32_t {
        if (cmd.id > 0) return cmd.id;
        if (cmd.imageNumber > 0) return findImageByNumber(cmd.imageNumber);
        return mLastKittyImageId;
    };

    // --- Dispatch on action ---
    switch (cmd.action) {
    case 'T': // transmit + display
    case 't': // transmit only
    case 'q': // query (validate but don't store)
        break;
    case 'p': {
        // Put (display a previously transmitted image at a new or existing placement)
        uint32_t targetId = resolveId();
        auto it = mImageRegistry.find(targetId);
        if (it == mImageRegistry.end()) {
            sendResponse(targetId, "ENOENT:image not found");
            return;
        }
        auto& img = it->second;
        int cols = cmd.cellCols > 0 ? static_cast<int>(cmd.cellCols) : static_cast<int>(img.cellWidth);
        int rows = cmd.cellRows > 0 ? static_cast<int>(cmd.cellRows) : static_cast<int>(img.cellHeight);

        // Store per-placement display params
        ImageEntry::Placement pl;
        pl.cellWidth = static_cast<uint32_t>(cols);
        pl.cellHeight = static_cast<uint32_t>(rows);
        pl.cropX = cmd.xOffset; pl.cropY = cmd.yOffset;
        pl.cropW = cmd.width;   pl.cropH = cmd.height;
        pl.cellXOffset = cmd.cellXOffset;
        pl.cellYOffset = cmd.cellYOffset;
        img.placements[cmd.placementId] = pl;

        placeImageInGrid(targetId, cmd.placementId, cols, rows, cmd.cursorMovement == 0);
        sendResponse(targetId, "OK");
        return;
    }
    case 'd': // delete
    {
        char da = cmd.deleteAction ? cmd.deleteAction : 'a';
        switch (da) {
        case 'a': // delete all visible placements
        case 'A':
        {
            // Collect image IDs that are visible in the current grid
            std::unordered_set<uint32_t> visibleIds;
            IGrid& dg = grid();
            for (int r = 0; r < mHeight; r++) {
                for (int c = 0; c < mWidth; c++) {
                    const CellExtra* cex = dg.getExtra(c, r);
                    if (cex && cex->imageId > 0) {
                        visibleIds.insert(cex->imageId);
                        break;
                    }
                }
            }
            // Clear cell extras for visible images
            for (int r = 0; r < mHeight; r++) {
                for (int c = 0; c < mWidth; c++) {
                    const CellExtra* cex = dg.getExtra(c, r);
                    if (cex && cex->imageId > 0) {
                        dg.clearRowExtras(r);
                        break;
                    }
                }
            }
            // For uppercase (free), also delete the image data
            if (da == 'A') {
                for (uint32_t vid : visibleIds)
                    mImageRegistry.erase(vid);
            }
            break;
        }
        case 'i': // delete by image id (optionally by placement)
        case 'I':
            if (cmd.id > 0) {
                if (cmd.placementId > 0) {
                    // Delete specific placement only
                    auto imgIt = mImageRegistry.find(cmd.id);
                    if (imgIt != mImageRegistry.end()) {
                        imgIt->second.placements.erase(cmd.placementId);
                        // Clear cells for this placement
                        IGrid& dg = grid();
                        for (int r = 0; r < mHeight; r++) {
                            for (int c = 0; c < mWidth; c++) {
                                const CellExtra* cex = dg.getExtra(c, r);
                                if (cex && cex->imageId == cmd.id && cex->imagePlacementId == cmd.placementId) {
                                    dg.clearExtra(c, r);
                                    dg.markRowDirty(r);
                                }
                            }
                        }
                        // If uppercase and no placements remain, free image data
                        if (da == 'I' && imgIt->second.placements.empty())
                            mImageRegistry.erase(imgIt);
                    }
                } else {
                    // Delete all placements of this image
                    mImageRegistry.erase(cmd.id);
                    // Clear cells referencing this image
                    IGrid& dg = grid();
                    for (int r = 0; r < mHeight; r++) {
                        for (int c = 0; c < mWidth; c++) {
                            const CellExtra* cex = dg.getExtra(c, r);
                            if (cex && cex->imageId == cmd.id) {
                                dg.clearExtra(c, r);
                                dg.markRowDirty(r);
                            }
                        }
                    }
                }
            }
            break;
        case 'n': // delete by image number (optionally by placement)
        case 'N':
            if (cmd.imageNumber > 0) {
                uint32_t found = findImageByNumber(cmd.imageNumber);
                if (found > 0) {
                    if (cmd.placementId > 0) {
                        auto imgIt = mImageRegistry.find(found);
                        if (imgIt != mImageRegistry.end()) {
                            imgIt->second.placements.erase(cmd.placementId);
                            IGrid& dg = grid();
                            for (int r = 0; r < mHeight; r++) {
                                for (int c = 0; c < mWidth; c++) {
                                    const CellExtra* cex = dg.getExtra(c, r);
                                    if (cex && cex->imageId == found && cex->imagePlacementId == cmd.placementId) {
                                        dg.clearExtra(c, r);
                                        dg.markRowDirty(r);
                                    }
                                }
                            }
                            if (da == 'N' && imgIt->second.placements.empty())
                                mImageRegistry.erase(imgIt);
                        }
                    } else {
                        mImageRegistry.erase(found);
                        IGrid& dg = grid();
                        for (int r = 0; r < mHeight; r++) {
                            for (int c = 0; c < mWidth; c++) {
                                const CellExtra* cex = dg.getExtra(c, r);
                                if (cex && cex->imageId == found) {
                                    dg.clearExtra(c, r);
                                    dg.markRowDirty(r);
                                }
                            }
                        }
                    }
                }
            }
            break;
        case 'f': // delete animation frames for an image
        case 'F':
        {
            uint32_t targetId = resolveId();
            auto fit = mImageRegistry.find(targetId);
            if (fit != mImageRegistry.end()) {
                fit->second.extraFrames.clear();
                fit->second.currentFrameIndex = 0;
                fit->second.animationState = ImageEntry::Stopped;
                fit->second.frameGeneration++;
            }
            break;
        }
        case 'p': case 'P': // delete by placement position (needs placement tracking)
        case 'q': case 'Q': // delete by position + z-index
        case 'c': case 'C': // delete at cursor position
        case 'x': case 'X': // delete by column
        case 'y': case 'Y': // delete by row
        case 'z': case 'Z': // delete by z-index
        case 'r': case 'R': // delete by ID range
            spdlog::debug("kitty graphics: unhandled delete action '{}'", da);
            break;
        default:
            spdlog::debug("kitty graphics: unknown delete action '{}'", da);
            break;
        }
        // kitty sends no response for a=d (delete)
        return;
    }
    case 'f': // animation frame load
    {
        uint32_t targetId = resolveId();
        auto it = mImageRegistry.find(targetId);
        if (it == mImageRegistry.end()) {
            sendResponse(targetId, "ENOENT:image not found");
            return;
        }
        auto& img = it->second;

        auto decoded = decodeImageData(chunkData, cmd.compressed,
                                        cmd.format, cmd.dataWidth, cmd.dataHeight);
        if (!decoded.error.empty()) {
            sendResponse(cmd.id, decoded.error.c_str());
            return;
        }

        uint32_t gap = cmd.zIndex > 0 ? static_cast<uint32_t>(cmd.zIndex) : 40;
        bool overwrite = (cmd.cursorMovement == 1); // C=1 means overwrite for a=f

        // Determine base frame for compositing:
        // c= (cellCols) specifies compose_onto frame (1-based). 0 = previous frame.
        uint32_t composeOnto = cmd.cellCols; // c= reused as compose_onto for a=f

        ImageEntry::Frame frame;
        frame.gap = gap;
        if (static_cast<uint32_t>(decoded.width) == img.pixelWidth &&
            static_cast<uint32_t>(decoded.height) == img.pixelHeight &&
            cmd.xOffset == 0 && cmd.yOffset == 0) {
            // Full-size frame at origin — store directly
            frame.rgba = std::move(decoded.rgba);
        } else {
            // Get the base frame to composite onto
            if (composeOnto == 0 && !img.extraFrames.empty()) {
                // Default: use previous frame
                frame.rgba = img.extraFrames.back().rgba;
            } else if (composeOnto == 1) {
                // Frame 1 = root frame
                frame.rgba = img.rgba;
            } else if (composeOnto >= 2 && composeOnto - 2 < img.extraFrames.size()) {
                frame.rgba = img.extraFrames[composeOnto - 2].rgba;
            } else {
                // Fallback to root
                frame.rgba = img.rgba;
            }

            int srcW = decoded.width, srcH = decoded.height;
            int dstW = static_cast<int>(img.pixelWidth);
            int ox = static_cast<int>(cmd.xOffset);
            int oy = static_cast<int>(cmd.yOffset);
            for (int y = 0; y < srcH; y++) {
                int dy = oy + y;
                if (dy < 0 || dy >= static_cast<int>(img.pixelHeight)) continue;
                for (int x = 0; x < srcW; x++) {
                    int dx = ox + x;
                    if (dx < 0 || dx >= dstW) continue;
                    size_t si = (static_cast<size_t>(y) * srcW + x) * 4;
                    size_t di = (static_cast<size_t>(dy) * dstW + dx) * 4;
                    if (overwrite) {
                        std::memcpy(&frame.rgba[di], &decoded.rgba[si], 4);
                    } else {
                        // Alpha-over compositing
                        uint8_t sa = decoded.rgba[si + 3];
                        if (sa == 255) {
                            std::memcpy(&frame.rgba[di], &decoded.rgba[si], 4);
                        } else if (sa > 0) {
                            for (int c = 0; c < 3; c++) {
                                uint8_t sc = decoded.rgba[si + c];
                                uint8_t dc = frame.rgba[di + c];
                                frame.rgba[di + c] = static_cast<uint8_t>(
                                    (sc * sa + dc * (255 - sa)) / 255);
                            }
                            uint8_t da = frame.rgba[di + 3];
                            frame.rgba[di + 3] = static_cast<uint8_t>(
                                sa + da * (255 - sa) / 255);
                        }
                    }
                }
            }
        }

        img.extraFrames.push_back(std::move(frame));
        sendResponse(targetId, "OK");
        return;
    }
    case 'a': // animation control
    {
        uint32_t targetId = resolveId();
        auto it = mImageRegistry.find(targetId);
        if (it == mImageRegistry.end()) {
            // Silently ignore — client may have exited
            return;
        }
        auto& img = it->second;

        // s= (dataWidth) is animation_state: 1=stop, 2=loading, 3=running
        if (cmd.dataWidth >= 1 && cmd.dataWidth <= 3) {
            auto newState = static_cast<ImageEntry::AnimState>(cmd.dataWidth - 1);
            if (newState == ImageEntry::Running && img.animationState != ImageEntry::Running) {
                img.currentFrameIndex = 0;
                img.currentLoop = 0;
                img.frameShownAt = mono();
            }
            img.animationState = newState;
        }

        // v= (dataHeight) is loop_count
        if (cmd.dataHeight > 0) {
            img.maxLoops = cmd.dataHeight - 1;
        }

        // z= (zIndex) is gap override for a specific frame
        if (cmd.zIndex > 0 && cmd.cellRows > 0) {
            // r= is frame_number (1-based), z= is new gap
            uint32_t frameNum = cmd.cellRows;
            uint32_t newGap = static_cast<uint32_t>(cmd.zIndex);
            if (frameNum == 1) {
                img.rootFrameGap = newGap;
            } else if (frameNum - 2 < img.extraFrames.size()) {
                img.extraFrames[frameNum - 2].gap = newGap;
            }
        }

        // kitty sends no response for a=a (animation control)
        return;
    }
    case 'c': // frame composition (not yet supported)
        // Silently consume
        return;
    default:
        // Unrecognized action — silently consume
        spdlog::debug("kitty graphics: unhandled action '{}'", cmd.action);
        return;
    }

    // --- Image transmission (a=T, a=t, a=q) ---

    auto decoded = decodeImageData(chunkData, cmd.compressed,
                                    cmd.format, cmd.dataWidth, cmd.dataHeight);
    if (!decoded.error.empty()) {
        sendResponse(cmd.id, decoded.error.c_str());
        return;
    }

    if (cmd.action == 'q') {
        sendResponse(cmd.id, "OK");
        return;
    }

    int imgW = decoded.width, imgH = decoded.height;
    std::vector<uint8_t> rgba = std::move(decoded.rgba);

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
        if (cmd.imageNumber > 0) {
            // I= without i= — auto-assign an id
            imageId = mNextImageId++;
        } else {
            imageId = mNextImageId++;
        }
    }

    // Store in registry
    ImageEntry entry;
    entry.id = imageId;
    entry.imageNumber = cmd.imageNumber;
    entry.pixelWidth = static_cast<uint32_t>(imgW);
    entry.pixelHeight = static_cast<uint32_t>(imgH);
    entry.cellWidth = static_cast<uint32_t>(cellCols);
    entry.cellHeight = static_cast<uint32_t>(cellRows);
    entry.cropX = cmd.xOffset;
    entry.cropY = cmd.yOffset;
    entry.cropW = cmd.width;
    entry.cropH = cmd.height;
    entry.rgba = std::move(rgba);

    spdlog::debug("kitty graphics: image id={} {}x{} px, {}x{} cells, action={}, t={}",
                  imageId, imgW, imgH, cellCols, cellRows, cmd.action, cmd.transmissionType);
    mImageRegistry[imageId] = std::move(entry);
    mLastKittyImageId = imageId;

    // Place in grid if action is transmit+display
    if (cmd.action == 'T') {
        ImageEntry::Placement pl;
        pl.cellWidth = static_cast<uint32_t>(cellCols);
        pl.cellHeight = static_cast<uint32_t>(cellRows);
        pl.cropX = cmd.xOffset; pl.cropY = cmd.yOffset;
        pl.cropW = cmd.width;   pl.cropH = cmd.height;
        pl.cellXOffset = cmd.cellXOffset;
        pl.cellYOffset = cmd.cellYOffset;
        mImageRegistry[imageId].placements[cmd.placementId] = pl;
        placeImageInGrid(imageId, cmd.placementId, cellCols, cellRows, cmd.cursorMovement == 0);
    }

    // Use cmd.id for response — if client didn't set i= (cmd.id==0), no response
    // (matches kitty: finish_command_response checks g->id || g->image_number)
    sendResponse(cmd.id, "OK");
}

void TerminalEmulator::tickAnimations()
{
    uint64_t now = mono();

    for (auto& [id, img] : mImageRegistry) {
        if (!img.hasAnimation()) continue;

        uint32_t gap = img.currentFrameGap();
        if (gap == 0) gap = 40; // default 40ms

        uint64_t elapsed = now - img.frameShownAt;

        if (elapsed < gap) continue;

        // Advance frame(s), potentially skipping if multiple gaps elapsed
        uint32_t totalFrames = 1 + static_cast<uint32_t>(img.extraFrames.size());
        bool stopped = false;

        while (elapsed >= gap && !stopped) {
            elapsed -= gap;

            uint32_t next = (img.currentFrameIndex + 1) % totalFrames;
            if (next == 0) {
                // Wrapped around
                if (img.maxLoops > 0 && ++img.currentLoop >= img.maxLoops) {
                    img.animationState = ImageEntry::Stopped;
                    stopped = true;
                    break;
                }
            }
            img.currentFrameIndex = next;
            gap = img.currentFrameGap();
            if (gap == 0) gap = 40;
        }

        img.frameShownAt = now;
        img.frameGeneration++;
    }
}

