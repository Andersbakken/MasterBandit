#include "FontFallback.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <fstream>
#include <vector>
#include <string>

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

static std::string fontFilePath(CTFontRef font)
{
    CFURLRef url = static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute));
    if (!url) return {};
    char buf[1024];
    bool ok = CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(buf), sizeof(buf));
    CFRelease(url);
    return ok ? std::string(buf) : std::string{};
}

static bool fontHasGlyph(CTFontRef font, const UniChar* utf16, CFIndex utf16Len)
{
    std::vector<CGGlyph> glyphs(utf16Len);
    return CTFontGetGlyphsForCharacters(font, utf16, glyphs.data(), utf16Len) && glyphs[0] != 0;
}

static bool isNormalVariant(CTFontRef font)
{
    CTFontSymbolicTraits traits = CTFontGetSymbolicTraits(font);
    // Reject bold, italic, condensed, expanded — only use regular variants as fallbacks
    constexpr CTFontSymbolicTraits kReject =
        kCTFontTraitBold | kCTFontTraitItalic | kCTFontTraitCondensed | kCTFontTraitExpanded;
    return (traits & kReject) == 0;
}

static bool isLastResort(CTFontRef font)
{
    CFStringRef name = CTFontCopyFamilyName(font);
    if (!name) return false;
    bool result = CFStringCompare(name, CFSTR(".LastResort"), 0) == kCFCompareEqualTo;
    CFRelease(name);
    return result;
}

static bool isColorFont(CTFontRef font)
{
    CTFontSymbolicTraits traits = CTFontGetSymbolicTraits(font);
    return (traits & kCTFontTraitColorGlyphs) != 0;
}

// Get Menlo — used as the base font for CTFontCreateForString, same as WezTerm.
// Menlo is always available on macOS and gives consistent system fallback results.
static CTFontRef getMenlo()
{
    static CTFontRef menlo = []() -> CTFontRef {
        CTFontRef f = CTFontCreateWithName(CFSTR("Menlo"), 16.0, nullptr);
        return f; // intentionally never released — process lifetime singleton
    }();
    return menlo;
}

static void codepointToUTF16(char32_t codepoint, UniChar utf16[2], CFIndex& utf16Len)
{
    if (codepoint <= 0xFFFF) {
        utf16[0] = static_cast<UniChar>(codepoint);
        utf16Len = 1;
    } else {
        char32_t cp = codepoint - 0x10000;
        utf16[0] = static_cast<UniChar>(0xD800 + (cp >> 10));
        utf16[1] = static_cast<UniChar>(0xDC00 + (cp & 0x3FF));
        utf16Len = 2;
    }
}

static std::vector<uint8_t> loadFontFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    auto size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

struct CandidateFont {
    std::string path;
    size_t coverage; // number of codepoints covered (for sorting)
};

std::vector<uint8_t> FontFallback::fontDataForEmoji(char32_t codepoint)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = emojiCache_.find(codepoint);
    if (it != emojiCache_.end()) {
        if (it->second < 0) return {};
        return fallbackFonts_[it->second].data;
    }

    UniChar utf16[2];
    CFIndex utf16Len;
    codepointToUTF16(codepoint, utf16, utf16Len);

    CFStringRef str = CFStringCreateWithCharacters(nullptr, utf16, utf16Len);

    // Ask CoreText for a fallback — it often picks Apple Color Emoji
    std::string found;
    CTFontRef menlo = getMenlo();
    if (menlo) {
        CTFontRef fallback = CTFontCreateForString(menlo, str, CFRangeMake(0, utf16Len));
        if (fallback) {
            if (!isLastResort(fallback) && isColorFont(fallback) && fontHasGlyph(fallback, utf16, utf16Len)) {
                found = fontFilePath(fallback);
            }
            CFRelease(fallback);
        }
    }

    // If CTFontCreateForString didn't give a color font, scan the cascade list
    if (found.empty() && menlo) {
        CFArrayRef cascade = CTFontCopyDefaultCascadeListForLanguages(menlo, nullptr);
        if (cascade) {
            CFIndex count = CFArrayGetCount(cascade);
            for (CFIndex i = 0; i < count; ++i) {
                CTFontDescriptorRef desc = static_cast<CTFontDescriptorRef>(
                    CFArrayGetValueAtIndex(cascade, i));
                CTFontRef candidate = CTFontCreateWithFontDescriptor(desc, 16.0, nullptr);
                if (!candidate) continue;

                if (!isLastResort(candidate) && isColorFont(candidate) &&
                    fontHasGlyph(candidate, utf16, utf16Len)) {
                    found = fontFilePath(candidate);
                    CFRelease(candidate);
                    break;
                }
                CFRelease(candidate);
            }
            CFRelease(cascade);
        }
    }

    CFRelease(str);

    if (found.empty()) {
        emojiCache_[codepoint] = -1;
        return {};
    }

    auto pathIt = pathToIndex_.find(found);
    if (pathIt != pathToIndex_.end()) {
        emojiCache_[codepoint] = pathIt->second;
        return fallbackFonts_[pathIt->second].data;
    }

    std::vector<uint8_t> data = loadFontFile(found);
    if (data.empty()) {
        emojiCache_[codepoint] = -1;
        return {};
    }

    spdlog::info("FontFallback: loaded emoji font {} for U+{:04X}", found, static_cast<uint32_t>(codepoint));

    int idx = static_cast<int>(fallbackFonts_.size());
    fallbackFonts_.push_back({found, std::move(data)});
    pathToIndex_[found] = idx;
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

    UniChar utf16[2];
    CFIndex utf16Len;
    codepointToUTF16(codepoint, utf16, utf16Len);

    CFStringRef str = CFStringCreateWithCharacters(nullptr, utf16, utf16Len);

    std::vector<CandidateFont> candidates;

    // Ask CoreText for the best fallback using Menlo as base (same as WezTerm)
    CTFontRef menlo = getMenlo();
    if (menlo) {
        CTFontRef fallback = CTFontCreateForString(menlo, str, CFRangeMake(0, utf16Len));
        if (fallback) {
            if (!isLastResort(fallback) && isNormalVariant(fallback) && fontHasGlyph(fallback, utf16, utf16Len)) {
                std::string path = fontFilePath(fallback);
                if (!path.empty() && path != primaryFontPath)
                    candidates.push_back({std::move(path), 1});
            }
            CFRelease(fallback);
        }
    }

    // If CTFontCreateForString found nothing usable, consult the system cascade list
    if (candidates.empty()) {
        CFArrayRef cascade = menlo
            ? CTFontCopyDefaultCascadeListForLanguages(menlo, nullptr)
            : nullptr;

        if (cascade) {
            CFIndex count = CFArrayGetCount(cascade);
            for (CFIndex i = 0; i < count; ++i) {
                CTFontDescriptorRef desc = static_cast<CTFontDescriptorRef>(
                    CFArrayGetValueAtIndex(cascade, i));
                CTFontRef candidate = CTFontCreateWithFontDescriptor(desc, 16.0, nullptr);
                if (!candidate) continue;

                if (!isLastResort(candidate) && isNormalVariant(candidate) &&
                    fontHasGlyph(candidate, utf16, utf16Len)) {
                    std::string path = fontFilePath(candidate);
                    if (!path.empty() && path != primaryFontPath)
                        candidates.push_back({std::move(path), 1});
                }
                CFRelease(candidate);
            }
            CFRelease(cascade);
        }
    }

    CFRelease(str);

    // Sort by descending coverage
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateFont& a, const CandidateFont& b) {
                  return a.coverage > b.coverage;
              });

    // Deduplicate by path
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const CandidateFont& a, const CandidateFont& b) {
                                     return a.path == b.path;
                                 }), candidates.end());

    if (candidates.empty()) {
        codepointCache_[codepoint] = -1;
        return {};
    }

    const std::string& fallbackPath = candidates[0].path;

    // Check if we already loaded this font
    auto pathIt = pathToIndex_.find(fallbackPath);
    if (pathIt != pathToIndex_.end()) {
        codepointCache_[codepoint] = pathIt->second;
        return fallbackFonts_[pathIt->second].data;
    }

    std::vector<uint8_t> data = loadFontFile(fallbackPath);
    if (data.empty()) {
        spdlog::warn("FontFallback: failed to open {}", fallbackPath);
        codepointCache_[codepoint] = -1;
        return {};
    }

    spdlog::info("FontFallback: loaded {} for U+{:04X}", fallbackPath, static_cast<uint32_t>(codepoint));

    int idx = static_cast<int>(fallbackFonts_.size());
    fallbackFonts_.push_back({fallbackPath, std::move(data)});
    pathToIndex_[fallbackPath] = idx;
    codepointCache_[codepoint] = idx;
    return fallbackFonts_[idx].data;
}
