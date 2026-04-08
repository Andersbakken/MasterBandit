#include "Window_cocoa.h"

#include "../../platform/InputTypes.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <spdlog/spdlog.h>

// ---------- macOS keycode → Key table (adapted from GLFW cocoa_window.m) ----------

static Key macKeyToKey(unsigned short keyCode)
{
    // Virtual key codes from HIToolbox/Events.h
    // Mapping matches GLFW's createKeyTables() in cocoa_init.m
    switch (keyCode) {
    // Letters (macOS keyCodes are physical positions, not ASCII)
    case 0x00: return Key_A;
    case 0x0B: return Key_B;
    case 0x08: return Key_C;
    case 0x02: return Key_D;
    case 0x0E: return Key_E;
    case 0x03: return Key_F;
    case 0x05: return Key_G;
    case 0x04: return Key_H;
    case 0x22: return Key_I;
    case 0x26: return Key_J;
    case 0x28: return Key_K;
    case 0x25: return Key_L;
    case 0x2E: return Key_M;
    case 0x2D: return Key_N;
    case 0x1F: return Key_O;
    case 0x23: return Key_P;
    case 0x0C: return Key_Q;
    case 0x0F: return Key_R;
    case 0x01: return Key_S;
    case 0x11: return Key_T;
    case 0x20: return Key_U;
    case 0x09: return Key_V;
    case 0x0D: return Key_W;
    case 0x07: return Key_X;
    case 0x10: return Key_Y;
    case 0x06: return Key_Z;
    // Numbers
    case 0x1D: return Key_0;
    case 0x12: return Key_1;
    case 0x13: return Key_2;
    case 0x14: return Key_3;
    case 0x15: return Key_4;
    case 0x17: return Key_5;
    case 0x16: return Key_6;
    case 0x1A: return Key_7;
    case 0x1C: return Key_8;
    case 0x19: return Key_9;
    // Punctuation / symbols
    case 0x27: return Key_Apostrophe;
    case 0x2A: return Key_Backslash;
    case 0x2B: return Key_Comma;
    case 0x18: return Key_Equal;
    case 0x32: return Key_QuoteLeft;
    case 0x21: return Key_BracketLeft;
    case 0x1B: return Key_Minus;
    case 0x2F: return Key_Period;
    case 0x1E: return Key_BracketRight;
    case 0x29: return Key_Semicolon;
    case 0x2C: return Key_Slash;
    case 0x31: return Key_Space;
    // Special keys
    case 0x35: return Key_Escape;
    case 0x30: return Key_Tab;
    case 0x33: return Key_Backspace;
    case 0x24: return Key_Return;
    case 0x4C: return Key_Enter;          // KP Enter
    case 0x72: return Key_Insert;         // Help → Insert
    case 0x75: return Key_Delete;
    case 0x47: return Key_NumLock;        // Clear → NumLock
    case 0x73: return Key_Home;
    case 0x77: return Key_End;
    case 0x7B: return Key_Left;
    case 0x7E: return Key_Up;
    case 0x7C: return Key_Right;
    case 0x7D: return Key_Down;
    case 0x74: return Key_PageUp;
    case 0x79: return Key_PageDown;
    case 0x38: return Key_Shift_L;
    case 0x3C: return Key_Shift_R;
    case 0x3B: return Key_Control_L;
    case 0x3E: return Key_Control_R;
    case 0x3A: return Key_Alt_L;
    case 0x3D: return Key_Alt_R;
    case 0x37: return Key_Super_L;        // Command
    case 0x36: return Key_Super_R;
    case 0x39: return Key_CapsLock;
    case 0x6E: return Key_Menu;
    case 0x45: return Key_KP_Add;
    case 0x4E: return Key_KP_Subtract;
    case 0x43: return Key_KP_Multiply;
    case 0x4B: return Key_KP_Divide;
    case 0x41: return Key_KP_Decimal;
    case 0x51: return Key_KP_Equal;
    case 0x52: return Key_KP_0;
    case 0x53: return Key_KP_1;
    case 0x54: return Key_KP_2;
    case 0x55: return Key_KP_3;
    case 0x56: return Key_KP_4;
    case 0x57: return Key_KP_5;
    case 0x58: return Key_KP_6;
    case 0x59: return Key_KP_7;
    case 0x5B: return Key_KP_8;
    case 0x5C: return Key_KP_9;
    case 0x7A: return Key_F1;
    case 0x78: return Key_F2;
    case 0x63: return Key_F3;
    case 0x76: return Key_F4;
    case 0x60: return Key_F5;
    case 0x61: return Key_F6;
    case 0x62: return Key_F7;
    case 0x64: return Key_F8;
    case 0x65: return Key_F9;
    case 0x6D: return Key_F10;
    case 0x67: return Key_F11;
    case 0x6F: return Key_F12;
    case 0x69: return Key_F13;
    case 0x6B: return Key_F14;
    case 0x71: return Key_F15;
    case 0x6A: return Key_F16;
    case 0x40: return Key_F17;
    case 0x4F: return Key_F18;
    case 0x50: return Key_F19;
    case 0x5A: return Key_F20;
    default:   return Key_unknown;
    }
}

static unsigned int nsModsToModifiers(NSEventModifierFlags flags)
{
    unsigned int m = 0;
    if (flags & NSEventModifierFlagShift)    m |= ShiftModifier;
    if (flags & NSEventModifierFlagControl)  m |= CtrlModifier;
    if (flags & NSEventModifierFlagOption)   m |= AltModifier;
    if (flags & NSEventModifierFlagCommand)  m |= MetaModifier;
    if (flags & NSEventModifierFlagCapsLock) m |= CapsLockModifier;
    return m;
}

// ---------- MBWindowDelegate ----------

@interface MBWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) CocoaWindow* cppWindow;
@end

@implementation MBWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    self.cppWindow->dispatchClose();
    return NO;
}
- (void)windowDidResize:(NSNotification*)notification {
    NSWindow* win = notification.object;
    NSRect frame = [win contentView].frame;
    NSRect backing = [[win contentView] convertRectToBacking:frame];
    self.cppWindow->dispatchResize(static_cast<int>(backing.size.width),
                                    static_cast<int>(backing.size.height));
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    NSWindow* win = notification.object;
    CGFloat sx = win.backingScaleFactor;
    self.cppWindow->dispatchContentScale(static_cast<float>(sx), static_cast<float>(sx));
}
- (void)windowDidBecomeKey:(NSNotification*)notification { (void)notification; self.cppWindow->dispatchFocus(true); }
- (void)windowDidResignKey:(NSNotification*)notification { (void)notification; self.cppWindow->dispatchFocus(false); }
@end

// ---------- MBView ----------

@interface MBView : NSView <NSTextInputClient>
@property (nonatomic, assign) CocoaWindow* cppWindow;
@property (nonatomic, strong) NSMutableAttributedString* markedText;
@end

@implementation MBView

- (instancetype)initWithFrame:(NSRect)frame cppWindow:(CocoaWindow*)cppWindow {
    self = [super initWithFrame:frame];
    if (self) {
        _cppWindow = cppWindow;
        _markedText = [[NSMutableAttributedString alloc] init];
        self.wantsLayer = YES;
        self.layer = [CAMetalLayer layer];
    }
    return self;
}

- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }

// ---------- keyboard ----------

- (void)keyDown:(NSEvent*)event {
    [self interpretKeyEvents:@[event]];  // drives NSTextInputClient for IME

    Key k = macKeyToKey(event.keyCode);
    unsigned int mods = nsModsToModifiers(event.modifierFlags);
    KeyAction action = event.isARepeat ? KeyAction_Repeat : KeyAction_Press;
    _cppWindow->dispatchKey(static_cast<int>(k),
                             static_cast<int>(event.keyCode),
                             static_cast<int>(action),
                             static_cast<int>(mods));
}

- (void)keyUp:(NSEvent*)event {
    Key k = macKeyToKey(event.keyCode);
    unsigned int mods = nsModsToModifiers(event.modifierFlags);
    _cppWindow->dispatchKey(static_cast<int>(k),
                             static_cast<int>(event.keyCode),
                             static_cast<int>(KeyAction_Release),
                             static_cast<int>(mods));
}

- (void)flagsChanged:(NSEvent*)event {
    // Modifier-only key events
    Key k = macKeyToKey(event.keyCode);
    unsigned int mods = nsModsToModifiers(event.modifierFlags);
    // Determine press vs release by checking if the modifier bit is now set
    bool pressed = (mods != 0);
    _cppWindow->dispatchKey(static_cast<int>(k),
                             static_cast<int>(event.keyCode),
                             static_cast<int>(pressed ? KeyAction_Press : KeyAction_Release),
                             static_cast<int>(mods));
}

// ---------- NSTextInputClient (IME / compose) ----------

- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    NSString* s = [string isKindOfClass:[NSAttributedString class]]
                ? [(NSAttributedString*)string string] : (NSString*)string;
    for (NSUInteger i = 0; i < s.length; ) {
        UTF32Char cp;
        NSRange r = [s rangeOfComposedCharacterSequenceAtIndex:i];
        [s getBytes:&cp maxLength:4 usedLength:nil encoding:NSUTF32LittleEndianStringEncoding
            options:0 range:r remainingRange:nil];
        // Skip C0 and C1 control characters — they are handled by keyDown → dispatchKey
        // (matches GLFW's _glfwInputChar filter)
        if (cp >= 32 && !(cp > 126 && cp < 160))
            _cppWindow->dispatchChar(static_cast<uint32_t>(cp));
        i += r.length;
    }
    [_markedText setAttributedString:[[NSAttributedString alloc] init]];
}

- (void)doCommandBySelector:(SEL)selector {
    // Intentionally empty — suppress system beep for unhandled actions
    // (Return → insertNewline:, Tab → insertTab:, etc.)
    (void)selector;
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    (void)sel; (void)rep;
    NSAttributedString* as = [string isKindOfClass:[NSAttributedString class]]
                            ? (NSAttributedString*)string
                            : [[NSAttributedString alloc] initWithString:(NSString*)string];
    [_markedText setAttributedString:as];
}

- (void)unmarkText {
    [_markedText setAttributedString:[[NSAttributedString alloc] init]];
}

- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange {
    if (_markedText.length > 0) return NSMakeRange(0, _markedText.length);
    return NSMakeRange(NSNotFound, 0);
}
- (BOOL)hasMarkedText { return _markedText.length > 0; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a {
    (void)r; (void)a; return nil;
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a {
    (void)r; (void)a; return NSZeroRect;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)p { (void)p; return 0; }

// ---------- mouse ----------

- (void)mouseDown:(NSEvent*)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    _cppWindow->dispatchCursorPos(p.x, self.bounds.size.height - p.y);
    _cppWindow->dispatchMouseButton(static_cast<int>(LeftButton),
                                     static_cast<int>(KeyAction_Press),
                                     static_cast<int>(nsModsToModifiers(event.modifierFlags)));
}
- (void)mouseUp:(NSEvent*)event {
    (void)event;
    _cppWindow->dispatchMouseButton(static_cast<int>(LeftButton),
                                     static_cast<int>(KeyAction_Release),
                                     static_cast<int>(nsModsToModifiers(event.modifierFlags)));
}
- (void)rightMouseDown:(NSEvent*)event {
    _cppWindow->dispatchMouseButton(static_cast<int>(RightButton),
                                     static_cast<int>(KeyAction_Press),
                                     static_cast<int>(nsModsToModifiers(event.modifierFlags)));
}
- (void)rightMouseUp:(NSEvent*)event {
    (void)event;
    _cppWindow->dispatchMouseButton(static_cast<int>(RightButton),
                                     static_cast<int>(KeyAction_Release),
                                     static_cast<int>(nsModsToModifiers(event.modifierFlags)));
}
- (void)otherMouseDown:(NSEvent*)event {
    _cppWindow->dispatchMouseButton(static_cast<int>(MidButton),
                                     static_cast<int>(KeyAction_Press),
                                     static_cast<int>(nsModsToModifiers(event.modifierFlags)));
}
- (void)otherMouseUp:(NSEvent*)event {
    (void)event;
    _cppWindow->dispatchMouseButton(static_cast<int>(MidButton),
                                     static_cast<int>(KeyAction_Release),
                                     static_cast<int>(nsModsToModifiers(event.modifierFlags)));
}
- (void)mouseMoved:(NSEvent*)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    _cppWindow->dispatchCursorPos(p.x, self.bounds.size.height - p.y);
}
- (void)mouseDragged:(NSEvent*)event  { [self mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }

- (void)scrollWheel:(NSEvent*)event {
    _cppWindow->dispatchScroll(event.scrollingDeltaX, event.scrollingDeltaY);
}

@end

// ---------- CocoaWindow ----------

CocoaWindow::CocoaWindow() = default;

CocoaWindow::~CocoaWindow()
{
    destroy();
}

bool CocoaWindow::create(int width, int height, const std::string& title)
{
    NSRect frame = NSMakeRect(0, 0, width, height);
    nsWindow_ = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];

    nsWindow_.title = [NSString stringWithUTF8String:title.c_str()];

    MBWindowDelegate* delegate = [[MBWindowDelegate alloc] init];
    delegate.cppWindow = this;
    nsWindow_.delegate = delegate;

    NSRect contentRect = [nsWindow_ contentRectForFrameRect:frame];
    mbView_ = [[MBView alloc] initWithFrame:contentRect cppWindow:this];
    [nsWindow_ setContentView:mbView_];
    [nsWindow_ makeFirstResponder:mbView_];

    [nsWindow_ center];
    [nsWindow_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    return true;
}

void CocoaWindow::destroy()
{
    if (nsWindow_) {
        [nsWindow_ close];
        nsWindow_ = nullptr;
        mbView_   = nullptr;
    }
}

void CocoaWindow::setTitle(const std::string& title)
{
    if (nsWindow_)
        nsWindow_.title = [NSString stringWithUTF8String:title.c_str()];
}

void CocoaWindow::getFramebufferSize(int& w, int& h) const
{
    if (!nsWindow_) { w = h = 0; return; }
    NSRect backing = [[nsWindow_ contentView] convertRectToBacking:
                      [nsWindow_ contentView].bounds];
    w = static_cast<int>(backing.size.width);
    h = static_cast<int>(backing.size.height);
}

void CocoaWindow::getContentScale(float& x, float& y) const
{
    float scale = nsWindow_ ? static_cast<float>(nsWindow_.backingScaleFactor) : 1.0f;
    x = y = scale;
}

void CocoaWindow::setClipboard(const std::string& text)
{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:text.c_str()]
          forType:NSPasteboardTypeString];
}

std::string CocoaWindow::getClipboard() const
{
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    NSString* s = [pb stringForType:NSPasteboardTypeString];
    return s ? std::string([s UTF8String]) : std::string{};
}

std::string CocoaWindow::keyName(int keycode) const
{
    // Map virtual keycode to a printable character name using current keyboard layout
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    CFDataRef layoutData = static_cast<CFDataRef>(
        TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (!layoutData) { CFRelease(source); return {}; }

    const UCKeyboardLayout* layout =
        reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layoutData));
    UInt32 deadKeyState = 0;
    UniChar chars[8];
    UniCharCount len = 0;
    UCKeyTranslate(layout, static_cast<UInt16>(keycode),
                    kUCKeyActionDisplay, 0, LMGetKbdType(),
                    kUCKeyTranslateNoDeadKeysBit, &deadKeyState, 8, &len, chars);
    CFRelease(source);
    if (len == 0) return {};
    return std::string(reinterpret_cast<const char*>(chars), len);
}

wgpu::Surface CocoaWindow::createWgpuSurface(wgpu::Instance instance)
{
    CAMetalLayer* layer = static_cast<CAMetalLayer*>([mbView_ layer]);

    wgpu::SurfaceSourceMetalLayer metalSource;
    metalSource.layer = layer;

    wgpu::SurfaceDescriptor desc;
    desc.nextInChain = &metalSource;
    return instance.CreateSurface(&desc);
}

// ---------- dispatch helpers ----------

void CocoaWindow::dispatchKey(int key, int scancode, int action, int mods)
{
    if (onKey) onKey(key, scancode, action, mods);
}
void CocoaWindow::dispatchChar(uint32_t cp)
{
    if (onChar) onChar(cp);
}
void CocoaWindow::dispatchMouseButton(int button, int action, int mods)
{
    if (onMouseButton) onMouseButton(button, action, mods);
}
void CocoaWindow::dispatchCursorPos(double x, double y)
{
    if (onCursorPos) onCursorPos(x, y);
}
void CocoaWindow::dispatchScroll(double dx, double dy)
{
    if (onScroll) onScroll(dx, dy);
}
void CocoaWindow::dispatchFocus(bool focused)
{
    if (onFocus) onFocus(focused);
}
void CocoaWindow::dispatchResize(int w, int h)
{
    if (onFramebufferResize) onFramebufferResize(w, h);
}
void CocoaWindow::dispatchContentScale(float sx, float sy)
{
    if (onContentScale) onContentScale(sx, sy);
}
void CocoaWindow::dispatchClose()
{
    shouldClose_ = true;
}
