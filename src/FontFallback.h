#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

class FontFallback {
public:
    // Returns font file data for a font that covers the given codepoint.
    // Returns empty vector if no fallback found.
    // Results are cached — repeated lookups for the same codepoint are fast.
    std::vector<uint8_t> fontDataForCodepoint(const std::string& primaryFontPath, char32_t codepoint);

    // Like fontDataForCodepoint, but prefers color/emoji fonts (FC_COLOR=true).
    // Used for emoji-presentation codepoints where a COLR font is wanted.
    std::vector<uint8_t> fontDataForEmoji(char32_t codepoint);

private:
    std::mutex mutex_;

    // Cache: codepoint → index into fallbackFonts_ (-1 = no fallback found)
    std::unordered_map<char32_t, int> codepointCache_;
    std::unordered_map<char32_t, int> emojiCache_;

    // Loaded fallback font data, deduped by font path
    struct FallbackEntry {
        std::string path;
        std::vector<uint8_t> data;
    };
    std::vector<FallbackEntry> fallbackFonts_;
    std::unordered_map<std::string, int> pathToIndex_;
};
