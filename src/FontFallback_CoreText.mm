#include "FontFallback.h"
#include <spdlog/spdlog.h>
#include <fstream>

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

std::vector<uint8_t> FontFallback::fontDataForCodepoint(const std::string& primaryFontPath, char32_t codepoint)
{
    // Check cache first
    auto it = codepointCache_.find(codepoint);
    if (it != codepointCache_.end()) {
        if (it->second < 0) return {};
        return fallbackFonts_[it->second].data;
    }

    // Create a CTFont from the primary font file
    CFStringRef cfPath = CFStringCreateWithCString(nullptr, primaryFontPath.c_str(), kCFStringEncodingUTF8);
    CFURLRef fontURL = CFURLCreateWithFileSystemPath(nullptr, cfPath, kCFURLPOSIXPathStyle, false);
    CGDataProviderRef provider = CGDataProviderCreateWithURL(fontURL);
    CGFontRef cgFont = provider ? CGFontCreateWithDataProvider(provider) : nullptr;
    CTFontRef primaryFont = cgFont ? CTFontCreateWithGraphicsFont(cgFont, 16.0, nullptr, nullptr) : nullptr;

    if (provider) CGDataProviderRelease(provider);
    if (cgFont) CGFontRelease(cgFont);
    CFRelease(fontURL);
    CFRelease(cfPath);

    if (!primaryFont) {
        codepointCache_[codepoint] = -1;
        return {};
    }

    // Create a string containing the codepoint
    UniChar utf16[2];
    CFIndex utf16Len;
    if (codepoint <= 0xFFFF) {
        utf16[0] = static_cast<UniChar>(codepoint);
        utf16Len = 1;
    } else {
        // Surrogate pair
        codepoint -= 0x10000;
        utf16[0] = static_cast<UniChar>(0xD800 + (codepoint >> 10));
        utf16[1] = static_cast<UniChar>(0xDC00 + (codepoint & 0x3FF));
        utf16Len = 2;
    }
    CFStringRef str = CFStringCreateWithCharacters(nullptr, utf16, utf16Len);

    // Ask Core Text for the fallback font
    CTFontRef fallbackFont = CTFontCreateForString(primaryFont, str, CFRangeMake(0, utf16Len));
    CFRelease(str);
    CFRelease(primaryFont);

    if (!fallbackFont) {
        codepointCache_[codepoint] = -1;
        return {};
    }

    // Check that the fallback actually has the glyph (CTFontCreateForString can return the same font)
    CGGlyph glyphs[2];
    bool hasGlyph = CTFontGetGlyphsForCharacters(fallbackFont, utf16, glyphs, static_cast<CFIndex>(utf16Len));
    if (!hasGlyph || glyphs[0] == 0) {
        CFRelease(fallbackFont);
        codepointCache_[codepoint] = -1;
        return {};
    }

    // Get the font file URL
    CFURLRef fallbackURL = static_cast<CFURLRef>(CTFontCopyAttribute(fallbackFont, kCTFontURLAttribute));
    CFRelease(fallbackFont);

    if (!fallbackURL) {
        codepointCache_[codepoint] = -1;
        return {};
    }

    char pathBuf[1024];
    if (!CFURLGetFileSystemRepresentation(fallbackURL, true, reinterpret_cast<UInt8*>(pathBuf), sizeof(pathBuf))) {
        CFRelease(fallbackURL);
        codepointCache_[codepoint] = -1;
        return {};
    }
    CFRelease(fallbackURL);

    std::string fallbackPath(pathBuf);

    // Skip if it's the same as the primary font
    if (fallbackPath == primaryFontPath) {
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
    fallbackFonts_.push_back({fallbackPath, data});
    pathToIndex_[fallbackPath] = idx;
    codepointCache_[codepoint] = idx;
    return data;
}
