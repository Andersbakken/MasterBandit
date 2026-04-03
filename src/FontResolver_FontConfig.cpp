#include "FontResolver.h"

#include <fontconfig/fontconfig.h>

std::string resolveFontFamily(const std::string& family)
{
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));
    if (!pat) return {};

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
