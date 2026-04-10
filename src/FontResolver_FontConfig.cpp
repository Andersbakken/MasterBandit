#include "FontResolver.h"

#include <fontconfig/fontconfig.h>

namespace {

// If requireFamily is true, verify the matched font's family matches the request.
// This is needed because FcFontMatch always returns a best-effort match even when
// the requested family doesn't exist.
std::string resolveByFamily(const std::string& family, bool bold, bool requireFamily = false)
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

std::string resolveFontFamily(const std::string& family, bool bold)
{
    if (isGenericMonoFamily(family)) {
        for (const auto& candidate : preferredMonospaceFonts()) {
            auto result = resolveByFamily(candidate, bold, true);
            if (!result.empty()) return result;
        }
        // Fall back to fontconfig's native "monospace" alias.
        return resolveByFamily("monospace", bold);
    }

    return resolveByFamily(family, bold, true);
}
