#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

class FontFallback {
public:
    // Returns font file data for a font that covers the given codepoint.
    // Returns empty vector if no fallback found.
    // Results are cached — repeated lookups for the same codepoint are fast.
    std::vector<uint8_t> fontDataForCodepoint(const std::string& primaryFontPath, char32_t codepoint);

private:
    // Cache: codepoint → index into fallbackFonts_ (-1 = no fallback found)
    std::unordered_map<char32_t, int> codepointCache_;

    // Loaded fallback font data, deduped by font path
    struct FallbackEntry {
        std::string path;
        std::vector<uint8_t> data;
    };
    std::vector<FallbackEntry> fallbackFonts_;
    std::unordered_map<std::string, int> pathToIndex_;
};
