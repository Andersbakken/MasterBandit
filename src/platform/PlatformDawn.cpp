#include "PlatformDawn.h"
#include "Utils.h"
#include "FontResolver.h"

#ifdef __APPLE__
#  include <mac/EventLoop_nsapp.h>
#  include <mac/Window_cocoa.h>
#  include <kqueue/EventLoop_kqueue.h>
#elif defined(__linux__)
#  include <epoll/EventLoop_epoll.h>
#  include <xcb/Window_xcb.h>
#endif
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sys/ioctl.h>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fs = std::filesystem;

static std::vector<uint8_t> loadFontFile(const std::string& path) { return io::loadFile(path); }


PlatformDawn::PlatformDawn(int argc, char** argv, uint32_t flags)
    : flags_(flags)
{
    try {
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/mb.log", true);
        auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        debugSink_ = std::make_shared<DebugIPCSink>();
        std::vector<spdlog::sink_ptr> sinks = {fileSink, stderrSink, debugSink_};
        auto logger = std::make_shared<spdlog::logger>("mb", sinks.begin(), sinks.end());
        logger->set_level(spdlog::default_logger()->level()); // inherit level set in main()
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
    } catch (...) {}

    exeDir_ = fs::weakly_canonical(fs::path(argv[0])).parent_path().string();

    nativeInstance_ = std::make_unique<dawn::native::Instance>();
    wgpu::Instance instance(nativeInstance_->Get());

    wgpu::RequestAdapterOptions adapterOpts = {};
    adapterOpts.powerPreference = wgpu::PowerPreference::HighPerformance;

    auto adapters = nativeInstance_->EnumerateAdapters(&adapterOpts);
    if (adapters.empty()) {
        spdlog::error("No suitable GPU adapter found");
        return;
    }

    dawn::native::Adapter nativeAdapter = adapters[0];
    wgpu::Adapter adapter(nativeAdapter.Get());

    wgpu::AdapterInfo info = {};
    adapter.GetInfo(&info);
    spdlog::info("GPU Adapter: {}", std::string_view(info.device.data, info.device.length));

    wgpu::DeviceDescriptor deviceDesc = {};
    deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        spdlog::error("Dawn error ({}): {}", static_cast<int>(type),
            std::string_view(message.data, message.length));
    });
    deviceDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
            if (reason == wgpu::DeviceLostReason::Destroyed) return;
            spdlog::error("Device lost ({}): {}", static_cast<int>(reason),
                std::string_view(message.data, message.length));
        });

    WGPUDevice rawDevice = nativeAdapter.CreateDevice(&deviceDesc);
    if (!rawDevice) {
        spdlog::error("Failed to create Dawn device");
        return;
    }
    device_ = wgpu::Device::Acquire(rawDevice);
    queue_ = device_.GetQueue();
    texturePool_.init(device_, wgpu::TextureFormat::BGRA8Unorm);
}

void PlatformDawn::postToMainThread(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(deferredMainMutex_);
        deferredMain_.push_back(std::move(fn));
    }
    if (eventLoop_) eventLoop_->wakeup();
}

void PlatformDawn::drainDeferredMain()
{
    // Called on the main thread from onTick under platformMutex_.
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lk(deferredMainMutex_);
        pending.swap(deferredMain_);
    }
    for (auto& fn : pending) fn();
}

void PlatformDawn::drainPendingExits()
{
    // Called on the main thread from onTick under platformMutex_. Skip when
    // the render thread has released platformMutex_ for its shaping section
    // but is still holding raw Tab/Pane/Terminal pointers captured before
    // the release — erasing now would dangle them. We'll retry on the next
    // tick (render wakes the event loop when clearing renderActive_).
    if (renderActive_.load(std::memory_order_acquire)) return;

    std::vector<Terminal*> exits;
    {
        std::lock_guard<std::mutex> lk(deferredExitMutex_);
        exits.swap(pendingExits_);
    }
    for (auto* t : exits) terminalExited(t);
}

void PlatformDawn::deferIfRendering(std::function<void()> fn)
{
    // Main thread holds platformMutex_ here. If renderActive_ is clear, the
    // render thread is not in its shaping-phase unlock window and can't
    // acquire a new raw-pointer capture until it takes platformMutex_
    // again — which is blocked by the caller. Safe to run `fn` directly.
    if (!renderActive_.load(std::memory_order_acquire)) {
        fn();
        return;
    }
    pendingTeardowns_.push_back(std::move(fn));
}

void PlatformDawn::drainPendingTeardowns()
{
    // Called on the main thread from onTick under platformMutex_. Same
    // renderActive_ gate as drainPendingExits — retry next tick if the
    // render thread is still mid-shaping.
    if (renderActive_.load(std::memory_order_acquire)) return;
    if (pendingTeardowns_.empty()) return;

    std::vector<std::function<void()>> fns;
    fns.swap(pendingTeardowns_);
    for (auto& fn : fns) fn();
}

void PlatformDawn::wakeRenderThread()
{
    {
        std::lock_guard<std::mutex> lk(renderCvMutex_);
        renderWake_.store(true, std::memory_order_release);
    }
    renderCv_.notify_one();
}

void PlatformDawn::renderThreadMain()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lk(renderCvMutex_);
            renderCv_.wait(lk, [this] {
                return renderWake_.load(std::memory_order_acquire) ||
                       renderStop_.load(std::memory_order_acquire);
            });
            renderWake_.store(false, std::memory_order_relaxed);
        }
        if (renderStop_.load(std::memory_order_acquire)) return;

        if (renderStop_.load(std::memory_order_acquire)) return;
        // Tick Dawn on the render thread: it drains device-side events
        // (completion callbacks, deferred destroys). Must not run on main
        // thread concurrently with render-thread encoding — Metal backend
        // asserts `encodeSignalEvent:value: with uncommitted encoder`.
        // Dawn Tick is thread-safe so it doesn't need platformMutex_.
        device_.Tick();
        // renderFrame manages platformMutex_ internally — it releases the
        // lock during shaping so input handlers / deferred callbacks on
        // main thread can proceed concurrently with the CPU-heavy section.
        renderFrame();
    }
}

PlatformDawn::~PlatformDawn()
{
    // Stop the render thread before destroying any Dawn / window state it
    // might be reading. Signal stop, wake it if idle, join.
    renderStop_.store(true, std::memory_order_release);
    wakeRenderThread();
    if (renderThread_.joinable()) renderThread_.join();

    // Drain any deferred releases that landed via GPU completion callbacks
    // between the render thread exiting and now — they hold raw pointers
    // into the texture / compute pools which are about to be destroyed.
    // Callbacks that fire *after* this drain harmlessly append to the
    // shared state (kept alive by lambda captures); their contents are
    // never observed, and the dangling pointers they contain are never
    // dereferenced.
    {
        auto& state = *deferredReleaseState_;
        std::lock_guard<std::mutex> lock(state.mutex);
        for (auto* t : state.textures) texturePool_.release(t);
        state.textures.clear();
        for (auto* cs : state.compute) renderer_.computePool().release(cs);
        state.compute.clear();
    }

    // Release held textures before clearing pool
    for (auto& [id, rs] : paneRenderStates_) {
        if (rs.heldTexture) texturePool_.release(rs.heldTexture);
        for (auto* t : rs.pendingRelease) texturePool_.release(t);
    }
    paneRenderStates_.clear();

    // Release tab bar textures
    if (tabBarTexture_) { texturePool_.release(tabBarTexture_); tabBarTexture_ = nullptr; }
    for (auto* t : pendingTabBarRelease_) texturePool_.release(t);
    pendingTabBarRelease_.clear();

    // Release headless composite texture
    headlessComposite_ = nullptr;

    // Release any pending compute states before destroying renderer
    for (auto* cs : pendingComputeRelease_) renderer_.computePool().release(cs);
    pendingComputeRelease_.clear();

    // Destroy tabs (and their layouts/panes/terminals) before GPU resources
    tabs_.clear();

    surface_ = nullptr;

    renderer_.destroy();
    queue_ = {};
    device_ = {};
    texturePool_.clear();
    nativeInstance_.reset();

    // Destroy the window (X11 connection) last — the nvidia driver
    // needs the display connection alive during Vulkan device teardown.
    if (window_) {
        window_->destroy();
        window_.reset();
    }
}

void PlatformDawn::createTerminal(const TerminalOptions& options)
{
    if (!device_) return;

    // One-time platform initialization (window/surface in windowed mode, font/renderer in both)
    if (!platformInitialized_) {
        platformInitialized_ = true;

        if (!isHeadless()) {
            // --- Windowed: create EventLoop + Window ---
#ifdef __APPLE__
            eventLoop_ = std::make_unique<NSAppEventLoop>();
            window_    = std::make_unique<CocoaWindow>();
#elif defined(__linux__)
            eventLoop_ = std::make_unique<EpollEventLoop>();
            window_.reset(new XCBWindow(*eventLoop_));
#endif

            // Wire up Window callbacks
            window_->onKey = [this](int key, int scancode, int action, int mods) {
                onKey(key, scancode, action, mods);
            };
            window_->onChar = [this](uint32_t cp) { onChar(cp); };
            window_->onFramebufferResize = [this](int w, int h) { onFramebufferResize(w, h); };
            window_->onContentScale = [this](float sx, float sy) {
                contentScaleX_ = sx; contentScaleY_ = sy;
            };
            window_->onMouseButton = [this](int button, int action, int mods) {
                onMouseButton(button, action, mods);
            };
            window_->onCursorPos = [this](double x, double y) { onCursorPos(x, y); };
            window_->onScroll = [this](double /*dx*/, double dy) {
                if (dy > 0) dispatchAction(Action::ScrollUp{});
                else if (dy < 0) dispatchAction(Action::ScrollDown{});
            };
            window_->onExpose = [this]() { setNeedsRedraw(); };
            window_->onLiveResizeEnd = [this]() {
                // Apply any debounced resize captured during the drag so the
                // final frame reflects the true window geometry.
                {
                    std::lock_guard<std::mutex> plk(platformMutex_);
                    flushPendingFramebufferResize();
                }
                setNeedsRedraw();
            };
            window_->onFocus = [this](bool focused) {
                windowHasFocus_ = focused;
                focusChanged_.store(true);
                setNeedsRedraw();
                Tab* t = activeTab();
                if (!t) return;
                if (t->hasOverlay()) {
                    t->topOverlay()->focusEvent(focused);
                } else if (Pane* fp = t->layout()->focusedPane()) {
                    if (fp->terminal()) fp->terminal()->focusEvent(focused);
                }
                setNeedsRedraw();
            };

            if (!window_->create(800, 600, "MasterBandit")) {
                spdlog::error("Failed to create window");
                return;
            }

            int w = 0, h = 0;
            window_->getFramebufferSize(w, h);
            fbWidth_  = static_cast<uint32_t>(w);
            fbHeight_ = static_cast<uint32_t>(h);

            float xscale = 1.0f, yscale = 1.0f;
            window_->getContentScale(xscale, yscale);
            contentScaleX_ = xscale;
            contentScaleY_ = yscale;
            fontSize_ = options.fontSize * xscale;
            baseFontSize_ = fontSize_;

            surface_ = window_->createWgpuSurface(nativeInstance_->Get());
            if (!surface_) {
                spdlog::error("Failed to create Dawn surface");
                return;
            }
            configureSurface(fbWidth_, fbHeight_);

            // Scale texture pool limit based on screen resolution.
            // A full-screen RGBA texture is w*h*4 bytes; allow 4x that for
            // comfortable pooling during resizes and multi-pane layouts.
            {
                int sw = 0, sh = 0;
                window_->getScreenSize(sw, sh);
                constexpr size_t kMinLimit = 128 * 1024 * 1024;
                constexpr size_t kMaxLimit = 512 * 1024 * 1024;
                size_t limit = kMinLimit;
                if (sw > 0 && sh > 0) {
                    size_t screenBytes = static_cast<size_t>(sw) * sh * 4;
                    limit = std::clamp(screenBytes * 4, kMinLimit, kMaxLimit);
                }
                texturePool_.setByteLimit(limit);
                spdlog::info("TexturePool: screen {}x{}, limit set to {:.0f} MB",
                             sw, sh, limit / (1024.0 * 1024.0));
            }

            // Observe system appearance changes for mode 2031
            platformObserveAppearanceChanges([this](bool isDark) {
                notifyAllTerminals([isDark](TerminalEmulator* term) {
                    term->notifyColorPreference(isDark);
                });
            });
        } else {
            // --- Headless: event loop but no window/surface ---
#ifdef __APPLE__
            eventLoop_ = std::make_unique<KQueueEventLoop>();
#elif defined(__linux__)
            eventLoop_ = std::make_unique<EpollEventLoop>();
#endif
            contentScaleX_ = contentScaleY_ = 1.0f;
            fontSize_ = testFontSize_;
            baseFontSize_ = fontSize_;
        }

        // Load font
        std::string fontPath;
        if (isHeadless() && !testFontPath_.empty()) {
            fontPath = testFontPath_;
        } else if (options.font.empty()) {
            fontPath = resolveFontFamily("monospace");
        } else {
            fontPath = resolveFontFamily(options.font);
            if (fontPath.empty()) {
                spdlog::warn("Font family '{}' not found, falling back to system monospace", options.font);
                fontPath = resolveFontFamily("monospace");
            }
        }
        if (fontPath.empty()) {
            spdlog::error("No monospace font found on system");
            return;
        }
        spdlog::info("Using font: {}", fontPath);
        primaryFontPath_ = fontPath;

        auto fontData = loadFontFile(fontPath);
        if (fontData.empty()) {
            spdlog::error("Failed to load font: {}", fontPath);
            return;
        }

        std::vector<std::vector<uint8_t>> fontList = {std::move(fontData)};

        if (!isHeadless()) {
            const std::string& family = options.font.empty() ? std::string{} : options.font;

            // Load bold variant
            bool hasBoldFont = false;
            if (!family.empty()) {
                std::string boldPath = resolveFontFamily(family, FontTraitBold);
                if (!boldPath.empty() && boldPath != fontPath) {
                    auto boldData = loadFontFile(boldPath);
                    if (!boldData.empty()) {
                        spdlog::info("Using bold font: {}", boldPath);
                        fontList.push_back(std::move(boldData));
                        hasBoldFont = true;
                    }
                }
            }

            // Load italic variant
            bool hasItalicFont = false;
            uint32_t italicFontIndex = 0;
            if (!family.empty()) {
                std::string italicPath = resolveFontFamily(family, FontTraitItalic);
                if (!italicPath.empty() && italicPath != fontPath) {
                    auto italicData = loadFontFile(italicPath);
                    if (!italicData.empty()) {
                        spdlog::info("Using italic font: {}", italicPath);
                        italicFontIndex = static_cast<uint32_t>(fontList.size());
                        fontList.push_back(std::move(italicData));
                        hasItalicFont = true;
                    }
                }
            }

            textSystem_.registerFont(fontName_, fontList, 48.0f);
            textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
            textSystem_.setSystemFallback([this](const std::string& path, char32_t cp) {
                return fontFallback_.fontDataForCodepoint(path, cp);
            });
            textSystem_.setEmojiFallback([this](char32_t cp) {
                return fontFallback_.fontDataForEmoji(cp);
            });

            if (!hasBoldFont) {
                textSystem_.addSyntheticBoldVariant(fontName_, options.boldStrength, options.boldStrength);
            }
            textSystem_.setBoldStrength(options.boldStrength, options.boldStrength);

            if (hasItalicFont) {
                textSystem_.tagFontStyle(fontName_, italicFontIndex, {.bold = false, .italic = true});
            } else {
                textSystem_.addSyntheticItalicVariant(fontName_);
            }

            // Load bundled Symbols Nerd Font Mono as a built-in fallback
            auto nerdFontPath = fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf";
            auto nerdFontData = loadFontFile(nerdFontPath.string());
            if (!nerdFontData.empty()) {
                textSystem_.addFallbackFont(fontName_, nerdFontData);
                spdlog::info("Loaded built-in Symbols Nerd Font Mono");
            } else {
                spdlog::warn("Built-in Symbols Nerd Font Mono not found at {}", nerdFontPath.string());
            }

            // Load Noto Color Emoji if available (COLRv1 color emoji support).
            // Loaded before system fallback so it takes priority over Apple Color Emoji (sbix).
            auto notoEmojiPath = std::string(getenv("HOME") ? getenv("HOME") : "") + "/Library/Fonts/NotoColorEmoji-Regular.ttf";
            auto notoEmojiData = loadFontFile(notoEmojiPath);
            if (!notoEmojiData.empty()) {
                textSystem_.addFallbackFont(fontName_, notoEmojiData);
                spdlog::info("COLR: loaded Noto Color Emoji from {}", notoEmojiPath);
            }
        } else {
            // Headless: register font, optional emoji fallback for rendering tests
            textSystem_.registerFont(fontName_, fontList, 48.0f);
            textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
            textSystem_.addSyntheticBoldVariant(fontName_, options.boldStrength, options.boldStrength);
            textSystem_.setBoldStrength(options.boldStrength, options.boldStrength);
            if (!testFallbackFontPath_.empty()) {
                auto fallbackData = loadFontFile(testFallbackFontPath_);
                if (!fallbackData.empty()) {
                    textSystem_.addFallbackFont(fontName_, fallbackData);
                    spdlog::info("Test: loaded fallback font from {}", testFallbackFontPath_);
                }
            }
            if (!testEmojiFontPath_.empty()) {
                auto emojiData = loadFontFile(testEmojiFontPath_);
                if (!emojiData.empty()) {
                    textSystem_.addFallbackFont(fontName_, emojiData);
                    textSystem_.setEmojiFallback([emojiData](char32_t) { return emojiData; });
                    spdlog::info("Test: loaded emoji font from {}", testEmojiFontPath_);
                }
            }
        }

        const FontData* font = textSystem_.getFont(fontName_);
        if (!font) {
            spdlog::error("Failed to register font");
            return;
        }

        float scale = fontSize_ / font->baseSize;
        lineHeight_ = font->lineHeight * scale;

        const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
        charWidth_ = shaped.width;
        if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

        spdlog::info("Font metrics: charWidth={:.1f}, lineHeight={:.1f}", charWidth_, lineHeight_);

        // In headless mode, compute framebuffer size from target cols/rows
        if (isHeadless()) {
            fbWidth_ = static_cast<uint32_t>(std::ceil(testCols_ * charWidth_));
            fbHeight_ = static_cast<uint32_t>(std::ceil(testRows_ * lineHeight_));
            spdlog::info("Headless framebuffer: {}x{} ({}x{} cells)", fbWidth_, fbHeight_, testCols_, testRows_);
        }

        std::string shaderDir = fs::weakly_canonical(
            fs::path(exeDir_) / "shaders").string();
        if (!fs::exists(shaderDir)) {
            shaderDir = (fs::path(__FILE__).parent_path().parent_path() / "shaders").string();
        }
        renderer_.init(device_, queue_, shaderDir, fbWidth_, fbHeight_);
        renderer_.initProgressPipeline(device_, shaderDir);
        renderer_.uploadFontAtlas(queue_, fontName_, *font);

        // Spawn the render thread once all Dawn/surface/font state is
        // initialized. It sits idle on the condition variable until the main
        // thread signals a wake after parse/input/animation mutations.
        renderThread_ = std::thread([this] { renderThreadMain(); });
    }

    // Create a layout and tab for this terminal
    auto layout = std::make_unique<Layout>();
    layout->setDividerPixels(dividerWidth_);
    Pane* pane = layout->createPane();
    layout->setFocusedPane(pane->id());
    layout->computeRects(fbWidth_, fbHeight_);

    int paneId = pane->id();

    auto cbs = buildTerminalCallbacks(paneId);
    PlatformCallbacks pcbs;
    pcbs.onTerminalExited = [this](Terminal* t) {
        // Deferred: terminalExited fires from inside Terminal::readFromFD
        // (during parse, with Terminal mutex held). Running the structural
        // mutation immediately would require acquiring platformMutex_ while
        // holding Terminal mutex — the opposite of the render thread's
        // order and a deadlock. Queue and drain after parse.
        std::lock_guard<std::mutex> lk(deferredExitMutex_);
        pendingExits_.push_back(t);
        if (eventLoop_) eventLoop_->wakeup();
    };
    pcbs.quit = [this]() { quit(); };
    auto terminal = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));

    terminal->applyColorScheme(options.colors);
    terminal->applyCursorConfig(options.cursor);
    if (!terminal->init(options)) {
        spdlog::error("Failed to init terminal");
        return;
    }

    // Parse padding early so it's available for grid calculation
    if (padTop_ == 0 && padLeft_ == 0 && padRight_ == 0 && padBottom_ == 0) {
        padLeft_   = options.padding.left   * contentScaleX_;
        padTop_    = options.padding.top    * contentScaleX_;
        padRight_  = options.padding.right  * contentScaleX_;
        padBottom_ = options.padding.bottom * contentScaleX_;
    }

    // Compute cols/rows from pane rect
    const PaneRect& pr = pane->rect();
    float usableW = std::max(0.0f, static_cast<float>(pr.w > 0 ? pr.w : fbWidth_) - padLeft_ - padRight_);
    float usableH = std::max(0.0f, static_cast<float>(pr.h > 0 ? pr.h : fbHeight_) - padTop_ - padBottom_);
    int cols = (charWidth_ > 0) ? static_cast<int>(usableW / charWidth_) : 80;
    int rows = (lineHeight_ > 0) ? static_cast<int>(usableH / lineHeight_) : 24;
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;

    // Init per-pane render state
    auto& rs = paneRenderStates_[paneId];
    rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);

    terminal->resize(cols, rows);
    terminal->flushPendingResize(); // initial size — send immediately

    Terminal* termPtr = terminal.get();
    int masterFD = terminal->masterFD();

    // Title will be set by foreground process detection on first PTY read

    pane->setTerminal(std::move(terminal));  // Pane owns the Terminal now

    if (eventLoop_) {
        addPtyPoll(masterFD, termPtr);
    }

    // Build and store tab
    auto tab = std::make_unique<Tab>(std::move(layout));
    tab->setTitle(pane->title());
    activeTabIdx_ = static_cast<int>(tabs_.size());
    tabs_.push_back(std::move(tab));
    updateWindowTitle();

    // Store options for future createTab() calls
    if (tabs_.size() == 1) {
        terminalOptions_ = options;

        // Parse padding (scale by content scale)
        padLeft_   = options.padding.left   * contentScaleX_;
        padTop_    = options.padding.top    * contentScaleX_;
        padRight_  = options.padding.right  * contentScaleX_;
        padBottom_ = options.padding.bottom * contentScaleX_;

        // Parse color scheme
        const auto& cs = options.colors;
        defaultFgColor_ = color::parseHexRGBA(cs.foreground, 0xFFDDDDDD);
        defaultBgColor_ = color::parseHexRGBA(cs.background, 0x00000000);
    }

    // Initialize bindings and tab bar (only once, on first terminal creation)
    if (tabs_.size() == 1) {
        bindings_ = defaultBindings();
        auto userBindings = parseBindings(options.keybindings);
        bindings_.insert(bindings_.end(), userBindings.begin(), userBindings.end());

        mouseBindings_ = defaultMouseBindings();
        auto userMouseBindings = parseMouseBindings(options.mousebindings);
        mouseBindings_.insert(mouseBindings_.end(), userMouseBindings.begin(), userMouseBindings.end());

        // Divider color
        dividerWidth_ = std::max(0, options.dividerWidth);
        const std::string& dc = options.dividerColor;
        if (dc.size() == 7 && dc[0] == '#') {
            auto h = [&](int i) -> float {
                return std::stoul(dc.substr(i, 2), nullptr, 16) / 255.0f;
            };
            dividerR_ = h(1); dividerG_ = h(3); dividerB_ = h(5); dividerA_ = 1.0f;
        }

        // Pane tints
        auto parseTint = [](const std::string& col, float alpha, float out[4]) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (col.size() == 7 && col[0] == '#') {
                r = std::stoul(col.substr(1, 2), nullptr, 16) / 255.0f;
                g = std::stoul(col.substr(3, 2), nullptr, 16) / 255.0f;
                b = std::stoul(col.substr(5, 2), nullptr, 16) / 255.0f;
            }
            // Tint is a multiplicative blend: dim by blending toward tint color.
            // result = src * (tint * alpha + (1 - alpha))
            out[0] = r * alpha + (1.0f - alpha);
            out[1] = g * alpha + (1.0f - alpha);
            out[2] = b * alpha + (1.0f - alpha);
            out[3] = 1.0f;
        };
        parseTint(options.activePaneTint,   options.activePaneTintAlpha,   activeTint_);
        parseTint(options.inactivePaneTint, options.inactivePaneTintAlpha, inactiveTint_);
        if (!options.replacementChar.empty())
            replacementChar_ = options.replacementChar;

        renderer_.updateDividerViewport(queue_, fbWidth_, fbHeight_);
        tabBarConfig_ = options.tabBar;
        initTabBar(options.tabBar);
        // Recompute rects with tab bar height applied
        tabs_.back()->layout()->computeRects(fbWidth_, fbHeight_);
        // Recompute cols/rows for this pane after layout adjustment
        {
            Pane* p = tabs_.back()->layout()->focusedPane();
            if (p) {
                p->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);
                TerminalEmulator* te = p->terminal();
                if (te) {
                    int c = te->width(), r = te->height();
                    auto& rs2 = paneRenderStates_[p->id()];
                    rs2.resolvedCells.resize(static_cast<size_t>(c > 0 ? c : 1) * (r > 0 ? r : 1));
                }
            }
        }
    }

}

void PlatformDawn::configureSurface(uint32_t width, uint32_t height)
{
    wgpu::SurfaceConfiguration config = {};
    config.device = device_;
    config.format = wgpu::TextureFormat::BGRA8Unorm;
    config.width = width;
    config.height = height;
    // Prefer non-blocking present modes: Mailbox → FifoRelaxed → Fifo.
    // Dawn's vkAcquireNextImageKHR uses UINT64_MAX timeout, so Fifo can stall
    // the render thread indefinitely if the compositor pauses frame consumption.
    {
        static wgpu::PresentMode chosenMode = wgpu::PresentMode::Undefined;
        config.presentMode = wgpu::PresentMode::Fifo;
        wgpu::SurfaceCapabilities caps = {};
        if (surface_.GetCapabilities(device_.GetAdapter(), &caps) == wgpu::Status::Success) {
            for (auto mode : { wgpu::PresentMode::Mailbox,
                               wgpu::PresentMode::FifoRelaxed,
                               wgpu::PresentMode::Fifo }) {
                bool found = false;
                for (size_t i = 0; i < caps.presentModeCount; ++i)
                    if (caps.presentModes[i] == mode) { found = true; break; }
                if (found) { config.presentMode = mode; break; }
            }
        }
        if (config.presentMode != chosenMode) {
            chosenMode = config.presentMode;
            const char* name = config.presentMode == wgpu::PresentMode::Mailbox    ? "Mailbox"
                             : config.presentMode == wgpu::PresentMode::FifoRelaxed ? "FifoRelaxed"
                                                                                    : "Fifo";
            spdlog::info("Surface present mode: {}", name);
        }
    }
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    surface_.Configure(&config);
}

void PlatformDawn::invalidateAllRowCaches()
{
    for (auto& [id, rs] : paneRenderStates_) {
        for (auto& row : rs.rowShapingCache) row.valid = false;
        rs.dirty = true;
    }
    for (auto& [tab, rs] : overlayRenderStates_) {
        for (auto& row : rs.rowShapingCache) row.valid = false;
        rs.dirty = true;
    }
    for (auto& [key, rs] : popupRenderStates_) {
        for (auto& row : rs.rowShapingCache) row.valid = false;
        rs.dirty = true;
    }
}

static void applyTintColor(const std::string& col, float alpha, float out[4])
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (col.size() == 7 && col[0] == '#') {
        r = std::stoul(col.substr(1, 2), nullptr, 16) / 255.0f;
        g = std::stoul(col.substr(3, 2), nullptr, 16) / 255.0f;
        b = std::stoul(col.substr(5, 2), nullptr, 16) / 255.0f;
    }
    out[0] = r * alpha + (1.0f - alpha);
    out[1] = g * alpha + (1.0f - alpha);
    out[2] = b * alpha + (1.0f - alpha);
    out[3] = 1.0f;
}

void PlatformDawn::applyFontChange(const Config& config)
{
    std::string fontPath = config.font.empty() ? resolveFontFamily("monospace") : resolveFontFamily(config.font);
    if (fontPath.empty() && !config.font.empty())
        fontPath = resolveFontFamily("monospace");
    if (fontPath.empty()) {
        spdlog::warn("Config reload: font '{}' not found, keeping current font", config.font);
        return;
    }

    auto fontData = loadFontFile(fontPath);
    if (fontData.empty()) {
        spdlog::warn("Config reload: failed to load font '{}', keeping current font", fontPath);
        return;
    }

    std::vector<std::vector<uint8_t>> fontList = {std::move(fontData)};
    bool hasBoldFont = false;
    if (!config.font.empty()) {
        std::string boldPath = resolveFontFamily(config.font, FontTraitBold);
        if (!boldPath.empty() && boldPath != fontPath) {
            auto boldData = loadFontFile(boldPath);
            if (!boldData.empty()) {
                fontList.push_back(std::move(boldData));
                hasBoldFont = true;
            }
        }
    }

    // Tear down old font
    textSystem_.unregisterFont(fontName_);
    renderer_.removeFontAtlas(fontName_);

    // Re-register
    textSystem_.registerFont(fontName_, fontList, 48.0f);
    primaryFontPath_ = fontPath;
    textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
    textSystem_.setSystemFallback([this](const std::string& path, char32_t cp) {
        return fontFallback_.fontDataForCodepoint(path, cp);
    });
    textSystem_.setEmojiFallback([this](char32_t cp) {
        return fontFallback_.fontDataForEmoji(cp);
    });

    if (!hasBoldFont)
        textSystem_.addSyntheticBoldVariant(fontName_, config.bold_strength, config.bold_strength);
    textSystem_.setBoldStrength(config.bold_strength, config.bold_strength);

    auto nerdFontPath = fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf";
    auto nerdData = loadFontFile(nerdFontPath.string());
    if (!nerdData.empty())
        textSystem_.addFallbackFont(fontName_, nerdData);

    terminalOptions_.font = config.font;

    // Recalculate metrics
    const FontData* font = textSystem_.getFont(fontName_);
    if (!font) { spdlog::error("Config reload: font re-registration failed"); return; }

    float scale = fontSize_ / font->baseSize;
    lineHeight_ = font->lineHeight * scale;
    const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
    charWidth_ = shaped.width;
    if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

    renderer_.uploadFontAtlas(queue_, fontName_, *font);
    invalidateAllRowCaches();
    onFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));

    spdlog::info("Config reload: font changed to '{}'", fontPath);
}

void PlatformDawn::applyBlinkInterval(int ms)
{
    if (!eventLoop_) {
        cursorBlinkInterval_ = ms;
        return;
    }
    if (cursorBlinkTimer_) {
        eventLoop_->removeTimer(cursorBlinkTimer_);
        cursorBlinkTimer_ = 0;
    }
    cursorBlinkInterval_ = ms;
    cursorBlinkPhaseOn_ = true;
    if (ms <= 0) return;
    cursorBlinkTimer_ = eventLoop_->addTimer(static_cast<uint64_t>(ms), true, [this]() {
        cursorBlinkPhaseOn_ = !cursorBlinkPhaseOn_;
        // Only request a redraw if something in the *active* tab is being
        // rendered with a visible blinking cursor — inactive tabs aren't drawn,
        // and an active overlay supersedes the layout's panes/popups.
        Tab* tab = activeTab();
        if (!tab) return;
        auto wantsRedraw = [](TerminalEmulator* term) {
            return term && term->cursorBlinking() && term->cursorVisible();
        };
        if (tab->hasOverlay()) {
            if (wantsRedraw(tab->topOverlay())) setNeedsRedraw();
            return;
        }
        for (auto& panePtr : tab->layout()->panes()) {
            if (wantsRedraw(panePtr->terminal())) {
                setNeedsRedraw();
                return;
            }
            for (const auto& popup : panePtr->popups()) {
                if (wantsRedraw(popup.terminal.get())) {
                    setNeedsRedraw();
                    return;
                }
            }
        }
    });
}

void PlatformDawn::scheduleAnimationWakeup(uint64_t dueAt)
{
    // Called from the render thread during renderFrame(). The event loop
    // (addTimer / removeTimer) is not thread-safe, so we stash the request
    // in an atomic and let the main thread's onTick wire it up via
    // applyPendingAnimationWakeup(). Kick the event loop so onTick runs
    // even when nothing else is happening.
    pendingAnimationDueAt_.store(dueAt, std::memory_order_release);
    if (eventLoop_) eventLoop_->wakeup();
}

void PlatformDawn::applyPendingAnimationWakeup()
{
    // Main thread only. Consume the atomic request stashed by the render
    // thread and (re)arm the event loop timer.
    uint64_t dueAt = pendingAnimationDueAt_.exchange(0, std::memory_order_acquire);
    if (!dueAt || !eventLoop_) return;

    uint64_t now = TerminalEmulator::mono();
    if (dueAt <= now) {
        if (animationTimer_) {
            eventLoop_->removeTimer(animationTimer_);
            animationTimer_ = 0;
        }
        animationTimerDueAt_ = 0;
        setNeedsRedraw();
        return;
    }
    // Skip reschedule if an equivalent timer is already pending.
    if (animationTimer_ && animationTimerDueAt_ == dueAt) return;
    if (animationTimer_) {
        eventLoop_->removeTimer(animationTimer_);
        animationTimer_ = 0;
    }
    animationTimerDueAt_ = dueAt;
    uint64_t delay = dueAt - now;
    animationTimer_ = eventLoop_->addTimer(delay, false, [this]() {
        animationTimer_ = 0;
        animationTimerDueAt_ = 0;
        setNeedsRedraw();
    });
}

void PlatformDawn::reloadConfigNow()
{
    spdlog::info("Config: hot-reload triggered");
    Config config = loadConfig();

    // Keybindings
    bindings_ = defaultBindings();
    auto userBindings = parseBindings(config.keybindings);
    bindings_.insert(bindings_.end(), userBindings.begin(), userBindings.end());
    mouseBindings_ = defaultMouseBindings();
    auto userMouseBindings = parseMouseBindings(config.mousebindings);
    mouseBindings_.insert(mouseBindings_.end(), userMouseBindings.begin(), userMouseBindings.end());
    sequenceMatcher_.reset();

    // Colors
    terminalOptions_.colors = config.colors;
    defaultFgColor_ = color::parseHexRGBA(config.colors.foreground, 0xFFDDDDDD);
    defaultBgColor_ = color::parseHexRGBA(config.colors.background, 0x00000000);
    notifyAllTerminals([&config](TerminalEmulator* term) {
        term->applyColorScheme(config.colors);
    });
    invalidateAllRowCaches();

    // Cursor
    terminalOptions_.cursor = config.cursor;
    notifyAllTerminals([&config](TerminalEmulator* term) {
        term->applyCursorConfig(config.cursor);
    });
    applyBlinkInterval(config.cursor.blink_interval);

    // Divider
    dividerWidth_ = std::max(0, config.divider_width);
    const std::string& dc = config.divider_color;
    if (dc.size() == 7 && dc[0] == '#') {
        auto h = [&](int i) -> float { return std::stoul(dc.substr(i, 2), nullptr, 16) / 255.0f; };
        dividerR_ = h(1); dividerG_ = h(3); dividerB_ = h(5); dividerA_ = 1.0f;
    }
    for (auto& t : tabs_) t->layout()->setDividerPixels(dividerWidth_);
    terminalOptions_.dividerWidth = config.divider_width;
    terminalOptions_.dividerColor = config.divider_color;

    // Tints
    applyTintColor(config.active_pane_tint,   config.active_pane_tint_alpha,   activeTint_);
    applyTintColor(config.inactive_pane_tint, config.inactive_pane_tint_alpha, inactiveTint_);
    terminalOptions_.activePaneTint       = config.active_pane_tint;
    terminalOptions_.activePaneTintAlpha  = config.active_pane_tint_alpha;
    terminalOptions_.inactivePaneTint     = config.inactive_pane_tint;
    terminalOptions_.inactivePaneTintAlpha= config.inactive_pane_tint_alpha;

    // Padding
    float newPL = config.padding.left   * contentScaleX_;
    float newPT = config.padding.top    * contentScaleX_;
    float newPR = config.padding.right  * contentScaleX_;
    float newPB = config.padding.bottom * contentScaleX_;
    bool paddingChanged = (newPL != padLeft_ || newPT != padTop_ ||
                           newPR != padRight_ || newPB != padBottom_);
    padLeft_ = newPL; padTop_ = newPT; padRight_ = newPR; padBottom_ = newPB;
    terminalOptions_.padding = config.padding;

    // Replacement char
    if (!config.replacement_char.empty()) {
        replacementChar_ = config.replacement_char;
        terminalOptions_.replacementChar = config.replacement_char;
    }

    // Bold strength (only if font isn't changing — applyFontChange handles it otherwise)
    bool fontNameChanged = (config.font != terminalOptions_.font);
    if (!fontNameChanged) {
        textSystem_.setBoldStrength(config.bold_strength, config.bold_strength);
        terminalOptions_.boldStrength = config.bold_strength;
    }

    // Tab bar
    tabBarConfig_ = config.tab_bar;
    textSystem_.unregisterFont(tabBarFontName_);
    renderer_.removeFontAtlas(tabBarFontName_);
    initTabBar(config.tab_bar);

    // Font name or size change
    float newFontSize = config.font_size * contentScaleX_;
    if (fontNameChanged) {
        fontSize_ = newFontSize;
        baseFontSize_ = fontSize_;
        terminalOptions_.fontSize = config.font_size;
        applyFontChange(config);
    } else if (newFontSize != fontSize_) {
        fontSize_ = newFontSize;
        baseFontSize_ = fontSize_;
        terminalOptions_.fontSize = config.font_size;
        const FontData* font = textSystem_.getFont(fontName_);
        if (font) {
            float scale = fontSize_ / font->baseSize;
            lineHeight_ = font->lineHeight * scale;
            const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
            charWidth_ = shaped.width;
            if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;
        }
        invalidateAllRowCaches();
        onFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));
    } else if (paddingChanged) {
        onFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));
    }

    tabBarDirty_ = true;
    setNeedsRedraw();
    spdlog::info("Config reloaded: {} user keybindings", userBindings.size());
}

std::unique_ptr<PlatformDawn> createPlatform(int argc, char** argv, uint32_t flags)
{
    return std::make_unique<PlatformDawn>(argc, argv, flags);
}
