#include "FontFallback.h"
#include <spdlog/spdlog.h>
#include <fstream>

#include <fontconfig/fontconfig.h>

// Query fontconfig for a font covering `codepoint`, optionally restricting to monospace.
// Returns the file path of the best match, or empty string.
static const int kMonoSpacings[] = { FC_MONO, FC_DUAL, FC_CHARCELL };

// Use FcFontList (all matching fonts, database order) for monospace — same approach as WezTerm.
// Use FcFontSort (best match first) for the any-font fallback pass.
static std::string queryFontConfig(char32_t codepoint, bool monoOnly, const std::string& primaryFontPath)
{
    FcCharSet* cs = FcCharSetCreate();
    if (!cs) return {};
    FcCharSetAddChar(cs, static_cast<FcChar32>(codepoint));

    auto tryList = [&](FcFontSet* fs) -> std::string {
        if (!fs) return {};
        std::string found;
        for (int i = 0; i < fs->nfont; ++i) {
            FcPattern* font = fs->fonts[i];
            FcChar8* filePath = nullptr;
            if (FcPatternGetString(font, FC_FILE, 0, &filePath) != FcResultMatch || !filePath)
                continue;
            std::string candidatePath(reinterpret_cast<const char*>(filePath));
            if (candidatePath == primaryFontPath)
                continue;
            FcCharSet* fontCs = nullptr;
            if (FcPatternGetCharSet(font, FC_CHARSET, 0, &fontCs) != FcResultMatch || !fontCs)
                continue;
            if (!FcCharSetHasChar(fontCs, static_cast<FcChar32>(codepoint)))
                continue;
            found = std::move(candidatePath);
            break;
        }
        FcFontSetDestroy(fs);
        return found;
    };

    std::string found;

    if (monoOnly) {
        // List all fonts per spacing value (no charset filter — checked manually in tryList).
        // FcFontList returns fonts in database order without sort scoring bias.
        for (int spacing : kMonoSpacings) {
            FcPattern* pat = FcPatternCreate();
            FcPatternAddInteger(pat, FC_SPACING, spacing);
            FcObjectSet* os = FcObjectSetBuild(FC_FILE, FC_CHARSET, FC_SPACING, nullptr);
            FcFontSet* fs = FcFontList(nullptr, pat, os);
            FcObjectSetDestroy(os);
            FcPatternDestroy(pat);
            found = tryList(fs);
            if (!found.empty()) break;
        }
    } else {
        // Any font: use FcFontSort for best-match ordering
        FcPattern* pat = FcPatternCreate();
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcConfigSubstitute(nullptr, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult result;
        FcFontSet* fs = FcFontSort(nullptr, pat, FcTrue, nullptr, &result);
        FcPatternDestroy(pat);
        found = tryList(fs);
    }

    FcCharSetDestroy(cs);
    return found;
}

static std::string queryEmojiFont(char32_t codepoint)
{
    FcCharSet* cs = FcCharSetCreate();
    if (!cs) return {};
    FcCharSetAddChar(cs, static_cast<FcChar32>(codepoint));

    FcPattern* pat = FcPatternCreate();
    FcPatternAddBool(pat, FC_COLOR, FcTrue);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcFontSet* fs = FcFontSort(nullptr, pat, FcTrue, nullptr, &result);
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);

    std::string found;
    if (fs) {
        for (int i = 0; i < fs->nfont; ++i) {
            FcPattern* font = fs->fonts[i];
            FcChar8* filePath = nullptr;
            if (FcPatternGetString(font, FC_FILE, 0, &filePath) != FcResultMatch || !filePath)
                continue;
            FcCharSet* fontCs = nullptr;
            if (FcPatternGetCharSet(font, FC_CHARSET, 0, &fontCs) != FcResultMatch || !fontCs)
                continue;
            if (!FcCharSetHasChar(fontCs, static_cast<FcChar32>(codepoint)))
                continue;
            // Verify it's actually a color font
            FcBool color = FcFalse;
            FcPatternGetBool(font, FC_COLOR, 0, &color);
            if (!color) continue;
            found = reinterpret_cast<const char*>(filePath);
            break;
        }
        FcFontSetDestroy(fs);
    }
    return found;
}

std::vector<uint8_t> FontFallback::fontDataForEmoji(char32_t codepoint)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = emojiCache_.find(codepoint);
    if (it != emojiCache_.end()) {
        if (it->second < 0) return {};
        return fallbackFonts_[it->second].data;
    }

    std::string path = queryEmojiFont(codepoint);
    if (path.empty()) {
        emojiCache_[codepoint] = -1;
        return {};
    }

    auto pathIt = pathToIndex_.find(path);
    if (pathIt != pathToIndex_.end()) {
        emojiCache_[codepoint] = pathIt->second;
        return fallbackFonts_[pathIt->second].data;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        emojiCache_[codepoint] = -1;
        return {};
    }
    auto size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    spdlog::info("FontFallback: loaded emoji font {} for U+{:04X}", path, static_cast<uint32_t>(codepoint));

    int idx = static_cast<int>(fallbackFonts_.size());
    fallbackFonts_.push_back({path, std::move(data)});
    pathToIndex_[path] = idx;
    emojiCache_[codepoint] = idx;
    return fallbackFonts_[idx].data;
}

std::vector<uint8_t> FontFallback::fontDataForCodepoint(const std::string& primaryFontPath, char32_t codepoint)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Check cache first
    auto it = codepointCache_.find(codepoint);
    if (it != codepointCache_.end()) {
        if (it->second < 0) return {};
        return fallbackFonts_[it->second].data;
    }

    // Pass 1: monospace fonts only
    std::string fallbackPath = queryFontConfig(codepoint, true, primaryFontPath);

    // Pass 2: any font if no monospace covers it
    if (fallbackPath.empty())
        fallbackPath = queryFontConfig(codepoint, false, primaryFontPath);

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
