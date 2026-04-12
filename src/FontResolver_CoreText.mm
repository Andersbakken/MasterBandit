#include "FontResolver.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

namespace {

// If requireFamily is true, verify the matched font's family matches the request.
// CoreText will happily return a substitute font when the requested family doesn't exist.
std::string resolveByFamily(const std::string& family, FontTraits traits, bool requireFamily = false)
{
    CFStringRef cfFamily = CFStringCreateWithCString(nullptr, family.c_str(), kCFStringEncodingUTF8);

    CTFontDescriptorRef desc;
    if (traits != FontTraitNone) {
        CFStringRef keys[] = { kCTFontFamilyNameAttribute, kCTFontTraitsAttribute };
        uint32_t ctTrait = 0;
        if (traits & FontTraitBold) ctTrait |= kCTFontBoldTrait;
        if (traits & FontTraitItalic) ctTrait |= kCTFontItalicTrait;
        CFNumberRef traitNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &ctTrait);
        CFStringRef traitKeys[] = { kCTFontSymbolicTrait };
        CFTypeRef traitVals[] = { traitNum };
        CFDictionaryRef traitDict = CFDictionaryCreate(nullptr,
            reinterpret_cast<const void**>(traitKeys),
            reinterpret_cast<const void**>(traitVals), 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFTypeRef vals[] = { cfFamily, traitDict };
        CFDictionaryRef attrs = CFDictionaryCreate(nullptr,
            reinterpret_cast<const void**>(keys),
            reinterpret_cast<const void**>(vals), 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        desc = CTFontDescriptorCreateWithAttributes(attrs);
        CFRelease(attrs);
        CFRelease(traitDict);
        CFRelease(traitNum);
    } else {
        desc = CTFontDescriptorCreateWithNameAndSize(cfFamily, 0.0);
    }
    CFRelease(cfFamily);

    if (!desc) return {};

    CTFontRef font = CTFontCreateWithFontDescriptor(desc, 16.0, nullptr);
    CFRelease(desc);

    if (!font) return {};

    if (requireFamily) {
        CFStringRef matchedFamily = static_cast<CFStringRef>(CTFontCopyFamilyName(font));
        if (matchedFamily) {
            CFStringRef cfExpected = CFStringCreateWithCString(nullptr, family.c_str(), kCFStringEncodingUTF8);
            bool matches = CFStringCompare(matchedFamily, cfExpected,
                kCFCompareCaseInsensitive) == kCFCompareEqualTo;
            CFRelease(cfExpected);
            CFRelease(matchedFamily);
            if (!matches) {
                CFRelease(font);
                return {};
            }
        }
    }

    CFURLRef url = static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute));
    CFRelease(font);

    if (!url) return {};

    char pathBuf[1024];
    bool ok = CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(pathBuf), sizeof(pathBuf));
    CFRelease(url);

    return ok ? std::string(pathBuf) : std::string{};
}

// Last-resort: ask CoreText for any font with the monospace trait.
std::string resolveByMonoTrait(FontTraits traits)
{
    uint32_t trait = kCTFontMonoSpaceTrait;
    if (traits & FontTraitBold) trait |= kCTFontBoldTrait;
    if (traits & FontTraitItalic) trait |= kCTFontItalicTrait;

    CFNumberRef traitNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &trait);
    CFStringRef traitKeys[] = { kCTFontSymbolicTrait };
    CFTypeRef traitVals[] = { traitNum };
    CFDictionaryRef traitDict = CFDictionaryCreate(nullptr,
        reinterpret_cast<const void**>(traitKeys),
        reinterpret_cast<const void**>(traitVals), 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFStringRef keys[] = { kCTFontTraitsAttribute };
    CFTypeRef vals[] = { traitDict };
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr,
        reinterpret_cast<const void**>(keys),
        reinterpret_cast<const void**>(vals), 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);
    CFRelease(traitDict);
    CFRelease(traitNum);

    if (!desc) return {};

    CTFontRef font = CTFontCreateWithFontDescriptor(desc, 16.0, nullptr);
    CFRelease(desc);

    if (!font) return {};

    CFURLRef url = static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute));
    CFRelease(font);

    if (!url) return {};

    char pathBuf[1024];
    bool ok = CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(pathBuf), sizeof(pathBuf));
    CFRelease(url);

    return ok ? std::string(pathBuf) : std::string{};
}

} // namespace

std::string resolveFontFamily(const std::string& family, FontTraits traits)
{
    if (isGenericMonoFamily(family)) {
        for (const auto& candidate : preferredMonospaceFonts()) {
            auto result = resolveByFamily(candidate, traits);
            if (!result.empty()) return result;
        }
        return resolveByMonoTrait(traits);
    }

    return resolveByFamily(family, traits, true);
}
