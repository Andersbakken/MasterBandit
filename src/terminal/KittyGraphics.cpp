// Kitty graphics protocol (APC-based image transmission)
// Spec: https://sw.kovidgoyal.net/kitty/graphics-protocol/

#include "TerminalEmulator.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <stb_image.h>
#include <zlib.h>

#include <cmath>
#include <algorithm>
#include <cerrno>
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
    uint32_t dataSize = 0;       // S= (bytes to read from file/shm, 0=all)
    uint32_t dataOffset = 0;     // O= (byte offset into file/shm)
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
        case 'S': cmd.dataSize = parseUint(val); break;
        case 'O': cmd.dataOffset = parseUint(val); break;
        // Ignored keys: U, P, Q, H, V — deferred features
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
        if (img->imageNumber == number && id > bestId)
            bestId = id;
    }
    return bestId;
}

bool TerminalEmulator::setImageFrameShownAtForTest(uint32_t id, uint64_t t)
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    auto it = mImageRegistry.find(id);
    if (it == mImageRegistry.end() || !it->second) return false;
    it->second->frameShownAt = t;
    return true;
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
    // - No id and no image number → no response
    // - q>=1: suppress OK responses
    // - q>=2: suppress all responses
    // - OK only sent if dataLoaded is true (a=a doesn't load data → no OK)
    // - Format: "_Gi=<id>[,I=<n>][,p=<p>][,r=<frame>];<msg>\e\\"
    auto sendResponse = [&](uint32_t imgId, const char* msg, bool dataLoaded = true) {
        if (imgId == 0 && cmd.imageNumber == 0) return;
        bool isOk = std::strncmp(msg, "OK", 2) == 0;
        if (cmd.quiet >= 2) return;
        if (cmd.quiet >= 1 && isOk) return;
        if (isOk && !dataLoaded) return;
        char buf[256];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\x1b_G");
        if (imgId > 0)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "i=%u", imgId);
        if (cmd.imageNumber > 0)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%sI=%u",
                            pos > 3 ? "," : "", cmd.imageNumber);
        if (cmd.placementId > 0)
            pos += snprintf(buf + pos, sizeof(buf) - pos, ",p=%u", cmd.placementId);
        if (cmd.cellRows > 0 && (cmd.action == 'f' || cmd.action == 'a'))
            pos += snprintf(buf + pos, sizeof(buf) - pos, ",r=%u", cmd.cellRows);
        pos += snprintf(buf + pos, sizeof(buf) - pos, ";%s\x1b\\", msg);
        if (pos > 0 && pos < (int)sizeof(buf))
            writeToOutput(buf, static_cast<size_t>(pos));
    };

    // --- Chunked transfer accumulation ---
    if (cmd.more == 1 || mKittyLoading.active) {
        if (!mKittyLoading.active) {
            // First chunk: save command params
            mKittyLoading = {};
            mKittyLoading.active = true;
            mKittyLoading.id = cmd.id;
            mKittyLoading.imageNumber = cmd.imageNumber;
            mKittyLoading.placementId = cmd.placementId;
            mKittyLoading.format = cmd.format;
            mKittyLoading.width = cmd.dataWidth;
            mKittyLoading.height = cmd.dataHeight;
            mKittyLoading.xOffset = cmd.xOffset;
            mKittyLoading.yOffset = cmd.yOffset;
            mKittyLoading.cellXOffset = cmd.cellXOffset;
            mKittyLoading.cellYOffset = cmd.cellYOffset;
            mKittyLoading.cropWidth = cmd.width;
            mKittyLoading.cropHeight = cmd.height;
            mKittyLoading.cellCols = cmd.cellCols;
            mKittyLoading.cellRows = cmd.cellRows;
            mKittyLoading.quiet = cmd.quiet;
            mKittyLoading.zIndex = cmd.zIndex;
            mKittyLoading.cursorMovement = cmd.cursorMovement;
            mKittyLoading.dataSize = cmd.dataSize;
            mKittyLoading.dataOffset = cmd.dataOffset;
            mKittyLoading.action = cmd.action;
            mKittyLoading.compressed = cmd.compressed;
            mKittyLoading.transmissionType = cmd.transmissionType;
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
        cmd.imageNumber = mKittyLoading.imageNumber;
        cmd.placementId = mKittyLoading.placementId;
        cmd.format = mKittyLoading.format;
        cmd.dataWidth = mKittyLoading.width;
        cmd.dataHeight = mKittyLoading.height;
        cmd.xOffset = mKittyLoading.xOffset;
        cmd.yOffset = mKittyLoading.yOffset;
        cmd.cellXOffset = mKittyLoading.cellXOffset;
        cmd.cellYOffset = mKittyLoading.cellYOffset;
        cmd.width = mKittyLoading.cropWidth;
        cmd.height = mKittyLoading.cropHeight;
        cmd.cellCols = mKittyLoading.cellCols;
        cmd.cellRows = mKittyLoading.cellRows;
        cmd.quiet = mKittyLoading.quiet;
        cmd.zIndex = mKittyLoading.zIndex;
        cmd.cursorMovement = mKittyLoading.cursorMovement;
        cmd.dataSize = mKittyLoading.dataSize;
        cmd.dataOffset = mKittyLoading.dataOffset;
        cmd.action = mKittyLoading.action;
        cmd.compressed = mKittyLoading.compressed;
        cmd.transmissionType = mKittyLoading.transmissionType;
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
        off_t fileSize = st.st_size;
        off_t offset = static_cast<off_t>(cmd.dataOffset);
        size_t readSize = cmd.dataSize > 0
            ? static_cast<size_t>(cmd.dataSize)
            : static_cast<size_t>(fileSize - offset);
        if (offset >= fileSize || offset + static_cast<off_t>(readSize) > fileSize) {
            close(fd);
            sendResponse(cmd.id, "EINVAL:offset/size out of range");
            return;
        }
        if (offset > 0) lseek(fd, offset, SEEK_SET);
        chunkData.resize(readSize);
        size_t total = 0;
        while (total < readSize) {
            ssize_t n = read(fd, chunkData.data() + total, readSize - total);
            if (n > 0) { total += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        close(fd);
        if (cmd.transmissionType == 't') unlink(path.c_str());
        if (total != readSize) {
            sendResponse(cmd.id, "EINVAL:cannot read file");
            return;
        }
        chunkData.resize(total);
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
        size_t offset = static_cast<size_t>(cmd.dataOffset);
        size_t readSize = cmd.dataSize > 0
            ? static_cast<size_t>(cmd.dataSize)
            : mapSize - offset;
        if (offset >= mapSize || offset + readSize > mapSize) {
            munmap(ptr, mapSize);
            sendResponse(cmd.id, "EINVAL:offset/size out of range");
            return;
        }
        chunkData.assign(static_cast<uint8_t*>(ptr) + offset,
                         static_cast<uint8_t*>(ptr) + offset + readSize);
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
        auto& img = *it->second;
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
        pl.zIndex = cmd.zIndex;
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
                        imgIt->second->placements.erase(cmd.placementId);
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
                        if (da == 'I' && imgIt->second->placements.empty())
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
                            imgIt->second->placements.erase(cmd.placementId);
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
                            if (da == 'N' && imgIt->second->placements.empty())
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
                fit->second->extraFrames.clear();
                fit->second->currentFrameIndex = 0;
                fit->second->animationState = ImageEntry::Stopped;
                fit->second->frameGeneration++;
            }
            break;
        }
        case 'c': case 'C': // delete at cursor position
        case 'p': case 'P': // delete at cell position (x=, y=)
        {
            int targetCol, targetRow;
            if (da == 'c' || da == 'C') {
                targetCol = mCursorX;
                targetRow = mCursorY;
            } else {
                // x= and y= are 1-based
                targetCol = cmd.xOffset > 0 ? static_cast<int>(cmd.xOffset) - 1 : 0;
                targetRow = cmd.yOffset > 0 ? static_cast<int>(cmd.yOffset) - 1 : 0;
            }
            // Find all images that intersect (targetCol, targetRow)
            IGrid& dg = grid();
            std::unordered_set<uint64_t> toDelete; // (imageId << 32) | placementId
            const CellExtra* cex = dg.getExtra(0, targetRow);
            for (int c = 0; c < mWidth; c++) {
                cex = dg.getExtra(c, targetRow);
                if (!cex || cex->imageId == 0) continue;
                // Check if targetCol falls within this image's column range
                auto imgIt = mImageRegistry.find(cex->imageId);
                if (imgIt == mImageRegistry.end()) continue;
                uint32_t cw = imgIt->second->cellWidth;
                auto plIt = imgIt->second->placements.find(cex->imagePlacementId);
                if (plIt != imgIt->second->placements.end() && plIt->second.cellWidth > 0)
                    cw = plIt->second.cellWidth;
                if (targetCol >= static_cast<int>(cex->imageStartCol) &&
                    targetCol < static_cast<int>(cex->imageStartCol + cw)) {
                    toDelete.insert((static_cast<uint64_t>(cex->imageId) << 32) | cex->imagePlacementId);
                }
            }
            // Clear cells and optionally free images
            for (uint64_t key : toDelete) {
                uint32_t imgId = static_cast<uint32_t>(key >> 32);
                uint32_t plId = static_cast<uint32_t>(key & 0xFFFFFFFF);
                for (int r = 0; r < mHeight; r++) {
                    for (int c = 0; c < mWidth; c++) {
                        const CellExtra* ce = dg.getExtra(c, r);
                        if (ce && ce->imageId == imgId && ce->imagePlacementId == plId) {
                            dg.clearExtra(c, r);
                            dg.markRowDirty(r);
                        }
                    }
                }
                if (da >= 'A' && da <= 'Z') {
                    auto it = mImageRegistry.find(imgId);
                    if (it != mImageRegistry.end()) {
                        it->second->placements.erase(plId);
                        if (it->second->placements.empty())
                            mImageRegistry.erase(it);
                    }
                }
            }
            break;
        }
        case 'x': case 'X': // delete by column
        {
            int targetCol = cmd.xOffset > 0 ? static_cast<int>(cmd.xOffset) - 1 : 0;
            IGrid& dg = grid();
            std::unordered_set<uint64_t> toDelete;
            for (int r = 0; r < mHeight; r++) {
                for (int c = 0; c < mWidth; c++) {
                    const CellExtra* cex = dg.getExtra(c, r);
                    if (!cex || cex->imageId == 0) continue;
                    auto imgIt = mImageRegistry.find(cex->imageId);
                    if (imgIt == mImageRegistry.end()) continue;
                    uint32_t cw = imgIt->second->cellWidth;
                    auto plIt = imgIt->second->placements.find(cex->imagePlacementId);
                    if (plIt != imgIt->second->placements.end() && plIt->second.cellWidth > 0)
                        cw = plIt->second.cellWidth;
                    if (targetCol >= static_cast<int>(cex->imageStartCol) &&
                        targetCol < static_cast<int>(cex->imageStartCol + cw)) {
                        toDelete.insert((static_cast<uint64_t>(cex->imageId) << 32) | cex->imagePlacementId);
                    }
                }
            }
            for (uint64_t key : toDelete) {
                uint32_t imgId = static_cast<uint32_t>(key >> 32);
                uint32_t plId = static_cast<uint32_t>(key & 0xFFFFFFFF);
                for (int r = 0; r < mHeight; r++) {
                    for (int c = 0; c < mWidth; c++) {
                        const CellExtra* ce = dg.getExtra(c, r);
                        if (ce && ce->imageId == imgId && ce->imagePlacementId == plId) {
                            dg.clearExtra(c, r);
                            dg.markRowDirty(r);
                        }
                    }
                }
                if (da >= 'A' && da <= 'Z') {
                    auto it = mImageRegistry.find(imgId);
                    if (it != mImageRegistry.end()) {
                        it->second->placements.erase(plId);
                        if (it->second->placements.empty())
                            mImageRegistry.erase(it);
                    }
                }
            }
            break;
        }
        case 'y': case 'Y': // delete by row
        {
            int targetRow = cmd.yOffset > 0 ? static_cast<int>(cmd.yOffset) - 1 : 0;
            IGrid& dg = grid();
            std::unordered_set<uint64_t> toDelete;
            for (int c = 0; c < mWidth; c++) {
                const CellExtra* cex = dg.getExtra(c, targetRow);
                if (cex && cex->imageId != 0)
                    toDelete.insert((static_cast<uint64_t>(cex->imageId) << 32) | cex->imagePlacementId);
            }
            for (uint64_t key : toDelete) {
                uint32_t imgId = static_cast<uint32_t>(key >> 32);
                uint32_t plId = static_cast<uint32_t>(key & 0xFFFFFFFF);
                for (int r = 0; r < mHeight; r++) {
                    for (int c = 0; c < mWidth; c++) {
                        const CellExtra* ce = dg.getExtra(c, r);
                        if (ce && ce->imageId == imgId && ce->imagePlacementId == plId) {
                            dg.clearExtra(c, r);
                            dg.markRowDirty(r);
                        }
                    }
                }
                if (da >= 'A' && da <= 'Z') {
                    auto it = mImageRegistry.find(imgId);
                    if (it != mImageRegistry.end()) {
                        it->second->placements.erase(plId);
                        if (it->second->placements.empty())
                            mImageRegistry.erase(it);
                    }
                }
            }
            break;
        }
        case 'r': case 'R': // delete by ID range [x, y]
        {
            uint32_t minId = cmd.xOffset;
            uint32_t maxId = cmd.yOffset;
            if (maxId < minId) break;
            IGrid& dg = grid();
            std::vector<uint32_t> idsToDelete;
            for (auto& [id, entry] : mImageRegistry) {
                if (id >= minId && id <= maxId)
                    idsToDelete.push_back(id);
            }
            for (uint32_t imgId : idsToDelete) {
                // Clear cells
                for (int r = 0; r < mHeight; r++) {
                    for (int c = 0; c < mWidth; c++) {
                        const CellExtra* ce = dg.getExtra(c, r);
                        if (ce && ce->imageId == imgId) {
                            dg.clearExtra(c, r);
                            dg.markRowDirty(r);
                        }
                    }
                }
                if (da == 'R')
                    mImageRegistry.erase(imgId);
            }
            break;
        }
        case 'z': case 'Z': // delete by z-index
        {
            IGrid& dg = grid();
            int32_t targetZ = cmd.zIndex;
            std::unordered_set<uint64_t> toDelete;
            for (int r = 0; r < mHeight; r++) {
                for (int c = 0; c < mWidth; c++) {
                    const CellExtra* cex = dg.getExtra(c, r);
                    if (!cex || cex->imageId == 0) continue;
                    auto imgIt = mImageRegistry.find(cex->imageId);
                    if (imgIt == mImageRegistry.end()) continue;
                    auto plIt = imgIt->second->placements.find(cex->imagePlacementId);
                    int32_t z = (plIt != imgIt->second->placements.end()) ? plIt->second.zIndex : 0;
                    if (z == targetZ)
                        toDelete.insert((static_cast<uint64_t>(cex->imageId) << 32) | cex->imagePlacementId);
                }
            }
            for (uint64_t key : toDelete) {
                uint32_t imgId = static_cast<uint32_t>(key >> 32);
                uint32_t plId = static_cast<uint32_t>(key & 0xFFFFFFFF);
                for (int r = 0; r < mHeight; r++) {
                    for (int c = 0; c < mWidth; c++) {
                        const CellExtra* ce = dg.getExtra(c, r);
                        if (ce && ce->imageId == imgId && ce->imagePlacementId == plId) {
                            dg.clearExtra(c, r);
                            dg.markRowDirty(r);
                        }
                    }
                }
                if (da == 'Z') {
                    auto it = mImageRegistry.find(imgId);
                    if (it != mImageRegistry.end()) {
                        it->second->placements.erase(plId);
                        if (it->second->placements.empty())
                            mImageRegistry.erase(it);
                    }
                }
            }
            break;
        }
        case 'q': case 'Q': // delete by position + z-index
        {
            int targetCol = cmd.xOffset > 0 ? static_cast<int>(cmd.xOffset) - 1 : 0;
            int targetRow = cmd.yOffset > 0 ? static_cast<int>(cmd.yOffset) - 1 : 0;
            int32_t targetZ = cmd.zIndex;
            IGrid& dg = grid();
            std::unordered_set<uint64_t> toDelete;
            for (int c = 0; c < mWidth; c++) {
                const CellExtra* cex = dg.getExtra(c, targetRow);
                if (!cex || cex->imageId == 0) continue;
                auto imgIt = mImageRegistry.find(cex->imageId);
                if (imgIt == mImageRegistry.end()) continue;
                uint32_t cw = imgIt->second->cellWidth;
                auto plIt = imgIt->second->placements.find(cex->imagePlacementId);
                if (plIt != imgIt->second->placements.end()) {
                    if (plIt->second.cellWidth > 0) cw = plIt->second.cellWidth;
                    if (plIt->second.zIndex != targetZ) continue;
                } else if (targetZ != 0) {
                    continue;
                }
                if (targetCol >= static_cast<int>(cex->imageStartCol) &&
                    targetCol < static_cast<int>(cex->imageStartCol + cw)) {
                    toDelete.insert((static_cast<uint64_t>(cex->imageId) << 32) | cex->imagePlacementId);
                }
            }
            for (uint64_t key : toDelete) {
                uint32_t imgId = static_cast<uint32_t>(key >> 32);
                uint32_t plId = static_cast<uint32_t>(key & 0xFFFFFFFF);
                for (int r = 0; r < mHeight; r++) {
                    for (int c = 0; c < mWidth; c++) {
                        const CellExtra* ce = dg.getExtra(c, r);
                        if (ce && ce->imageId == imgId && ce->imagePlacementId == plId) {
                            dg.clearExtra(c, r);
                            dg.markRowDirty(r);
                        }
                    }
                }
                if (da == 'Q') {
                    auto it = mImageRegistry.find(imgId);
                    if (it != mImageRegistry.end()) {
                        it->second->placements.erase(plId);
                        if (it->second->placements.empty())
                            mImageRegistry.erase(it);
                    }
                }
            }
            break;
        }
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
        auto& img = *it->second;

        auto decoded = decodeImageData(chunkData, cmd.compressed,
                                        cmd.format, cmd.dataWidth, cmd.dataHeight);
        if (!decoded.error.empty()) {
            sendResponse(cmd.id, decoded.error.c_str());
            return;
        }

        // a=f reuses C= as composition mode: 0=alpha-blend, 1=overwrite.
        // (Matches kitty: graphics.h unions cursor_movement with compose_mode.)
        bool overwrite = (cmd.cursorMovement == 1);

        // c= specifies the 1-based frame to composite onto. 0 or unset means
        // standalone — the base is the Y= background color (transparent
        // black if unset).
        uint32_t composeOnto = cmd.cellCols;

        // r= specifies the 1-based frame to edit. 0/unset/past-end means
        // append a new frame. (Matches kitty graphics.c:1559.)
        uint32_t totalFrames = 1 + static_cast<uint32_t>(img.extraFrames.size());
        uint32_t frameNumber = cmd.cellRows;
        if (frameNumber == 0 || frameNumber > totalFrames + 1)
            frameNumber = totalFrames + 1;
        bool isNewFrame = (frameNumber == totalFrames + 1);

        // Build the base buffer this frame will start from.
        std::vector<uint8_t> baseRGBA;
        size_t pixelCount = static_cast<size_t>(img.pixelWidth) * img.pixelHeight;
        if (isNewFrame) {
            if (composeOnto == 1) {
                baseRGBA = img.rgba;
            } else if (composeOnto >= 2 && composeOnto - 2 < img.extraFrames.size()) {
                baseRGBA = img.extraFrames[composeOnto - 2].rgba;
            } else {
                // Standalone: fill with Y= as packed RRGGBBAA.
                baseRGBA.resize(pixelCount * 4);
                uint32_t bg = cmd.cellYOffset;
                if (bg != 0) {
                    uint8_t r = static_cast<uint8_t>((bg >> 24) & 0xff);
                    uint8_t g = static_cast<uint8_t>((bg >> 16) & 0xff);
                    uint8_t b = static_cast<uint8_t>((bg >>  8) & 0xff);
                    uint8_t a = static_cast<uint8_t>( bg        & 0xff);
                    for (size_t i = 0; i < pixelCount; ++i) {
                        baseRGBA[i*4+0] = r; baseRGBA[i*4+1] = g;
                        baseRGBA[i*4+2] = b; baseRGBA[i*4+3] = a;
                    }
                }
            }
        } else {
            // Edit-in-place: base is the target frame's current content.
            if (frameNumber == 1) baseRGBA = img.rgba;
            else                  baseRGBA = img.extraFrames[frameNumber - 2].rgba;
        }

        // Fast path: full-size standalone new frame at origin — store the
        // decoded data directly as the frame.
        bool fastStore = isNewFrame && composeOnto == 0 && cmd.cellYOffset == 0 &&
            cmd.xOffset == 0 && cmd.yOffset == 0 &&
            static_cast<uint32_t>(decoded.width) == img.pixelWidth &&
            static_cast<uint32_t>(decoded.height) == img.pixelHeight;
        if (fastStore) {
            baseRGBA = std::move(decoded.rgba);
        } else {
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
                        std::memcpy(&baseRGBA[di], &decoded.rgba[si], 4);
                    } else {
                        uint8_t sa = decoded.rgba[si + 3];
                        if (sa == 255) {
                            std::memcpy(&baseRGBA[di], &decoded.rgba[si], 4);
                        } else if (sa > 0) {
                            for (int c = 0; c < 3; c++) {
                                uint8_t sc = decoded.rgba[si + c];
                                uint8_t dc = baseRGBA[di + c];
                                baseRGBA[di + c] = static_cast<uint8_t>(
                                    (sc * sa + dc * (255 - sa)) / 255);
                            }
                            uint8_t da = baseRGBA[di + 3];
                            baseRGBA[di + 3] = static_cast<uint8_t>(
                                sa + da * (255 - sa) / 255);
                        }
                    }
                }
            }
        }

        if (isNewFrame) {
            uint32_t gap = cmd.zIndex > 0 ? static_cast<uint32_t>(cmd.zIndex) : 40;
            ImageEntry::Frame frame;
            frame.gap = gap;
            frame.rgba = std::move(baseRGBA);
            img.extraFrames.push_back(std::move(frame));
        } else {
            // Edit-in-place: write back and optionally refresh the gap.
            if (frameNumber == 1) {
                img.rgba = std::move(baseRGBA);
                if (cmd.zIndex > 0)
                    img.rootFrameGap = static_cast<uint32_t>(cmd.zIndex);
            } else {
                auto& target = img.extraFrames[frameNumber - 2];
                target.rgba = std::move(baseRGBA);
                if (cmd.zIndex > 0)
                    target.gap = static_cast<uint32_t>(cmd.zIndex);
            }
            // Frame content changed — invalidate cached GPU textures.
            img.frameGeneration++;
        }
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
        auto& img = *it->second;

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

        // c= (cellCols) sets current frame (1-based)
        if (cmd.cellCols > 0) {
            uint32_t totalFrames = 1 + static_cast<uint32_t>(img.extraFrames.size());
            uint32_t idx = cmd.cellCols - 1; // convert to 0-based
            if (idx < totalFrames) {
                img.currentFrameIndex = idx;
                // Frame-index change is a display switch, not a content edit —
                // don't bump frameGeneration (see useImageFrame caching).
                // Dirty grid rows containing this image so the render loop re-uploads
                IGrid& ag = grid();
                for (int r = 0; r < mHeight; r++) {
                    for (int c = 0; c < mWidth; c++) {
                        const CellExtra* ce = ag.getExtra(c, r);
                        if (ce && ce->imageId == targetId) {
                            ag.markRowDirty(r);
                            break;
                        }
                    }
                }
            }
        }

        // kitty sends no response for a=a (animation control)
        return;
    }
    case 'c': // frame composition — blit rectangle from one frame to another
    {
        uint32_t targetId = resolveId();
        auto it = mImageRegistry.find(targetId);
        if (it == mImageRegistry.end()) {
            sendResponse(targetId, "ENOENT:image not found");
            return;
        }
        auto& img = *it->second;
        uint32_t totalFrames = 1 + static_cast<uint32_t>(img.extraFrames.size());

        // r= source frame, c= dest frame (1-based; reused as cellRows/cellCols)
        uint32_t srcFrameNum = cmd.cellRows;  // r=
        uint32_t dstFrameNum = cmd.cellCols;  // c=
        if (srcFrameNum == 0 || srcFrameNum > totalFrames ||
            dstFrameNum == 0 || dstFrameNum > totalFrames) {
            sendResponse(targetId, "ENOENT:frame not found");
            return;
        }

        // Get frame data pointers
        auto frameData = [&](uint32_t num) -> std::vector<uint8_t>& {
            if (num == 1) return img.rgba;
            return img.extraFrames[num - 2].rgba;
        };
        const std::vector<uint8_t>& srcData = frameData(srcFrameNum);
        std::vector<uint8_t>& dstData = frameData(dstFrameNum);

        int imgW = static_cast<int>(img.pixelWidth);
        int imgH = static_cast<int>(img.pixelHeight);

        // Rectangle size (default: full image)
        int rectW = cmd.width > 0 ? static_cast<int>(cmd.width) : imgW;
        int rectH = cmd.height > 0 ? static_cast<int>(cmd.height) : imgH;

        // Kitty maps: x=/y= → destination offset, X=/Y= → source offset
        int srcX = static_cast<int>(cmd.cellXOffset);
        int srcY = static_cast<int>(cmd.cellYOffset);
        int dstX = static_cast<int>(cmd.xOffset);
        int dstY = static_cast<int>(cmd.yOffset);

        // Bounds check
        if (srcX + rectW > imgW || srcY + rectH > imgH ||
            dstX + rectW > imgW || dstY + rectH > imgH ||
            srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0) {
            sendResponse(targetId, "EINVAL:rectangle out of bounds");
            return;
        }

        // Overlap check for same frame
        if (srcFrameNum == dstFrameNum) {
            bool overlapX = srcX < dstX + rectW && dstX < srcX + rectW;
            bool overlapY = srcY < dstY + rectH && dstY < srcY + rectH;
            if (overlapX && overlapY) {
                sendResponse(targetId, "EINVAL:overlapping rectangles on same frame");
                return;
            }
        }

        bool overwrite = (cmd.cursorMovement == 1); // C=1 means overwrite

        for (int y = 0; y < rectH; y++) {
            for (int x = 0; x < rectW; x++) {
                size_t si = (static_cast<size_t>(srcY + y) * imgW + srcX + x) * 4;
                size_t di = (static_cast<size_t>(dstY + y) * imgW + dstX + x) * 4;
                if (overwrite) {
                    std::memcpy(&dstData[di], &srcData[si], 4);
                } else {
                    uint8_t sa = srcData[si + 3];
                    if (sa == 255) {
                        std::memcpy(&dstData[di], &srcData[si], 4);
                    } else if (sa > 0) {
                        for (int ch = 0; ch < 3; ch++) {
                            uint8_t sc = srcData[si + ch];
                            uint8_t dc = dstData[di + ch];
                            dstData[di + ch] = static_cast<uint8_t>(
                                (sc * sa + dc * (255 - sa)) / 255);
                        }
                        uint8_t da = dstData[di + 3];
                        dstData[di + 3] = static_cast<uint8_t>(
                            sa + da * (255 - sa) / 255);
                    }
                }
            }
        }

        img.frameGeneration++;
        sendResponse(targetId, "OK");
        return;
    }
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
    mImageRegistry[imageId] = std::make_shared<ImageEntry>(std::move(entry));
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
        pl.zIndex = cmd.zIndex;
        mImageRegistry[imageId]->placements[cmd.placementId] = pl;
        placeImageInGrid(imageId, cmd.placementId, cellCols, cellRows, cmd.cursorMovement == 0);
    }

    // Use cmd.id for response — if client didn't set i= (cmd.id==0), no response
    // (matches kitty: finish_command_response checks g->id || g->image_number)
    sendResponse(cmd.id, "OK");
}

bool TerminalEmulator::tickAnimations()
{
    std::lock_guard<std::recursive_mutex> _lk(mMutex);
    uint64_t now = mono();
    bool anyAdvanced = false;

    for (auto& [id, imgPtr] : mImageRegistry) {
        auto& img = *imgPtr;
        if (!img.hasAnimation()) continue;

        uint32_t gap = img.currentFrameGap();
        if (gap == 0) gap = 40; // default 40ms

        uint64_t elapsed = now - img.frameShownAt;

        if (elapsed < gap) continue;

        // Advance frame(s), potentially skipping if multiple gaps elapsed
        uint32_t totalFrames = 1 + static_cast<uint32_t>(img.extraFrames.size());
        bool stopped = false;
        uint32_t startIndex = img.currentFrameIndex;

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

        // Drift-free rebase: `elapsed` has had each consumed gap subtracted, so
        // `now - elapsed` is the boundary at which the current frame should
        // have started. Setting `frameShownAt = now` instead would rebase to
        // wake-up time and let timer jitter accumulate — eventually crossing
        // a gap boundary and causing a visible double-advance.
        img.frameShownAt = now - elapsed;
        if (img.currentFrameIndex != startIndex)
            anyAdvanced = true;
        // Do NOT bump frameGeneration here: it tracks *content* edits only.
        // The renderer keys cached textures by (frameIndex, frameGeneration);
        // bumping on a tick would invalidate every cached frame slot every
        // cycle, defeating the cache.
    }

    return anyAdvanced;
}

