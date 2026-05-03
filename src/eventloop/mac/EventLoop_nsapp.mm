#include "EventLoop_nsapp.h"

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreServices/CoreServices.h>

#include <spdlog/spdlog.h>

#include <sys/event.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>

static constexpr int MaxKqEvents = 64;

// ---------- Objective-C glue ----------

@interface MBAppDelegate : NSObject <NSApplicationDelegate> {
@public
    NSAppEventLoop* loop;
}
- (void)mbQuit:(id)sender;
@end

@implementation MBAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    // Route through the shared quit callback (same path as mb.quit()) and
    // cancel the OS-driven termination so AppKit doesn't bypass our cleanup.
    if (loop && loop->onQuitRequested) loop->onQuitRequested();
    else if (loop) loop->stop();
    return NSTerminateCancel;
}
- (void)mbQuit:(id)sender {
    (void)sender;
    if (loop && loop->onQuitRequested) loop->onQuitRequested();
    else if (loop) loop->stop();
}
@end

// Build the standard macOS app menu (App > [About, Hide…, Quit]). The bundle
// name supplies "<App>" via NSRunningApplication.localizedName; falls back to
// "MasterBandit" when running the bare binary. The Quit item targets our
// delegate's mbQuit: rather than NSApp.terminate: so the cleanup path goes
// through onQuitRequested directly without involving applicationShouldTerminate.
static void installMainMenu(MBAppDelegate* delegate)
{
    NSString* appName = [[NSRunningApplication currentApplication] localizedName];
    if (!appName.length) appName = @"MasterBandit";

    NSMenu* mainMenu = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appMenuItem];

    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* hide = [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                                          action:@selector(hide:)
                                   keyEquivalent:@"h"];
    (void)hide;
    NSMenuItem* hideOthers = [appMenu addItemWithTitle:@"Hide Others"
                                                action:@selector(hideOtherApplications:)
                                         keyEquivalent:@"h"];
    [hideOthers setKeyEquivalentModifierMask:(NSEventModifierFlagOption | NSEventModifierFlagCommand)];
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:appName]
                                                  action:@selector(mbQuit:)
                                           keyEquivalent:@"q"];
    [quit setTarget:delegate];
    [appMenu addItem:quit];

    [appMenuItem setSubmenu:appMenu];
    [NSApp setMainMenu:mainMenu];
}

// ---------- FSEvents callback ----------

static void fsEventsCallback(ConstFSEventStreamRef, void* info,
                              size_t, void*, const FSEventStreamEventFlags*,
                              const FSEventStreamEventId*)
{
    auto* loop = static_cast<NSAppEventLoop*>(info);
    loop->fileChanged();
}

// ---------- CFFileDescriptor callback for kqueue ----------

static void kqueueCfCallback(CFFileDescriptorRef fdRef, CFOptionFlags, void* info)
{
    auto* loop = static_cast<NSAppEventLoop*>(info);
    loop->drainKqueue();
    // Re-arm the CFFileDescriptor so we get notified again
    CFFileDescriptorEnableCallBacks(fdRef, kCFFileDescriptorReadCallBack);
}

// ---------- NSAppEventLoop ----------

NSAppEventLoop::NSAppEventLoop()
{
    // Ensure NSApp exists
    [NSApplication sharedApplication];
    // Disable Press-and-Hold so interpretKeyEvents: delivers key repeats for
    // every key. With the default ON, the OS suppresses repeats for keys
    // without accent alternates (f, g, j, k…) and tries to show the accent
    // picker for the rest — neither is what a terminal wants.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"ApplePressAndHoldEnabled": @NO}];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    MBAppDelegate* delegate = [[MBAppDelegate alloc] init];
    delegate->loop = this;
    [NSApp setDelegate:delegate];
    installMainMenu(delegate);

    // Per-fd watching: delegate to FdPoller. Wrap its kqueue fd in
    // a CFFileDescriptor so the CFRunLoop wakes when any managed
    // fd has events; drainKqueue() then calls poll(0) to dispatch.
    fdPoller_ = std::make_unique<FdPollerKQueue>();
    int innerFd = fdPoller_->nativeHandle();
    if (innerFd < 0)
        throw std::runtime_error("FdPollerKQueue has no native handle");

    CFFileDescriptorContext ctx{};
    ctx.info = this;
    CFFileDescriptorRef cfFd = CFFileDescriptorCreate(
        kCFAllocatorDefault, innerFd, false, kqueueCfCallback, &ctx);
    CFFileDescriptorEnableCallBacks(cfFd, kCFFileDescriptorReadCallBack);

    CFRunLoopSourceRef source = CFFileDescriptorCreateRunLoopSource(
        kCFAllocatorDefault, cfFd, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);

    kqCfFdRef_  = cfFd;
    kqCfSource_ = source;

    // CFRunLoop observer: fires before sleeping and after waking, calls onTick
    CFRunLoopObserverContext obsCtx{};
    obsCtx.info = this;
    observer_ = CFRunLoopObserverCreateWithHandler(
        kCFAllocatorDefault,
        kCFRunLoopBeforeWaiting | kCFRunLoopAfterWaiting,
        true, 0,
        ^(CFRunLoopObserverRef, CFRunLoopActivity activity) {
            if (activity == kCFRunLoopBeforeWaiting) {
                if (onTick) onTick();
                if (wakeupPending_.exchange(false, std::memory_order_acquire)) {
                    CFRunLoopWakeUp(CFRunLoopGetMain());
                }
            } else if (activity == kCFRunLoopAfterWaiting) {
                if (onTick) onTick();
            }
        });
    CFRunLoopAddObserver(CFRunLoopGetMain(),
                          static_cast<CFRunLoopObserverRef>(observer_),
                          kCFRunLoopCommonModes);
}

NSAppEventLoop::~NSAppEventLoop()
{
    removeFileWatch();

    // Remove kqueue CFFileDescriptor (which wraps the FdPoller's
    // kqueue fd) before freeing the poller — the wrapper holds a
    // raw fd which must remain valid until invalidate.
    if (kqCfSource_) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(),
                               static_cast<CFRunLoopSourceRef>(kqCfSource_),
                               kCFRunLoopCommonModes);
        CFRelease(static_cast<CFRunLoopSourceRef>(kqCfSource_));
    }
    if (kqCfFdRef_) {
        CFFileDescriptorInvalidate(static_cast<CFFileDescriptorRef>(kqCfFdRef_));
        CFRelease(static_cast<CFFileDescriptorRef>(kqCfFdRef_));
    }
    fdPoller_.reset();

    // Cancel all timers
    for (auto& t : timers_) {
        if (t.nsTimer) {
            [t.nsTimer invalidate];
        }
    }

    if (observer_) {
        CFRunLoopRemoveObserver(CFRunLoopGetMain(),
                                 static_cast<CFRunLoopObserverRef>(observer_),
                                 kCFRunLoopCommonModes);
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
    // Thread-safe: CFRunLoopWakeUp is documented as callable from any thread.
    // wakeupPending_ is atomic.
    wakeupPending_.store(true, std::memory_order_release);
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

void NSAppEventLoop::tick()
{
    if (onTick) onTick();
}

void NSAppEventLoop::drainKqueue()
{
    if (fdPoller_) fdPoller_->poll(0);
}

void NSAppEventLoop::fileChanged()
{
    // FSEvents fires at the directory level. Every registered callback
    // gets called; the callback re-stats its own file to decide if the
    // change was relevant. Iterate by index for re-entrancy safety
    // (a callback may call removeFileWatch).
    for (size_t i = 0; i < fileWatches_.size(); ++i) {
        WatchCb cb = fileWatches_[i].cb;
        if (cb) cb();
    }
}

// ---------- fd watching (delegated to FdPoller) ----------

FdPoller::Events NSAppEventLoop::toPoller(FdEvents ev)
{
    return static_cast<FdPoller::Events>(static_cast<uint8_t>(ev));
}

EventLoop::FdEvents NSAppEventLoop::fromPoller(FdPoller::Events ev)
{
    return static_cast<FdEvents>(static_cast<uint8_t>(ev));
}

void NSAppEventLoop::watchFd(int fd, FdEvents events, FdCb cb)
{
    if (!fdPoller_) return;
    auto inner = std::move(cb);
    fdPoller_->add(fd, toPoller(events),
        [inner = std::move(inner)](FdPoller::Events ev) {
            if (inner) inner(fromPoller(ev));
        });
}

void NSAppEventLoop::updateFd(int fd, FdEvents events)
{
    if (!fdPoller_) return;
    fdPoller_->update(fd, toPoller(events));
}

void NSAppEventLoop::removeFd(int fd)
{
    if (!fdPoller_) return;
    fdPoller_->remove(fd);
}

// ---------- timers ----------

void NSAppEventLoop::timerFired(TimerId id)
{
    auto it = std::find_if(timers_.begin(), timers_.end(),
                            [id](const Timer& t) { return t.id == id; });
    if (it == timers_.end()) return;

    // Copy the fields we need *before* running the callback — it may mutate
    // timers_ via addTimer (push_back → reallocation) or removeTimer, either
    // of which invalidates `it`.
    TimerCb cb = it->cb;
    bool repeat = it->repeat;
    NSTimer* nsTimer = it->nsTimer;

    if (!repeat) {
        [nsTimer invalidate];
        timers_.erase(it);
    }
    // For repeat timers, `it` may dangle after cb() but we don't touch it again.
    cb();
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

void NSAppEventLoop::restartTimer(TimerId id)
{
    auto it = std::find_if(timers_.begin(), timers_.end(),
                            [id](const Timer& t) { return t.id == id; });
    if (it == timers_.end()) return;
    [it->nsTimer invalidate];
    NSTimeInterval interval = static_cast<double>(it->ms) / 1000.0;
    __block TimerId blockId = id;
    it->nsTimer = [NSTimer scheduledTimerWithTimeInterval:interval
                                                  repeats:(it->repeat ? YES : NO)
                                                    block:^(NSTimer*) {
        this->timerFired(blockId);
    }];
}

// ---------- file watching ----------

void NSAppEventLoop::addFileWatch(const std::string& path, WatchCb cb)
{
    // Watch parent directory (handles atomic renames from editors).
    std::string dir = path;
    auto sep = dir.rfind('/');
    if (sep != std::string::npos) dir.resize(sep);
    if (dir.empty()) dir = ".";

    // If we already have a watch for a different parent dir, drop it.
    if (!fileWatches_.empty() && dir != fileWatchDir_) {
        removeFileWatch();
    }

    if (!fsEventStream_) {
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
        fileWatchDir_  = dir;
    }
    fileWatches_.push_back({path, std::move(cb)});
}

void NSAppEventLoop::removeFileWatch()
{
    if (fsEventStream_) {
        FSEventStreamRef stream = static_cast<FSEventStreamRef>(fsEventStream_);
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        fsEventStream_ = nullptr;
    }
    fileWatchDir_.clear();
    fileWatches_.clear();
}
