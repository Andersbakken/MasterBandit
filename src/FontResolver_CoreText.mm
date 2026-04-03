#include "FontResolver.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

std::string resolveFontFamily(const std::string& family)
{
    CFStringRef cfFamily = CFStringCreateWithCString(nullptr, family.c_str(), kCFStringEncodingUTF8);
    CTFontDescriptorRef desc = CTFontDescriptorCreateWithNameAndSize(cfFamily, 0.0);
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
