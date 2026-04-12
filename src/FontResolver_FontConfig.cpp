#include "FontResolver.h"

#include <fontconfig/fontconfig.h>

namespace {

std::string resolveByFamily(const std::string& family, FontTraits traits, bool requireFamily = false)
{
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));
    if (!pat) return {};

    if (traits & FontTraitBold)
        FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
    if (traits & FontTraitItalic)
        FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);

    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);

    if (!match) return {};

    if (requireFamily) {
        FcChar8* matchedFamily = nullptr;
        if (FcPatternGetString(match, FC_FAMILY, 0, &matchedFamily) == FcResultMatch && matchedFamily) {
            if (FcStrCmpIgnoreCase(matchedFamily, reinterpret_cast<const FcChar8*>(family.c_str())) != 0) {
                FcPatternDestroy(match);
                return {};
            }
        }
    }

    FcChar8* file = nullptr;
    std::string path;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
        path = reinterpret_cast<const char*>(file);
    }
    FcPatternDestroy(match);

    return path;
}

} // namespace

std::string resolveFontFamily(const std::string& family, FontTraits traits)
{
    if (isGenericMonoFamily(family)) {
        for (const auto& candidate : preferredMonospaceFonts()) {
            auto result = resolveByFamily(candidate, traits, true);
            if (!result.empty()) return result;
        }
        return resolveByFamily("monospace", traits);
    }

    return resolveByFamily(family, traits, true);
}
