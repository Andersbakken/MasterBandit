#include "EventLoop_nsapp.h"

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreServices/CoreServices.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

// ---------- Objective-C glue ----------

@interface MBAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation MBAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    return NSTerminateCancel;  // let PlatformDawn handle quit
}
@end

// ---------- FSEvents callback ----------

static void fsEventsCallback(ConstFSEventStreamRef, void* info,
                              size_t, void*, const FSEventStreamEventFlags*,
                              const FSEventStreamEventId*)
{
    auto* loop = static_cast<NSAppEventLoop*>(info);
    loop->fileChanged();
}

// ---------- NSAppEventLoop ----------

NSAppEventLoop::NSAppEventLoop()
{
    // Ensure NSApp exists
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    id delegate = [[MBAppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    // CFRunLoop observer: fires before the run loop sleeps, calls onTick
    CFRunLoopObserverContext ctx{};
    ctx.info = this;
    observer_ = CFRunLoopObserverCreateWithHandler(
        kCFAllocatorDefault,
        kCFRunLoopBeforeWaiting | kCFRunLoopAfterWaiting,
        true, 0,
        ^(CFRunLoopObserverRef, CFRunLoopActivity activity) {
            if (activity == kCFRunLoopBeforeWaiting && onTick)
                onTick();
        });
    CFRunLoopAddObserver(CFRunLoopGetMain(),
                          static_cast<CFRunLoopObserverRef>(observer_),
                          kCFRunLoopDefaultMode);
}

NSAppEventLoop::~NSAppEventLoop()
{
    removeFileWatch();

    // Remove all fd sources
    for (auto& [fd, entry] : fds_) {
        if (entry.cfSource) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(),
                                   static_cast<CFRunLoopSourceRef>(entry.cfSource),
                                   kCFRunLoopDefaultMode);
            CFRelease(static_cast<CFRunLoopSourceRef>(entry.cfSource));
        }
        if (entry.cfFdRef)
            CFRelease(static_cast<CFFileDescriptorRef>(entry.cfFdRef));
    }

    // Cancel all timers
    for (auto& t : timers_) {
        if (t.nsTimer) {
            [t.nsTimer invalidate];
        }
    }

    if (observer_) {
        CFRunLoopRemoveObserver(CFRunLoopGetMain(),
                                 static_cast<CFRunLoopObserverRef>(observer_),
                                 kCFRunLoopDefaultMode);
        CFRelease(static_cast<CFRunLoopObserverRef>(observer_));
    }
}

// ---------- run / stop / wakeup ----------

void NSAppEventLoop::run()
{
    [NSApp run];
}

void NSAppEventLoop::stop()
{
    [NSApp stop:nil];
    wakeup();  // post an event to unblock the run loop
}

void NSAppEventLoop::wakeup()
{
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

void NSAppEventLoop::tick()
{
    if (onTick) onTick();
}

void NSAppEventLoop::fdReady(int fd, FdEvents events)
{
    auto it = fds_.find(fd);
    if (it != fds_.end())
        it->second.cb(events);
}

void NSAppEventLoop::fileChanged()
{
    if (fileWatchCb_) fileWatchCb_();
}

// ---------- fd watching ----------

// We store two pointers in the CFFileDescriptorContext.info:
//   [0] = &FdEntry (for the callback to find the cb)
//   [1] = NSAppEventLoop* (for fdReady dispatch)
// We achieve this by embedding them in a small heap struct.
struct FdCallbackInfo {
    NSAppEventLoop*          loop;
    NSAppEventLoop::FdEntry* entry;
};

static void cfFdCallbackV2(CFFileDescriptorRef fdRef, CFOptionFlags callBackTypes, void* info)
{
    auto* cbInfo = static_cast<FdCallbackInfo*>(info);

    EventLoop::FdEvents fired = static_cast<EventLoop::FdEvents>(0);
    if (callBackTypes & kCFFileDescriptorReadCallBack)
        fired = fired | EventLoop::FdEvents::Readable;
    if (callBackTypes & kCFFileDescriptorWriteCallBack)
        fired = fired | EventLoop::FdEvents::Writable;

    CFFileDescriptorEnableCallBacks(fdRef, callBackTypes);
    cbInfo->loop->fdReady(CFFileDescriptorGetNativeDescriptor(fdRef), fired);
}

void NSAppEventLoop::watchFd(int fd, FdEvents events, FdCb cb)
{
    FdEntry entry;
    entry.events = events;
    entry.cb     = std::move(cb);

    auto* cbInfo = new FdCallbackInfo{ this, nullptr };

    CFOptionFlags cfEvents = 0;
    if (events & FdEvents::Readable) cfEvents |= kCFFileDescriptorReadCallBack;
    if (events & FdEvents::Writable) cfEvents |= kCFFileDescriptorWriteCallBack;

    CFFileDescriptorContext ctx{};
    ctx.info = cbInfo;
    ctx.release = [](void* info) { delete static_cast<FdCallbackInfo*>(info); };

    CFFileDescriptorRef fdRef = CFFileDescriptorCreate(
        kCFAllocatorDefault, fd, false, cfFdCallbackV2, &ctx);
    CFFileDescriptorEnableCallBacks(fdRef, cfEvents);

    CFRunLoopSourceRef source = CFFileDescriptorCreateRunLoopSource(
        kCFAllocatorDefault, fdRef, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);

    entry.cfFdRef  = fdRef;
    entry.cfSource = source;

    auto [it, ok] = fds_.emplace(fd, std::move(entry));
    cbInfo->entry = &it->second;
}

void NSAppEventLoop::updateFd(int fd, FdEvents events)
{
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;

    it->second.events = events;
    CFFileDescriptorRef fdRef = static_cast<CFFileDescriptorRef>(it->second.cfFdRef);

    CFOptionFlags cfEvents = 0;
    if (events & FdEvents::Readable) cfEvents |= kCFFileDescriptorReadCallBack;
    if (events & FdEvents::Writable) cfEvents |= kCFFileDescriptorWriteCallBack;
    CFFileDescriptorEnableCallBacks(fdRef, cfEvents);
}

void NSAppEventLoop::removeFd(int fd)
{
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;

    if (it->second.cfSource) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(),
                               static_cast<CFRunLoopSourceRef>(it->second.cfSource),
                               kCFRunLoopDefaultMode);
        CFRelease(static_cast<CFRunLoopSourceRef>(it->second.cfSource));
    }
    if (it->second.cfFdRef)
        CFRelease(static_cast<CFFileDescriptorRef>(it->second.cfFdRef));

    fds_.erase(it);
}

// ---------- timers ----------

void NSAppEventLoop::timerFired(TimerId id)
{
    auto it = std::find_if(timers_.begin(), timers_.end(),
                            [id](const Timer& t) { return t.id == id; });
    if (it == timers_.end()) return;
    it->cb();
    if (!it->repeat) {
        [it->nsTimer invalidate];
        timers_.erase(it);
    }
}

EventLoop::TimerId NSAppEventLoop::addTimer(uint64_t ms, bool repeat, TimerCb cb)
{
    TimerId id = nextTimerId_++;
    NSTimeInterval interval = static_cast<double>(ms) / 1000.0;

    __block TimerId blockId = id;
    NSTimer* timer = [NSTimer scheduledTimerWithTimeInterval:interval
                                                     repeats:(repeat ? YES : NO)
                                                       block:^(NSTimer*) {
        this->timerFired(blockId);
    }];

    timers_.push_back({ id, ms, repeat, std::move(cb), timer });
    return id;
}

void NSAppEventLoop::removeTimer(TimerId id)
{
    auto it = std::find_if(timers_.begin(), timers_.end(),
                            [id](const Timer& t) { return t.id == id; });
    if (it == timers_.end()) return;
    [it->nsTimer invalidate];
    timers_.erase(it);
}

// ---------- file watching ----------

void NSAppEventLoop::addFileWatch(const std::string& path, WatchCb cb)
{
    removeFileWatch();
    fileWatchCb_ = std::move(cb);

    // Watch parent directory (handles atomic renames from editors)
    std::string dir = path;
    auto sep = dir.rfind('/');
    if (sep != std::string::npos) dir.resize(sep);
    if (dir.empty()) dir = ".";

    CFStringRef cfDir = CFStringCreateWithCString(nullptr, dir.c_str(), kCFStringEncodingUTF8);
    CFArrayRef  paths = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&cfDir), 1,
                                       &kCFTypeArrayCallBacks);
    CFRelease(cfDir);

    FSEventStreamContext ctx{};
    ctx.info = this;
    FSEventStreamRef stream = FSEventStreamCreate(
        nullptr, fsEventsCallback, &ctx,
        paths, kFSEventStreamEventIdSinceNow,
        0.1,  // latency seconds
        kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagFileEvents);
    CFRelease(paths);

    FSEventStreamSetDispatchQueue(stream, dispatch_get_main_queue());
    FSEventStreamStart(stream);
    fsEventStream_ = stream;
}

void NSAppEventLoop::removeFileWatch()
{
    if (!fsEventStream_) return;
    FSEventStreamRef stream = static_cast<FSEventStreamRef>(fsEventStream_);
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    fsEventStream_ = nullptr;
    fileWatchCb_   = nullptr;
}
