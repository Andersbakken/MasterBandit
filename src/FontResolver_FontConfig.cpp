#include "FontResolver.h"

#include <fontconfig/fontconfig.h>

std::string resolveFontFamily(const std::string& family, bool bold)
{
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));
    if (!pat) return {};

    if (bold)
        FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);

    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);

    if (!match) return {};

    FcChar8* file = nullptr;
    std::string path;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
        path = reinterpret_cast<const char*>(file);
    }
    FcPatternDestroy(match);

    return path;
}
