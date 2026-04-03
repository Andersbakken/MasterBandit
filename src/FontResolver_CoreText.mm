#include "FontResolver.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

std::string resolveFontFamily(const std::string& family, bool bold)
{
    CFStringRef cfFamily = CFStringCreateWithCString(nullptr, family.c_str(), kCFStringEncodingUTF8);

    CTFontDescriptorRef desc;
    if (bold) {
        CFStringRef keys[] = { kCTFontFamilyNameAttribute, kCTFontTraitsAttribute };
        uint32_t trait = kCTFontBoldTrait;
        CFNumberRef traitNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &trait);
        CFStringRef traitKeys[] = { kCTFontSymbolicTrait };
        CFTypeRef traitVals[] = { traitNum };
        CFDictionaryRef traits = CFDictionaryCreate(nullptr,
            reinterpret_cast<const void**>(traitKeys),
            reinterpret_cast<const void**>(traitVals), 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFTypeRef vals[] = { cfFamily, traits };
        CFDictionaryRef attrs = CFDictionaryCreate(nullptr,
            reinterpret_cast<const void**>(keys),
            reinterpret_cast<const void**>(vals), 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        desc = CTFontDescriptorCreateWithAttributes(attrs);
        CFRelease(attrs);
        CFRelease(traits);
        CFRelease(traitNum);
    } else {
        desc = CTFontDescriptorCreateWithNameAndSize(cfFamily, 0.0);
    }
    CFRelease(cfFamily);

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
