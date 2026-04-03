#include "FontFallback.h"
#include <spdlog/spdlog.h>
#include <fstream>

#include <fontconfig/fontconfig.h>

std::vector<uint8_t> FontFallback::fontDataForCodepoint(const std::string& primaryFontPath, char32_t codepoint)
{
    // Check cache first
    auto it = codepointCache_.find(codepoint);
    if (it != codepointCache_.end()) {
        if (it->second < 0) return {};
        return fallbackFonts_[it->second].data;
    }

    // Build a fontconfig pattern that requests a font covering the codepoint
    FcPattern* pat = FcPatternCreate();
    if (!pat) {
        codepointCache_[codepoint] = -1;
        return {};
    }

    FcCharSet* cs = FcCharSetCreate();
    if (!cs) {
        FcPatternDestroy(pat);
        codepointCache_[codepoint] = -1;
        return {};
    }
    FcCharSetAddChar(cs, static_cast<FcChar32>(codepoint));
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcCharSetDestroy(cs);

    // Request monospace to prefer monospace fallbacks (best-effort)
    FcPatternAddInteger(pat, FC_SPACING, FC_MONO);

    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcFontSet* fs = FcFontSort(nullptr, pat, FcTrue, nullptr, &result);
    FcPatternDestroy(pat);

    if (!fs || fs->nfont == 0) {
        if (fs) FcFontSetDestroy(fs);
        codepointCache_[codepoint] = -1;
        return {};
    }

    std::string fallbackPath;

    for (int i = 0; i < fs->nfont; ++i) {
        FcPattern* font = fs->fonts[i];

        FcChar8* filePath = nullptr;
        if (FcPatternGetString(font, FC_FILE, 0, &filePath) != FcResultMatch || !filePath) {
            continue;
        }

        std::string candidatePath(reinterpret_cast<const char*>(filePath));

        // Skip if it's the same as the primary font
        if (candidatePath == primaryFontPath) {
            continue;
        }

        // Verify via fontconfig's own charset data
        FcCharSet* fontCs = nullptr;
        if (FcPatternGetCharSet(font, FC_CHARSET, 0, &fontCs) != FcResultMatch || !fontCs) {
            continue;
        }
        if (!FcCharSetHasChar(fontCs, static_cast<FcChar32>(codepoint))) {
            continue;
        }

        fallbackPath = std::move(candidatePath);
        break;
    }

    FcFontSetDestroy(fs);

    if (fallbackPath.empty()) {
        codepointCache_[codepoint] = -1;
        return {};
    }

    // Check if we already loaded this font
    auto pathIt = pathToIndex_.find(fallbackPath);
    if (pathIt != pathToIndex_.end()) {
        codepointCache_[codepoint] = pathIt->second;
        return fallbackFonts_[pathIt->second].data;
    }

    // Load the font file
    std::ifstream file(fallbackPath, std::ios::binary | std::ios::ate);
    if (!file) {
        spdlog::warn("FontFallback: failed to open {}", fallbackPath);
        codepointCache_[codepoint] = -1;
        return {};
    }
    auto size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    spdlog::info("FontFallback: loaded {} for U+{:04X}", fallbackPath, static_cast<uint32_t>(codepoint));

    int idx = static_cast<int>(fallbackFonts_.size());
    fallbackFonts_.push_back({fallbackPath, std::move(data)});
    pathToIndex_[fallbackPath] = idx;
    codepointCache_[codepoint] = idx;
    return fallbackFonts_[idx].data;
}
