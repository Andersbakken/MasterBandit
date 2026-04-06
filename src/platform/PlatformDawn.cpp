#include "PlatformDawn.h"
#include "NativeSurface.h"
#include "Utils.h"
#include "FontResolver.h"
#include "Log.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sys/ioctl.h>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fs = std::filesystem;

static std::vector<uint8_t> loadFontFile(const std::string& path) { return io::loadFile(path); }

// --- Find a monospace font on the system ---
static std::string findMonospaceFont()
{
    static const char* candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
#ifdef __APPLE__
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/SFMono-Regular.otf",
        "/Library/Fonts/Courier New.ttf",
#endif
        nullptr
    };

    for (const char** p = candidates; *p; ++p) {
        if (fs::exists(*p)) return *p;
    }
    return {};
}

PlatformDawn::PlatformDawn(int argc, char** argv, bool headless)
    : headless_(headless)
{
    try {
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/mb.log", true);
        auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        debugSink_ = std::make_shared<DebugIPCSink>();
        std::vector<spdlog::sink_ptr> sinks = {fileSink, stderrSink, debugSink_};
        auto logger = std::make_shared<spdlog::logger>("mb", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
    } catch (...) {}

    exeDir_ = fs::weakly_canonical(fs::path(argv[0])).parent_path().string();

    if (!headless_) {
        if (!glfwInit()) {
            spdlog::error("glfwInit failed");
            return;
        }
    }

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

PlatformDawn::~PlatformDawn()
{
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

    if (glfwWindow_) {
        glfwDestroyWindow(glfwWindow_);
        glfwWindow_ = nullptr;
    }

    renderer_.destroy();
    queue_ = {};
    device_ = {};
    texturePool_.clear();
    nativeInstance_.reset();
    if (!headless_) glfwTerminate();
}

void PlatformDawn::createTerminal(const TerminalOptions& options)
{
    if (!device_) return;

    // One-time platform initialization (window/surface in windowed mode, font/renderer in both)
    if (!platformInitialized_) {
        platformInitialized_ = true;

        if (!headless_) {
            // --- Windowed: create GLFW window and surface ---
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindow_ = glfwCreateWindow(800, 600, "MasterBandit", nullptr, nullptr);
            if (!glfwWindow_) {
                spdlog::error("glfwCreateWindow failed");
                return;
            }

            wgpu::Instance instance(nativeInstance_->Get());
            surface_ = createNativeSurface(glfwWindow_, instance);
            if (!surface_) {
                spdlog::error("Failed to create Dawn surface");
                glfwDestroyWindow(glfwWindow_);
                glfwWindow_ = nullptr;
                return;
            }

            int w, h;
            glfwGetFramebufferSize(glfwWindow_, &w, &h);
            fbWidth_ = static_cast<uint32_t>(w);
            fbHeight_ = static_cast<uint32_t>(h);

            float xscale, yscale;
            glfwGetWindowContentScale(glfwWindow_, &xscale, &yscale);
            contentScaleX_ = xscale;
            contentScaleY_ = yscale;
            fontSize_ = options.fontSize * xscale;
            baseFontSize_ = fontSize_;

            configureSurface(fbWidth_, fbHeight_);
        } else {
            // --- Headless: no window, no surface ---
            contentScaleX_ = contentScaleY_ = 1.0f;
            fontSize_ = testFontSize_;
            baseFontSize_ = fontSize_;
        }

        // Load font
        std::string fontPath;
        if (headless_ && !testFontPath_.empty()) {
            fontPath = testFontPath_;
        } else if (options.font.empty()) {
            fontPath = findMonospaceFont();
        } else {
            fontPath = resolveFontFamily(options.font);
            if (fontPath.empty()) {
                spdlog::warn("Font family '{}' not found, falling back to system monospace", options.font);
                fontPath = findMonospaceFont();
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

        if (!headless_) {
            // Load bold variant (windowed only)
            const std::string& family = options.font.empty() ? std::string{} : options.font;
            std::string boldPath = family.empty() ? std::string{} : resolveFontFamily(family, true);
            bool hasBoldFont = false;
            if (!boldPath.empty() && boldPath != fontPath) {
                auto boldData = loadFontFile(boldPath);
                if (!boldData.empty()) {
                    spdlog::info("Using bold font: {}", boldPath);
                    fontList.push_back(std::move(boldData));
                    hasBoldFont = true;
                }
            }

            textSystem_.registerFont(fontName_, fontList, 48.0f);
            textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
            textSystem_.setSystemFallback([this](const std::string& path, char32_t cp) {
                return fontFallback_.fontDataForCodepoint(path, cp);
            });

            if (!hasBoldFont) {
                textSystem_.addSyntheticBoldVariant(fontName_, options.boldStrength, options.boldStrength);
            }
            textSystem_.setBoldStrength(options.boldStrength, options.boldStrength);

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
            // Headless: single font, no fallback
            textSystem_.registerFont(fontName_, fontList, 48.0f);
            textSystem_.setPrimaryFontPath(fontName_, primaryFontPath_);
            textSystem_.addSyntheticBoldVariant(fontName_, options.boldStrength, options.boldStrength);
            textSystem_.setBoldStrength(options.boldStrength, options.boldStrength);
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
        if (headless_) {
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

        if (!headless_) {
            // Wire up GLFW callbacks
            glfwSetWindowUserPointer(glfwWindow_, this);

            glfwSetInputMode(glfwWindow_, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
            glfwSetKeyCallback(glfwWindow_, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
                static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w))->onKey(key, scancode, action, mods);
            });

            glfwSetCharCallback(glfwWindow_, [](GLFWwindow* w, unsigned int codepoint) {
                static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w))->onChar(codepoint);
            });

            glfwSetFramebufferSizeCallback(glfwWindow_, [](GLFWwindow* w, int width, int height) {
                static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w))->onFramebufferResize(width, height);
            });

            glfwSetMouseButtonCallback(glfwWindow_, [](GLFWwindow* w, int button, int action, int mods) {
                static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w))->onMouseButton(button, action, mods);
            });

            glfwSetCursorPosCallback(glfwWindow_, [](GLFWwindow* w, double x, double y) {
                static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w))->onCursorPos(x, y);
            });

            glfwSetScrollCallback(glfwWindow_, [](GLFWwindow* w, double /*xoffset*/, double yoffset) {
                auto* self = static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w));
                int lines = static_cast<int>(yoffset);
                if (lines > 0) {
                    self->dispatchAction(Action::ScrollUp{lines});
                } else if (lines < 0) {
                    self->dispatchAction(Action::ScrollDown{-lines});
                }
            });

            glfwSetWindowFocusCallback(glfwWindow_, [](GLFWwindow* w, int focused) {
                auto* self = static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w));
                // Window focus goes to the main pane terminal, not popup
                Tab* tab = self->activeTab();
                if (tab && !tab->hasOverlay()) {
                    Pane* fp = tab->layout()->focusedPane();
                    if (fp && fp->terminal()) fp->terminal()->focusEvent(focused != 0);
                } else if (tab && tab->hasOverlay()) {
                    tab->topOverlay()->focusEvent(focused != 0);
                }
            });

            // Observe system appearance changes for mode 2031
            platformObserveAppearanceChanges([this](bool isDark) {
                notifyAllTerminals([isDark](TerminalEmulator* term) {
                    term->notifyColorPreference(isDark);
                });
            });
        }
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
    pcbs.onTerminalExited = [this](Terminal* t) { terminalExited(t); };
    pcbs.quit = [this]() { quit(); };
    auto terminal = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));

    terminal->applyColorScheme(options.colors);
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

    if (loop_) {
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

    // Terminal is owned by the Pane/Tab/Layout — terminal is owned by the Pane/Tab/Layout.
    return;
}

void PlatformDawn::configureSurface(uint32_t width, uint32_t height)
{
    wgpu::SurfaceConfiguration config = {};
    config.device = device_;
    config.format = wgpu::TextureFormat::BGRA8Unorm;
    config.width = width;
    config.height = height;
    config.presentMode = wgpu::PresentMode::Fifo;
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    surface_.Configure(&config);
}

std::unique_ptr<PlatformDawn> createPlatform(int argc, char** argv, bool headless)
{
    return std::make_unique<PlatformDawn>(argc, argv, headless);
}
