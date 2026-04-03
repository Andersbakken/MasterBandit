#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

extern "C" void* createMetalLayer(void* nsWindow)
{
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    NSView* contentView = [window contentView];
    [contentView setWantsLayer:YES];
    CAMetalLayer* metalLayer = [CAMetalLayer layer];
    [contentView setLayer:metalLayer];
    return metalLayer;
}
