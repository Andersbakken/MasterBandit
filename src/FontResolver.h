#pragma once

#include <cstdint>
#include <span>
#include <string>

enum FontTraits : uint8_t {
    FontTraitNone   = 0,
    FontTraitBold   = 1 << 0,
    FontTraitItalic = 1 << 1,
};

// Resolves a font family name (e.g. "JetBrains Mono") to an absolute font file path.
// Returns empty string if the family cannot be found.
std::string resolveFontFamily(const std::string& family, FontTraits traits = FontTraitNone);

// Returns whether the family name is a generic alias (e.g. "monospace", "mono")
// that should be resolved via preferredMonospaceFonts() instead of directly.
inline bool isGenericMonoFamily(const std::string& family)
{
    return family == "monospace" || family == "mono";
}

// Platform-preferred monospace fonts, tried in order before falling back
// to the system's generic monospace resolution.
inline std::span<const char* const> preferredMonospaceFonts()
{
#ifdef __APPLE__
    static const char* const fonts[] = { "Menlo", "SF Mono", "Monaco", "Courier" };
#else
    static const char* const fonts[] = { "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Ubuntu Mono", "Courier New" };
#endif
    return fonts;
}
