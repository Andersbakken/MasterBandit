#include "Platform.h"
#include "Terminal.h"
#include "Renderer.h"
#include "TexturePool.h"
#include "text.h"
#include "Log.h"
#include "DebugIPC.h"
#include "NativeSurface.h"
#include "Utils.h"
#include "FontFallback.h"
#include "FontResolver.h"
#include "Layout.h"
#include "Pane.h"
#include "Tab.h"
#include "Action.h"
#include "Bindings.h"

#include <glaze/glaze.hpp>

#include <dawn/webgpu_cpp.h>
#include <dawn/native/DawnNative.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <uv.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <sys/ioctl.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;


// --- Custom spdlog sink that forwards to DebugIPC ---

class DebugIPCSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    void setIPC(DebugIPC* ipc) { ipc_ = ipc; }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        if (!ipc_) return;
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        ipc_->broadcastLog(std::string(formatted.data(), formatted.size()));
    }

    void flush_() override {}

private:
    DebugIPC* ipc_ = nullptr;
};

// --- GLFW modifier conversion ---

static unsigned int glfwModsToModifiers(int mods)
{
    unsigned int m = 0;
    if (mods & GLFW_MOD_SHIFT) m |= ShiftModifier;
    if (mods & GLFW_MOD_CONTROL) m |= CtrlModifier;
    if (mods & GLFW_MOD_ALT) m |= AltModifier;
    if (mods & GLFW_MOD_SUPER) m |= MetaModifier;
    return m;
}

// --- GLFW key → Platform Key mapping ---

static Key glfwKeyToKey(int glfwKey)
{
    switch (glfwKey) {
    case GLFW_KEY_ESCAPE:       return Key_Escape;
    case GLFW_KEY_TAB:          return Key_Tab;
    case GLFW_KEY_BACKSPACE:    return Key_Backspace;
    case GLFW_KEY_ENTER:        return Key_Return;
    case GLFW_KEY_KP_ENTER:     return Key_Enter;
    case GLFW_KEY_INSERT:       return Key_Insert;
    case GLFW_KEY_DELETE:        return Key_Delete;
    case GLFW_KEY_PAUSE:        return Key_Pause;
    case GLFW_KEY_PRINT_SCREEN: return Key_Print;
    case GLFW_KEY_HOME:         return Key_Home;
    case GLFW_KEY_END:          return Key_End;
    case GLFW_KEY_LEFT:         return Key_Left;
    case GLFW_KEY_UP:           return Key_Up;
    case GLFW_KEY_RIGHT:        return Key_Right;
    case GLFW_KEY_DOWN:         return Key_Down;
    case GLFW_KEY_PAGE_UP:      return Key_PageUp;
    case GLFW_KEY_PAGE_DOWN:    return Key_PageDown;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT:  return Key_Shift;
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL: return Key_Control;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT:    return Key_Alt;
    case GLFW_KEY_LEFT_SUPER:   return Key_Super_L;
    case GLFW_KEY_RIGHT_SUPER:  return Key_Super_R;
    case GLFW_KEY_CAPS_LOCK:    return Key_CapsLock;
    case GLFW_KEY_NUM_LOCK:     return Key_NumLock;
    case GLFW_KEY_SCROLL_LOCK:  return Key_ScrollLock;
    case GLFW_KEY_F1:           return Key_F1;
    case GLFW_KEY_F2:           return Key_F2;
    case GLFW_KEY_F3:           return Key_F3;
    case GLFW_KEY_F4:           return Key_F4;
    case GLFW_KEY_F5:           return Key_F5;
    case GLFW_KEY_F6:           return Key_F6;
    case GLFW_KEY_F7:           return Key_F7;
    case GLFW_KEY_F8:           return Key_F8;
    case GLFW_KEY_F9:           return Key_F9;
    case GLFW_KEY_F10:          return Key_F10;
    case GLFW_KEY_F11:          return Key_F11;
    case GLFW_KEY_F12:          return Key_F12;
    case GLFW_KEY_SPACE:        return Key_Space;
    case GLFW_KEY_MENU:         return Key_Menu;
    default:
        if (glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z)
            return static_cast<Key>(Key_A + (glfwKey - GLFW_KEY_A));
        if (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9)
            return static_cast<Key>(Key_0 + (glfwKey - GLFW_KEY_0));
        return Key_unknown;
    }
}

// --- UTF-32 codepoint to UTF-8 string ---
static std::string codepointToUtf8(uint32_t cp)
{
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

// --- Load TTF file into memory ---
// Legacy parseHexColor kept for tab bar colors (BGRA format)
// TODO: migrate tab bar colors to color::parseHexBGRA and remove this
static uint32_t parseHexColor(const std::string& hex, uint32_t def = 0xFF000000) {
    return color::parseHexBGRA(hex, def);
}

// Implemented in platform-specific files (PlatformUtils_macOS.mm / PlatformUtils_Linux.cpp)
bool platformIsDarkMode();
void platformObserveAppearanceChanges(std::function<void(bool isDark)> callback);
void platformSendNotification(const std::string& title, const std::string& body);
void platformOpenURL(const std::string& url);

static std::vector<uint8_t> loadFontFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

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

// Replace invalid UTF-8 bytes with U+FFFD so glaze doesn't reject them
static std::string sanitizeUtf8(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        if (*p < 0x80) {
            out += static_cast<char>(*p++);
        } else if ((*p & 0xE0) == 0xC0 && p + 1 < end && (p[1] & 0xC0) == 0x80) {
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
        } else if ((*p & 0xF0) == 0xE0 && p + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
        } else if ((*p & 0xF8) == 0xF0 && p + 3 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
        } else {
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++p;
        }
    }
    return out;
}

// Parse "#RRGGBB" hex color → packed BGRA8 (0xAARRGGBB in little-endian memory = BGRA GPU layout)
// Dawn BGRA8Unorm: in memory bytes are B,G,R,A. As a uint32 read LE: (A<<24)|(R<<16)|(G<<8)|B

// ========================================================================
// PlatformDawn class
// ========================================================================

class PlatformDawn : public Platform {
public:
    PlatformDawn(int argc, char** argv);
    ~PlatformDawn();

    int exec() override;
    void quit(int status = 0) override;
    std::unique_ptr<Terminal> createTerminal(const TerminalOptions& options) override;
    void createTab();
    void closeTab(int idx);

    wgpu::Device device() const { return device_; }
    wgpu::Queue queue() const { return queue_; }
    TexturePool& texturePool() { return texturePool_; }

    const std::string& exeDir() const { return exeDir_; }

    // Called from GLFW callbacks
    void onKey(int key, int scancode, int action, int mods);
    void onChar(unsigned int codepoint);
    void onFramebufferResize(int width, int height);
    void adjustFontSize(float delta);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);

    void renderFrame();
    bool shouldClose() { return glfwWindow_ && glfwWindowShouldClose(glfwWindow_); }

    std::string gridToJson(int id);
    std::string statsJson(int id);

private:
    void configureSurface(uint32_t width, uint32_t height);
    void resolveRow(int paneId, int row, FontData* font, float scale);

    void addPtyPoll(int fd, Terminal* term);
    void removePtyPoll(int fd);

    // Dawn core
    wgpu::Device device_;
    wgpu::Queue queue_;
    std::unique_ptr<dawn::native::Instance> nativeInstance_;
    TexturePool texturePool_;
    int exitStatus_ = 0;
    bool running_ = false;
    uv_loop_t* loop_ = nullptr;
    uv_idle_t idleCb_ = {};
    std::string exeDir_;
    std::unique_ptr<DebugIPC> debugIPC_;
    std::shared_ptr<DebugIPCSink> debugSink_;

    // Tabs
    std::vector<std::unique_ptr<Tab>> tabs_;
    int activeTabIdx_ = 0;
    int lastFocusedPaneId_ = -1;

    void updateTabBarVisibility() {
        if (tabBarConfig_.style != "auto") return;
        int h = tabBarVisible() ? static_cast<int>(std::ceil(tabBarLineHeight_)) : 0;
        for (auto& tab : tabs_) {
            tab->layout()->setTabBar(h, tabBarConfig_.position);
            tab->layout()->computeRects(fbWidth_, fbHeight_);
            for (auto& p : tab->layout()->panes())
                p->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);
        }
    }

    bool tabBarVisible() const {
        if (tabBarConfig_.style == "hidden") return false;
        if (tabBarConfig_.style == "auto") return tabs_.size() > 1;
        return true; // "visible" or legacy "powerline"
    }

    Tab* activeTab() {
        if (tabs_.empty() || activeTabIdx_ < 0 || activeTabIdx_ >= static_cast<int>(tabs_.size()))
            return nullptr;
        return tabs_[activeTabIdx_].get();
    }

    void notifyAllTerminals(const std::function<void(TerminalEmulator*)>& fn) {
        for (auto& tab : tabs_) {
            for (auto& [paneId, _] : paneRenderStates_) {
                Pane* pane = tab->layout()->pane(paneId);
                if (pane) {
                    if (auto* term = pane->activeTerm()) fn(term);
                }
            }
        }
    }

    // Active terminal for input routing
    Terminal* activeTerm() {
        Tab* tab = activeTab();
        if (!tab) return nullptr;
        if (tab->hasOverlay()) return tab->topOverlay();
        Pane* pane = tab->layout()->focusedPane();
        return pane ? static_cast<Terminal*>(pane->activeTerm()) : nullptr;
    }

    // Shared rendering state (moved from TerminalWindow)
    GLFWwindow* glfwWindow_ = nullptr;
    wgpu::Surface surface_;
    Renderer renderer_;
    TextSystem textSystem_;
    std::string fontName_ = "mono";
    float fontSize_ = 16.0f;
    float baseFontSize_ = 0.0f; // set from fontSize_ after scaling
    float charWidth_ = 0.0f;
    float lineHeight_ = 0.0f;
    float contentScaleX_ = 1.0f, contentScaleY_ = 1.0f;
    uint32_t fbWidth_ = 0, fbHeight_ = 0;
    std::string primaryFontPath_;
    FontFallback fontFallback_;

    // Default colors (packed as R | G<<8 | B<<16 | 0xFF<<24, matching packFgAsU32)
    uint32_t defaultFgColor_ = 0xFFDDDDDD; // #dddddd
    uint32_t defaultBgColor_ = 0x00000000; // transparent (uses clear color)
    bool needsRedraw_ = true;
    bool controlPressed_ = false;
    unsigned int lastMods_ = 0;

    // Per-pane render state
    struct PaneRenderState {
        std::vector<ResolvedCell> resolvedCells;
        std::vector<GlyphEntry> glyphBuffer;      // all glyphs for all cells
        uint32_t totalGlyphs = 0;
        int lastCursorX = -1, lastCursorY = -1;
        bool lastCursorVisible = true;
        PooledTexture* heldTexture = nullptr;
        bool dirty = true;
        std::vector<PooledTexture*> pendingRelease;
        wgpu::Buffer dividerVB; // null = no divider for this pane
        int lastViewportOffset = 0;
        int lastHistorySize = 0;

        // Per-row shaping cache: avoids re-shaping unchanged rows
        struct RowGlyphCache {
            std::vector<GlyphEntry> glyphs;
            // Per-cell: (offset into glyphs, count)
            std::vector<std::pair<uint32_t, uint32_t>> cellGlyphRanges;
            bool valid = false;
        };
        std::vector<RowGlyphCache> rowShapingCache;
    };
    std::unordered_map<int, PaneRenderState> paneRenderStates_;

    // Multiple PTY poll handles — keyed by master fd
    std::unordered_map<int, uv_poll_t*> ptyPolls_;

    TerminalOptions terminalOptions_;  // stored from first createTerminal call

    // Tab bar
    TabBarConfig  tabBarConfig_;
    std::string   tabBarFontName_   = "tab_bar";
    float         tabBarFontSize_   = 0.0f;
    float         tabBarCharWidth_  = 0.0f;
    float         tabBarLineHeight_ = 0.0f;
    int           tabBarCols_       = 0;
    PooledTexture* tabBarTexture_   = nullptr;
    bool          tabBarDirty_      = true;
    int           tabBarAnimFrame_  = 0;
    uint64_t      lastAnimTick_     = 0;
    std::vector<PooledTexture*> pendingTabBarRelease_;
    std::vector<ComputeState*> pendingComputeRelease_;

    // Tab bar colors (packed BGRA8 as used in ResolvedCell)
    // Pane divider
    float dividerR_ = 0.24f, dividerG_ = 0.24f, dividerB_ = 0.24f, dividerA_ = 1.0f;
    int   dividerWidth_ = 1;

    // Pane tints (RGBA, applied as uniform multiplier in shaders)
    float activeTint_[4]   = {1.0f, 1.0f, 1.0f, 1.0f};
    float inactiveTint_[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    uint32_t tbBgColor_        = 0xFF261926;
    uint32_t tbActiveBgColor_  = 0xFFf7a27a;
    uint32_t tbActiveFgColor_  = 0xFF261a1b;
    uint32_t tbInactiveBgColor_= 0xFF3b2824;
    uint32_t tbInactiveFgColor_= 0xFF895f56;
    float progressColorR_ = 0.0f, progressColorG_ = 0.6f, progressColorB_ = 1.0f;
    float progressBarHeight_ = 3.0f;

    // Scaled padding in pixels
    float padLeft_ = 0, padTop_ = 0, padRight_ = 0, padBottom_ = 0;

    void initTabBar(const TabBarConfig& cfg);
    void renderTabBar();

    // Keybindings
    std::vector<Binding> bindings_;
    SequenceMatcher      sequenceMatcher_;
    void dispatchAction(const Action::Any& action);
    void terminalExited(Terminal* terminal) override;

    // Pane helpers
    void spawnTerminalForPane(Pane* pane, int tabIdx);
    void resizeAllPanesInTab(Tab* tab);
    void refreshDividers(Tab* tab);
    void clearDividers(Tab* tab);
    void releaseTabTextures(Tab* tab);
    void updateTabTitleFromFocusedPane(int tabIdx);
    // Send focus-out to prevId's terminal and focus-in to newId's terminal.
    void notifyPaneFocusChange(Tab* tab, int prevId, int newId);
};

// ========================================================================
// PlatformDawn implementation
// ========================================================================

PlatformDawn::PlatformDawn(int argc, char** argv)
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

    if (!glfwInit()) {
        spdlog::error("glfwInit failed");
        return;
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
    glfwTerminate();
}

std::unique_ptr<Terminal> PlatformDawn::createTerminal(const TerminalOptions& options)
{
    if (!device_) return nullptr;

    // Create the GLFW window only once
    if (!glfwWindow_) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindow_ = glfwCreateWindow(800, 600, "MasterBandit", nullptr, nullptr);
        if (!glfwWindow_) {
            spdlog::error("glfwCreateWindow failed");
            return nullptr;
        }

        wgpu::Instance instance(nativeInstance_->Get());
        surface_ = createNativeSurface(glfwWindow_, instance);
        if (!surface_) {
            spdlog::error("Failed to create Dawn surface");
            glfwDestroyWindow(glfwWindow_);
            glfwWindow_ = nullptr;
            return nullptr;
        }

        // Get framebuffer size and content scale
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

        // Load font
        std::string fontPath;
        if (options.font.empty()) {
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
            return nullptr;
        }
        spdlog::info("Using font: {}", fontPath);
        primaryFontPath_ = fontPath;

        auto fontData = loadFontFile(fontPath);
        if (fontData.empty()) {
            spdlog::error("Failed to load font: {}", fontPath);
            return nullptr;
        }

        std::vector<std::vector<uint8_t>> fontList = {std::move(fontData)};

        // Load bold variant
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

        // Load bundled Symbols Nerd Font Mono as a built-in fallback for
        // powerline glyphs (U+E0B0-E0B3) and other symbols.
        auto nerdFontPath = fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf";
        auto nerdFontData = loadFontFile(nerdFontPath.string());
        if (!nerdFontData.empty()) {
            textSystem_.addFallbackFont(fontName_, nerdFontData);
            spdlog::info("Loaded built-in Symbols Nerd Font Mono");
        } else {
            spdlog::warn("Built-in Symbols Nerd Font Mono not found at {}", nerdFontPath.string());
        }

        const FontData* font = textSystem_.getFont(fontName_);
        if (!font) {
            spdlog::error("Failed to register font");
            return nullptr;
        }

        float scale = fontSize_ / font->baseSize;
        lineHeight_ = font->lineHeight * scale;

        const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
        charWidth_ = shaped.width;
        if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

        spdlog::info("Font metrics: charWidth={:.1f}, lineHeight={:.1f}", charWidth_, lineHeight_);

        std::string shaderDir = fs::weakly_canonical(
            fs::path(exeDir_) / "shaders").string();
        if (!fs::exists(shaderDir)) {
            shaderDir = (fs::path(__FILE__).parent_path().parent_path() / "shaders").string();
        }
        renderer_.init(device_, queue_, shaderDir, fbWidth_, fbHeight_);
        renderer_.initProgressPipeline(device_, shaderDir);
        renderer_.uploadFontAtlas(queue_, fontName_, *font);

        // Wire up GLFW callbacks
        glfwSetWindowUserPointer(glfwWindow_, this);

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
            if (lines != 0) {
                Terminal* t = self->activeTerm();
                if (t) t->scrollViewport(lines);
            }
        });

        glfwSetWindowFocusCallback(glfwWindow_, [](GLFWwindow* w, int focused) {
            auto* self = static_cast<PlatformDawn*>(glfwGetWindowUserPointer(w));
            Terminal* t = self->activeTerm();
            if (t) t->focusEvent(focused != 0);
        });

        // Observe system appearance changes for mode 2031
        platformObserveAppearanceChanges([this](bool isDark) {
            notifyAllTerminals([isDark](TerminalEmulator* term) {
                term->notifyColorPreference(isDark);
            });
        });
    }

    // Create a layout and tab for this terminal
    auto layout = std::make_unique<Layout>();
    layout->setDividerPixels(dividerWidth_);
    Pane* pane = layout->createPane();
    layout->setFocusedPane(pane->id());
    layout->computeRects(fbWidth_, fbHeight_);

    int paneId = pane->id();

    // Build TerminalCallbacks capturing paneId
    TerminalCallbacks cbs;
    cbs.event = [this, paneId](TerminalEmulator*, int ev, void*) {
        switch (static_cast<TerminalEmulator::Event>(ev)) {
        case TerminalEmulator::Update:
        case TerminalEmulator::ScrollbackChanged:
            needsRedraw_ = true;
            {
                auto it = paneRenderStates_.find(paneId);
                if (it != paneRenderStates_.end()) it->second.dirty = true;
            }
            break;
        case TerminalEmulator::VisibleBell:
            break;
        }
    };
    cbs.copyToClipboard = [this](const std::string& text) {
        glfwSetClipboardString(glfwWindow_, text.c_str());
    };
    cbs.pasteFromClipboard = [this]() -> std::string {
        const char* clip = glfwGetClipboardString(glfwWindow_);
        return clip ? std::string(clip) : std::string();
    };
    cbs.onTitleChanged = [this, paneId, tabIdx = static_cast<int>(tabs_.size())](const std::string& title) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setTitle(title);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setTitle(title);
            if (tabIdx == activeTabIdx_) glfwSetWindowTitle(glfwWindow_, title.c_str());
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onIconChanged = [this, paneId, tabIdx = static_cast<int>(tabs_.size())](const std::string& icon) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setIcon(icon);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setIcon(icon);
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setProgress(state, pct);
                tabBarDirty_ = true;
                needsRedraw_ = true;
                break;
            }
        }
    };
    cbs.cellPixelWidth = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    cbs.isDarkMode = []() { return platformIsDarkMode(); };
    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setCWD(dir);
                break;
            }
        }
    };
    cbs.onDesktopNotification = [](const std::string& title, const std::string& body, const std::string&) {
        platformSendNotification(title, body);
    };

    auto terminal = std::make_unique<Terminal>(this, std::move(cbs));

    terminal->applyColorScheme(options.colors);
    if (!terminal->init(options)) {
        spdlog::error("Failed to init terminal");
        return nullptr;
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

    // Set pty window size
    {
        struct winsize ws = {};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ws.ws_xpixel = static_cast<unsigned short>(fbWidth_);
        ws.ws_ypixel = static_cast<unsigned short>(fbHeight_);
        ioctl(terminal->masterFD(), TIOCSWINSZ, &ws);
    }

    Terminal* termPtr = terminal.get();
    int masterFD = terminal->masterFD();

    // Set initial title from shell name
    {
        std::string shellName = options.shell;
        auto slash = shellName.rfind('/');
        if (slash != std::string::npos) shellName = shellName.substr(slash + 1);
        pane->setTitle(shellName);
    }

    pane->setTerminal(std::move(terminal));  // Pane owns the Terminal now

    if (loop_) {
        addPtyPoll(masterFD, termPtr);
    }

    // Build and store tab
    auto tab = std::make_unique<Tab>(std::move(layout));
    tab->setTitle(pane->title());
    activeTabIdx_ = static_cast<int>(tabs_.size());
    tabs_.push_back(std::move(tab));
    glfwSetWindowTitle(glfwWindow_, pane->title().c_str());

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
                TerminalEmulator* te = p->activeTerm();
                if (te) {
                    int c = te->width(), r = te->height();
                    auto& rs2 = paneRenderStates_[p->id()];
                    rs2.resolvedCells.resize(static_cast<size_t>(c > 0 ? c : 1) * (r > 0 ? r : 1));
                }
            }
        }
    }

    // Terminal is owned by the Pane/Tab/Layout — return nullptr, not a second owner.
    return nullptr;
}

void PlatformDawn::createTab()
{
    if (!device_ || !glfwWindow_) return;

    auto layout = std::make_unique<Layout>();
    layout->setDividerPixels(dividerWidth_);
    Pane* pane = layout->createPane();
    layout->setFocusedPane(pane->id());

    // Apply tab bar height to the new layout
    if (tabBarVisible() && tabBarLineHeight_ > 0.0f) {
        layout->setTabBar(static_cast<int>(std::ceil(tabBarLineHeight_)), tabBarConfig_.position);
    }
    layout->computeRects(fbWidth_, fbHeight_);

    int paneId = pane->id();
    int tabIdx = static_cast<int>(tabs_.size());

    TerminalCallbacks cbs;
    cbs.event = [this, paneId](TerminalEmulator*, int ev, void*) {
        if (ev == TerminalEmulator::Update || ev == TerminalEmulator::ScrollbackChanged) {
            needsRedraw_ = true;
            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) it->second.dirty = true;
        }
    };
    cbs.copyToClipboard = [this](const std::string& text) {
        glfwSetClipboardString(glfwWindow_, text.c_str());
    };
    cbs.pasteFromClipboard = [this]() -> std::string {
        const char* clip = glfwGetClipboardString(glfwWindow_);
        return clip ? std::string(clip) : std::string();
    };
    cbs.onTitleChanged = [this, paneId, tabIdx](const std::string& title) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setTitle(title);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setTitle(title);
            if (tabIdx == activeTabIdx_) glfwSetWindowTitle(glfwWindow_, title.c_str());
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onIconChanged = [this, paneId, tabIdx](const std::string& icon) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setIcon(icon);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setIcon(icon);
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setProgress(state, pct);
                tabBarDirty_ = true;
                needsRedraw_ = true;
                break;
            }
        }
    };
    cbs.cellPixelWidth  = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    cbs.isDarkMode = []() { return platformIsDarkMode(); };
    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setCWD(dir);
                break;
            }
        }
    };
    cbs.onDesktopNotification = [](const std::string& title, const std::string& body, const std::string&) {
        platformSendNotification(title, body);
    };

    auto terminal = std::make_unique<Terminal>(this, std::move(cbs));
    terminal->applyColorScheme(terminalOptions_.colors);
    if (!terminal->init(terminalOptions_)) {
        spdlog::error("createTab: failed to init terminal");
        return;
    }

    const PaneRect& pr = pane->rect();
    int cols = (pr.w > 0 && charWidth_ > 0) ? static_cast<int>((pr.w - padLeft_ - padRight_) / charWidth_) : 80;
    int rows = (pr.h > 0 && lineHeight_ > 0) ? static_cast<int>((pr.h - padTop_ - padBottom_) / lineHeight_) : 24;

    auto& rs = paneRenderStates_[paneId];
    rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
    rs.dirty = true;

    terminal->resize(cols, rows);
    {
        struct winsize ws = {};
        ws.ws_col    = static_cast<unsigned short>(cols);
        ws.ws_row    = static_cast<unsigned short>(rows);
        ws.ws_xpixel = static_cast<unsigned short>(fbWidth_);
        ws.ws_ypixel = static_cast<unsigned short>(fbHeight_);
        ioctl(terminal->masterFD(), TIOCSWINSZ, &ws);
    }

    // Set initial title from shell name
    {
        std::string shellName = terminalOptions_.shell;
        auto slash = shellName.rfind('/');
        if (slash != std::string::npos) shellName = shellName.substr(slash + 1);
        pane->setTitle(shellName);
    }

    Terminal* termPtr = terminal.get();
    int masterFD = terminal->masterFD();
    pane->setTerminal(std::move(terminal));
    addPtyPoll(masterFD, termPtr);

    activeTabIdx_ = tabIdx;
    auto tab = std::make_unique<Tab>(std::move(layout));
    tab->setTitle(pane->title());
    tabs_.push_back(std::move(tab));

    updateTabBarVisibility();
    tabBarDirty_ = true;
    needsRedraw_ = true;

    spdlog::info("Created tab {}", tabIdx + 1);
}

void PlatformDawn::closeTab(int idx)
{
    if (tabs_.empty() || idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
    if (tabs_.size() == 1) return; // can't close the last tab

    // Stop PTY polls for all terminals in this tab
    Tab* tab = tabs_[idx].get();
    for (auto& panePtr : tab->layout()->panes()) {
        TerminalEmulator* term = panePtr->activeTerm();
        if (auto* t = dynamic_cast<Terminal*>(term)) {
            removePtyPoll(t->masterFD());
        }
        // Release pane render state
        auto it = paneRenderStates_.find(panePtr->id());
        if (it != paneRenderStates_.end()) {
            if (it->second.heldTexture)
                pendingTabBarRelease_.push_back(it->second.heldTexture);
            for (auto* t2 : it->second.pendingRelease)
                pendingTabBarRelease_.push_back(t2);
            paneRenderStates_.erase(it);
        }
    }

    tabs_.erase(tabs_.begin() + idx);

    // Adjust active tab index
    if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
        activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;

    updateTabBarVisibility();
    tabBarDirty_ = true;
    needsRedraw_ = true;
    spdlog::info("Closed tab {}", idx + 1);
}

void PlatformDawn::addPtyPoll(int fd, Terminal* term)
{
    auto* poll = new uv_poll_t{};
    uv_poll_init(loop_, poll, fd);
    poll->data = term;
    uv_poll_start(poll, UV_READABLE, [](uv_poll_t* handle, int status, int events) {
        if (status < 0) return;
        if (events & UV_READABLE)
            static_cast<Terminal*>(handle->data)->readFromFD();
    });
    ptyPolls_[fd] = poll;
}

void PlatformDawn::removePtyPoll(int fd)
{
    auto it = ptyPolls_.find(fd);
    if (it == ptyPolls_.end()) return;
    uv_poll_stop(it->second);
    uv_close(reinterpret_cast<uv_handle_t*>(it->second), [](uv_handle_t* h) {
        delete reinterpret_cast<uv_poll_t*>(h);
    });
    ptyPolls_.erase(it);
}

int PlatformDawn::exec()
{
    Tab* tab = activeTab();
    if (!tab || tab->layout()->panes().empty()) return 1;

    running_ = true;
    loop_ = uv_default_loop();

    debugIPC_ = std::make_unique<DebugIPC>(loop_, nullptr,
        [this](int id) {
            Tab* t = activeTab();
            if (!t) return std::string{};
            Pane* pane = t->layout()->focusedPane();
            return pane ? gridToJson(pane->id()) : std::string{};
        },
        [this](int id) { return statsJson(id); });
    if (debugSink_) {
        debugSink_->setIPC(debugIPC_.get());
    }

    // Add PTY polls for all terminals already created
    for (auto& panePtr : tab->layout()->panes()) {
        Terminal* term = panePtr->terminal();
        if (term && term->masterFD() >= 0) {
            addPtyPoll(term->masterFD(), term);
        }
    }

    uv_idle_init(loop_, &idleCb_);
    idleCb_.data = this;
    uv_idle_start(&idleCb_, [](uv_idle_t* handle) {
        auto* self = static_cast<PlatformDawn*>(handle->data);
        glfwPollEvents();

        if (self->shouldClose()) {
            uv_stop(self->loop_);
            return;
        }

        // Advance progress animations
        {
            bool hasAnim = false;
            for (auto& tab : self->tabs_) {
                for (auto& panePtr : tab->layout()->panes()) {
                    if (panePtr->progressState() == 3) { hasAnim = true; break; }
                }
                if (hasAnim) break;
            }
            if (hasAnim) {
                // Indeterminate: redraw every frame for smooth animation
                self->needsRedraw_ = true;
                // Tab bar glyph animation at 10fps
                auto now = TerminalEmulator::mono();
                if (self->tabBarVisible() && now - self->lastAnimTick_ > 100) {
                    self->lastAnimTick_ = now;
                    self->tabBarAnimFrame_++;
                    self->tabBarDirty_ = true;
                }
            }
        }

        self->device_.Tick();
        self->renderFrame();
    });

    uv_run(loop_, UV_RUN_DEFAULT);

    if (debugSink_) {
        debugSink_->setIPC(nullptr);
    }

    // Stop all PTY polls
    std::vector<int> fds;
    for (auto& [fd, _] : ptyPolls_) fds.push_back(fd);
    for (int fd : fds) removePtyPoll(fd);

    uv_idle_stop(&idleCb_);
    uv_close(reinterpret_cast<uv_handle_t*>(&idleCb_), nullptr);
    if (debugIPC_) debugIPC_->closeHandles();
    uv_run(loop_, UV_RUN_DEFAULT);
    debugIPC_.reset();

    return exitStatus_;
}

void PlatformDawn::quit(int status)
{
    exitStatus_ = status;
    if (loop_) {
        uv_stop(loop_);
    }
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

std::string PlatformDawn::gridToJson(int id)
{
    // Search for the pane across all tabs
    Pane* pane = nullptr;
    for (auto& tabPtr : tabs_) {
        pane = tabPtr->layout()->pane(id);
        if (pane) break;
    }
    if (!pane) return {};
    TerminalEmulator* term = pane->activeTerm();
    if (!term) return {};

    const IGrid& g = term->grid();

    glz::generic::object_t resp;
    resp["type"] = "screenshot";
    resp["format"] = "grid";
    if (id) resp["id"] = static_cast<double>(id);
    resp["cols"] = static_cast<double>(term->width());
    resp["rows"] = static_cast<double>(term->height());
    resp["cursor"] = glz::generic::object_t{
        {"x", static_cast<double>(term->cursorX())},
        {"y", static_cast<double>(term->cursorY())}
    };

    glz::generic::array_t lines;
    for (int row = 0; row < g.rows(); ++row) {
        std::string text;
        for (int col = 0; col < g.cols(); ++col) {
            const Cell& c = g.cell(col, row);
            if (c.wc == 0) {
                text += ' ';
            } else {
                text += codepointToUtf8(c.wc);
            }
        }
        // Trim trailing spaces
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (!text.empty()) {
            glz::generic::object_t line;
            line["y"] = static_cast<double>(row);
            line["text"] = sanitizeUtf8(text);
            lines.emplace_back(std::move(line));
        }
    }
    resp["lines"] = std::move(lines);

    std::string buf;
    (void)glz::write_json(resp, buf);
    return buf;
}

std::string PlatformDawn::statsJson(int id)
{
    auto texStats     = texturePool_.stats();
    auto computeStats = renderer_.computePool().stats();

    glz::generic::object_t resp;
    resp["type"] = "stats";
    if (id) resp["id"] = static_cast<double>(id);

    auto toKB = [](size_t b) { return static_cast<double>(b) / 1024.0; };

    resp["texture_pool"] = glz::generic::object_t{
        {"total",       static_cast<double>(texStats.total)},
        {"in_use",      static_cast<double>(texStats.inUse)},
        {"free",        static_cast<double>(texStats.free)},
        {"total_kb",    toKB(texStats.totalBytes)},
        {"free_kb",     toKB(texStats.freeBytes)},
        {"limit_kb",    toKB(texStats.limitBytes)},
    };
    resp["compute_pool"] = glz::generic::object_t{
        {"total",       static_cast<double>(computeStats.total)},
        {"in_use",      static_cast<double>(computeStats.inUse)},
        {"free",        static_cast<double>(computeStats.free)},
        {"total_kb",    toKB(computeStats.totalBytes)},
        {"free_kb",     toKB(computeStats.freeBytes)},
        {"limit_kb",    toKB(computeStats.limitBytes)},
    };

    glz::generic::array_t tabsArr;
    for (int ti = 0; ti < static_cast<int>(tabs_.size()); ++ti) {
        Tab* tab = tabs_[ti].get();
        glz::generic::array_t panesArr;
        for (auto& panePtr : tab->layout()->panes()) {
            int pid = panePtr->id();
            auto it = paneRenderStates_.find(pid);
            TerminalEmulator* term = panePtr->activeTerm();
            glz::generic::object_t paneObj;
            paneObj["id"]   = static_cast<double>(pid);
            paneObj["cols"] = static_cast<double>(term ? term->width()  : 0);
            paneObj["rows"] = static_cast<double>(term ? term->height() : 0);
            if (it != paneRenderStates_.end()) {
                const auto& rs = it->second;
                paneObj["held_texture"] = rs.heldTexture != nullptr;
                paneObj["texture_kb"]   = rs.heldTexture
                    ? toKB(rs.heldTexture->sizeBytes) : 0.0;
                paneObj["has_divider"]  = rs.dividerVB != nullptr;
            }
            panesArr.emplace_back(std::move(paneObj));
        }
        glz::generic::object_t tabObj;
        tabObj["index"]  = static_cast<double>(ti);
        tabObj["active"] = (ti == activeTabIdx_);
        tabObj["panes"]  = std::move(panesArr);
        tabsArr.emplace_back(std::move(tabObj));
    }
    resp["tabs"] = std::move(tabsArr);

    std::string buf;
    (void)glz::write_json(resp, buf);
    return buf;
}

void PlatformDawn::resolveRow(int paneId, int row, FontData* font, float scale)
{
    auto paneIt = paneRenderStates_.find(paneId);
    if (paneIt == paneRenderStates_.end()) return;
    PaneRenderState& rs = paneIt->second;

    Pane* pane = nullptr;
    for (auto& tabPtr : tabs_) {
        pane = tabPtr->layout()->pane(paneId);
        if (pane) break;
    }
    if (!pane) return;
    TerminalEmulator* term = pane->activeTerm();
    if (!term) return;

    int cols = term->width();
    int baseIdx = row * cols;
    const Cell* rowData = term->viewportRow(row);

    // Ensure row shaping cache is sized
    if (static_cast<int>(rs.rowShapingCache.size()) != term->height())
        rs.rowShapingCache.resize(term->height());

    auto& rowCache = rs.rowShapingCache[row];
    rowCache.glyphs.clear();
    rowCache.cellGlyphRanges.assign(cols, {0, 0});

    // Pass 1: Resolve per-cell decorations (fg, bg, underline)
    for (int col = 0; col < cols; ++col) {
        ResolvedCell& rc = rs.resolvedCells[baseIdx + col];
        const Cell& cell = rowData[col];

        uint32_t fg = (cell.attrs.fgMode() == CellAttrs::Default) ? defaultFgColor_ : cell.attrs.packFgAsU32();
        uint32_t bg = (cell.attrs.bgMode() == CellAttrs::Default) ? defaultBgColor_ : cell.attrs.packBgAsU32();
        if (cell.attrs.inverse()) std::swap(fg, bg);

        uint32_t ulInfo = 0;
        {
            const CellExtra* extra = term->grid().getExtra(col, row);
            bool hasUnderline = cell.attrs.underline();
            bool isHyperlink = extra && extra->hyperlinkId;
            if (!hasUnderline && isHyperlink) hasUnderline = true;
            if (hasUnderline) {
                uint8_t style = cell.attrs.underline() ? cell.attrs.underlineStyle() : 3;
                ulInfo = static_cast<uint32_t>(style + 1);
                if (extra && extra->underlineColor) {
                    ulInfo |= (extra->underlineColor & 0x00FFFFFF) << 8;
                }
            }
        }

        rc.glyph_offset = 0;
        rc.glyph_count = 0;
        rc.fg_color = fg;
        rc.bg_color = bg;
        rc.underline_info = ulInfo;
    }

    // Pass 2: Build runs and shape
    float cellWidthPx = charWidth_;
    int col = 0;
    while (col < cols) {
        const Cell& cell = rowData[col];

        // Skip empty/spacer cells
        if (cell.wc == 0 || cell.attrs.wideSpacer()) {
            col++;
            continue;
        }

        // Determine run-breaking attributes
        bool runBold = cell.attrs.bold();
        bool runItalic = cell.attrs.italic();
        int runStart = col;

        // Extend run while font-affecting attributes match
        int runEnd = col + 1;
        while (runEnd < cols) {
            const Cell& next = rowData[runEnd];
            if (next.wc == 0) break;
            if (next.attrs.wideSpacer()) { runEnd++; continue; } // skip spacers, keep run going
            if (next.attrs.bold() != runBold) break;
            if (next.attrs.italic() != runItalic) break;
            runEnd++;
        }

        // Build UTF-8 string and byte-to-cell mapping
        std::string runText;
        std::vector<std::pair<uint32_t, int>> byteToCell; // (byteOffset, cellCol)
        for (int c = runStart; c < runEnd; ++c) {
            if (rowData[c].attrs.wideSpacer()) continue;
            if (rowData[c].wc == 0) continue;
            byteToCell.push_back({static_cast<uint32_t>(runText.size()), c});
            runText += codepointToUtf8(rowData[c].wc);
        }

        if (runText.empty()) {
            col = runEnd;
            continue;
        }

        // Shape the run
        int boldHint = runBold ? 1 : 0;
        const ShapedRun& shaped = textSystem_.shapeRun(fontName_, runText, fontSize_, boldHint);

        // Find contiguous RTL cell ranges for mirroring.
        // Build a map of which byteToCell indices are RTL.
        struct RtlRange { int firstCell, lastCell; }; // inclusive cell columns
        std::vector<RtlRange> rtlRanges;
        {
            int i = 0;
            int n = static_cast<int>(byteToCell.size());
            // Walk through shaped glyphs to identify which byteToCell entries are RTL
            std::vector<bool> cellIsRtl(n, false);
            for (const auto& sg : shaped.glyphs) {
                if (!sg.rtl) continue;
                for (int j = 0; j < n; j++) {
                    if (sg.cluster >= byteToCell[j].first &&
                        (j + 1 >= n || sg.cluster < byteToCell[j + 1].first)) {
                        cellIsRtl[j] = true;
                        break;
                    }
                }
            }
            // Build contiguous ranges of RTL cells
            i = 0;
            while (i < n) {
                if (cellIsRtl[i]) {
                    int start = i;
                    while (i < n && cellIsRtl[i]) i++;
                    rtlRanges.push_back({byteToCell[start].second, byteToCell[i - 1].second});
                } else {
                    i++;
                }
            }
        }

        // Map glyphs to cells via cluster index
        float penX = 0;
        for (const auto& sg : shaped.glyphs) {
            // Find which cell this glyph belongs to via cluster (byte offset)
            int cellCol = -1;
            for (auto it = byteToCell.rbegin(); it != byteToCell.rend(); ++it) {
                if (sg.cluster >= it->first) {
                    cellCol = it->second;
                    break;
                }
            }

            // For RTL glyphs, mirror cell assignment within the RTL range they belong to
            if (sg.rtl && cellCol >= 0) {
                for (const auto& range : rtlRanges) {
                    if (cellCol >= range.firstCell && cellCol <= range.lastCell) {
                        cellCol = range.firstCell + (range.lastCell - cellCol);
                        break;
                    }
                }
            }

            if (cellCol < 0 || cellCol >= cols) {
                penX += sg.xAdvance;
                continue;
            }

            // Look up glyph info in font atlas
            uint64_t glyphId = sg.glyphId;
            if ((glyphId & 0xFFFFFFFF) == 0) {
                penX += sg.xAdvance;
                continue;
            }
            auto git = font->glyphs.find(glyphId);
            if (git == font->glyphs.end() || git->second.is_empty) {
                penX += sg.xAdvance;
                continue;
            }
            const GlyphInfo& gi = git->second;

            // Glyph positioning: for substituted glyphs (ligatures, contextual forms),
            // use HarfBuzz advance-based positioning to preserve inter-glyph relationships.
            // For normal glyphs, anchor at cell origin — the terminal grid is authoritative.
            float glyphX, glyphY;
            if (sg.isSubstitution) {
                float cellLocalX = static_cast<float>(cellCol - runStart) * cellWidthPx;
                glyphX = penX + sg.xOffset - cellLocalX;
            } else {
                glyphX = sg.xOffset;
            }
            glyphY = sg.yOffset;

            // Add to row glyph cache
            GlyphEntry entry;
            entry.atlas_offset = gi.atlas_offset;
            entry.ext_min_x = gi.ext_min_x;
            entry.ext_min_y = gi.ext_min_y;
            entry.ext_max_x = gi.ext_max_x;
            entry.ext_max_y = gi.ext_max_y;
            entry.upem = gi.upem;
            entry.x_offset = glyphX;
            entry.y_offset = glyphY;

            uint32_t glyphIdx = static_cast<uint32_t>(rowCache.glyphs.size());
            rowCache.glyphs.push_back(entry);

            // Track per-cell glyph range
            auto& range = rowCache.cellGlyphRanges[cellCol];
            if (range.second == 0) {
                range.first = glyphIdx;
            }
            range.second++;

            penX += sg.xAdvance;
        }

        col = runEnd;
    }

    rowCache.valid = true;
}

void PlatformDawn::renderFrame()
{
    if (fbWidth_ == 0 || fbHeight_ == 0) return;
    Tab* currentTab = activeTab();
    if (!currentTab) return;

    wgpu::SurfaceTexture surfaceTexture;
    surface_.GetCurrentTexture(&surfaceTexture);
    if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        return;
    }

    FontData* font = const_cast<FontData*>(textSystem_.getFont(fontName_));
    if (!font) return;

    float scale = fontSize_ / font->baseSize;

    std::vector<Renderer::CompositeEntry> compositeEntries;
    bool pngNeeded = debugIPC_ && debugIPC_->pngScreenshotPending();

    // If the focused pane changed, mark both old and new pane dirty so the
    // cursor switches between solid and hollow without waiting for other content.
    int currentFocusedPaneId = currentTab->layout()->focusedPaneId();
    if (currentFocusedPaneId != lastFocusedPaneId_) {
        auto markDirty = [&](int id) {
            auto it = paneRenderStates_.find(id);
            if (it != paneRenderStates_.end()) it->second.dirty = true;
        };
        markDirty(lastFocusedPaneId_);
        markDirty(currentFocusedPaneId);
        lastFocusedPaneId_ = currentFocusedPaneId;
    }

    for (auto& panePtr : currentTab->layout()->panes()) {
        Pane* pane = panePtr.get();
        if (pane->rect().isEmpty()) continue;
        int paneId = pane->id();
        TerminalEmulator* term = pane->activeTerm();
        if (!term) continue;

        PaneRenderState& rs = paneRenderStates_[paneId];
        const IGrid& g = term->grid();

        int curX = term->cursorX(), curY = term->cursorY();
        bool cursorMoved = (curX != rs.lastCursorX || curY != rs.lastCursorY ||
                            term->cursorVisible() != rs.lastCursorVisible);
        rs.lastCursorX = curX;
        rs.lastCursorY = curY;
        rs.lastCursorVisible = term->cursorVisible();

        bool needsRender = rs.dirty || g.anyDirty() || cursorMoved || !rs.heldTexture;

        if (needsRender || pngNeeded) {
            // Ensure resolvedCells is sized correctly
            size_t needed = static_cast<size_t>(g.cols()) * g.rows();
            if (rs.resolvedCells.size() != needed)
                rs.resolvedCells.resize(needed);

            {
                int vo = term->viewportOffset();
                int histSize = term->document().historySize();
                // Detect whether the viewport content shifted since last frame:
                // viewport offset changed, or new output grew the history while scrolled.
                bool viewportShifted = (vo != rs.lastViewportOffset ||
                                        (vo != 0 && histSize != rs.lastHistorySize));
                rs.lastViewportOffset = vo;
                rs.lastHistorySize = histSize;

                if (viewportShifted) {
                    // Content shifted — must re-resolve all rows
                    for (int row = 0; row < g.rows(); ++row)
                        resolveRow(paneId, row, font, scale);
                } else {
                    // Stable viewport — only resolve dirty rows
                    for (int row = 0; row < g.rows(); ++row) {
                        if (g.isRowDirty(row) || (cursorMoved && row == curY))
                            resolveRow(paneId, row, font, scale);
                    }
                }
            }
            const_cast<IGrid&>(g).clearAllDirty();

            // Assemble glyph buffer from per-row caches and set cell glyph_offset/glyph_count
            rs.glyphBuffer.clear();
            for (int row = 0; row < g.rows(); ++row) {
                auto& rowCache = rs.rowShapingCache[row];
                if (!rowCache.valid) continue;
                uint32_t rowGlyphBase = static_cast<uint32_t>(rs.glyphBuffer.size());
                rs.glyphBuffer.insert(rs.glyphBuffer.end(),
                                      rowCache.glyphs.begin(), rowCache.glyphs.end());
                int baseIdx = row * g.cols();
                for (int col = 0; col < g.cols(); ++col) {
                    auto& range = rowCache.cellGlyphRanges[col];
                    rs.resolvedCells[baseIdx + col].glyph_offset = rowGlyphBase + range.first;
                    rs.resolvedCells[baseIdx + col].glyph_count = range.second;
                }
            }
            rs.totalGlyphs = static_cast<uint32_t>(rs.glyphBuffer.size());

            // Apply selection highlight
            bool selectionVisible = term->hasSelection();
            int histSize = term->document().historySize();
            if (selectionVisible) {
                for (int row = 0; row < g.rows(); ++row) {
                    int absRow = histSize - term->viewportOffset() + row;
                    for (int col = 0; col < g.cols(); ++col) {
                        if (term->isCellSelected(col, absRow)) {
                            int idx = row * g.cols() + col;
                            rs.resolvedCells[idx].bg_color = 0xFF664422;
                            rs.resolvedCells[idx].fg_color = 0xFFFFFFFF;
                        }
                    }
                }
            }

            // Cursor — passed as UBO params, no cell mutation needed
            bool isFocused = (paneId == currentTab->layout()->focusedPaneId());

            uint32_t totalCells = static_cast<uint32_t>(g.cols()) * g.rows();
            ComputeState* cs = renderer_.computePool().acquire(totalCells);

            // Ensure glyph buffer and vertex buffers are large enough (grow-only)
            uint32_t glyphCount = std::max(rs.totalGlyphs, 1u); // at least 1 to avoid 0-size buffer
            renderer_.computePool().ensureGlyphCapacity(cs, glyphCount);

            renderer_.uploadResolvedCells(queue_, cs, rs.resolvedCells.data(), totalCells);
            renderer_.uploadGlyphs(queue_, cs, rs.glyphBuffer.data(), rs.totalGlyphs);

            // Mark selection rows dirty for next frame
            if (selectionVisible) {
                for (int row = 0; row < g.rows(); ++row) {
                    int absRow = histSize - term->viewportOffset() + row;
                    for (int col = 0; col < g.cols(); ++col) {
                        if (term->isCellSelected(col, absRow)) {
                            const_cast<IGrid&>(g).markRowDirty(row);
                            break;
                        }
                    }
                }
            }

            renderer_.updateFontAtlas(queue_, fontName_, *font);

            // Collect image draw commands
            std::vector<Renderer::ImageDrawCmd> imageCmds;
            std::unordered_set<uint32_t> seenImages;
            int vo = term->viewportOffset();
            float vpW = static_cast<float>(pane->rect().w);
            float vpH = static_cast<float>(pane->rect().h);

            for (int viewRow = 0; viewRow < g.rows(); ++viewRow) {
                int absRow = histSize - vo + viewRow;

                const CellExtra* ex = nullptr;
                if (absRow >= histSize) {
                    int gridRow = absRow - histSize;
                    if (gridRow >= 0 && gridRow < g.rows())
                        ex = g.getExtra(0, gridRow);
                } else {
                    auto* extrasMap = term->document().historyExtras(absRow);
                    if (extrasMap) {
                        auto it = extrasMap->find(0);
                        if (it != extrasMap->end()) ex = &it->second;
                    }
                }

                if (!ex || ex->imageId == 0) continue;
                if (seenImages.count(ex->imageId)) continue;
                seenImages.insert(ex->imageId);

                auto it = term->imageRegistry().find(ex->imageId);
                if (it == term->imageRegistry().end()) continue;
                const auto& img = it->second;

                renderer_.ensureImageGPU(queue_, ex->imageId,
                    img.rgba.data(), img.pixelWidth, img.pixelHeight);

                float imgW = static_cast<float>(img.pixelWidth);
                float imgH = static_cast<float>(img.pixelHeight);
                float imgX = 0.0f;
                float imgY = (static_cast<float>(viewRow) - ex->imageOffsetRow) * lineHeight_;

                float x0 = std::max(imgX, 0.0f);
                float y0 = std::max(imgY, 0.0f);
                float x1 = std::min(imgX + imgW, vpW);
                float y1 = std::min(imgY + imgH, vpH);

                if (x1 <= x0 || y1 <= y0) continue;

                Renderer::ImageDrawCmd cmd;
                cmd.imageId = ex->imageId;
                cmd.x = x0;
                cmd.y = y0;
                cmd.w = x1 - x0;
                cmd.h = y1 - y0;
                cmd.u0 = (x0 - imgX) / imgW;
                cmd.v0 = (y0 - imgY) / imgH;
                cmd.u1 = (x1 - imgX) / imgW;
                cmd.v1 = (y1 - imgY) / imgH;
                imageCmds.push_back(cmd);
            }

            TerminalComputeParams params = {};
            params.cols = static_cast<uint32_t>(g.cols());
            params.rows = static_cast<uint32_t>(g.rows());
            params.cell_width = charWidth_;
            params.cell_height = lineHeight_;
            params.viewport_w = static_cast<float>(pane->rect().w);
            params.viewport_h = static_cast<float>(pane->rect().h);
            params.font_ascender = font->ascender * scale;
            params.font_size = fontSize_;
            params.pane_origin_x = padLeft_;
            params.pane_origin_y = padTop_;
            params.max_text_vertices = cs->maxTextVertices;

            // Cursor
            params.cursor_type = 0;
            if (term->viewportOffset() == 0 && term->cursorVisible() &&
                term->cursorX() >= 0 && term->cursorX() < g.cols() &&
                term->cursorY() >= 0 && term->cursorY() < g.rows()) {
                params.cursor_col   = static_cast<uint32_t>(term->cursorX());
                params.cursor_row   = static_cast<uint32_t>(term->cursorY());
                params.cursor_color = 0xFFCCCCCCu;
                if (!isFocused) {
                    params.cursor_type = 2u; // hollow for unfocused
                } else {
                    switch (term->cursorShape()) {
                    case TerminalEmulator::CursorBlock:
                    case TerminalEmulator::CursorSteadyBlock:
                        params.cursor_type = 1u; // solid block
                        break;
                    case TerminalEmulator::CursorUnderline:
                    case TerminalEmulator::CursorSteadyUnderline:
                        params.cursor_type = 3u; // underline
                        break;
                    case TerminalEmulator::CursorBar:
                    case TerminalEmulator::CursorSteadyBar:
                        params.cursor_type = 4u; // bar
                        break;
                    }
                }
            }

            PooledTexture* newTexture = texturePool_.acquire(
                static_cast<uint32_t>(pane->rect().w),
                static_cast<uint32_t>(pane->rect().h));

            wgpu::CommandEncoderDescriptor encDesc = {};
            wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
            const float* tint = isFocused ? activeTint_ : inactiveTint_;
            renderer_.renderToPane(encoder, queue_, fontName_, params, cs, newTexture->view, tint, imageCmds);
            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
            pendingComputeRelease_.push_back(cs);

            if (rs.heldTexture) rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = newTexture;
            rs.dirty = false;
        }

        if (rs.heldTexture) {
            Renderer::CompositeEntry entry;
            entry.texture = rs.heldTexture->texture;
            entry.srcW = static_cast<uint32_t>(pane->rect().w);
            entry.srcH = static_cast<uint32_t>(pane->rect().h);
            entry.dstX = static_cast<uint32_t>(pane->rect().x);
            entry.dstY = static_cast<uint32_t>(pane->rect().y);
            compositeEntries.push_back(entry);
        }
    }

    // Render tab bar if dirty
    if (tabBarVisible() && tabBarDirty_) {
        renderTabBar();
    }

    // Add tab bar texture to composite entries
    if (tabBarTexture_) {
        PaneRect tbRect = activeTab()->layout()->tabBarRect(fbWidth_, fbHeight_);
        if (!tbRect.isEmpty()) {
            Renderer::CompositeEntry entry;
            entry.texture = tabBarTexture_->texture;
            entry.srcW = static_cast<uint32_t>(tbRect.w);
            entry.srcH = static_cast<uint32_t>(tbRect.h);
            entry.dstX = static_cast<uint32_t>(tbRect.x);
            entry.dstY = static_cast<uint32_t>(tbRect.y);
            compositeEntries.push_back(entry);
        }
    }

    // Composite + submit
    if (!compositeEntries.empty()) {
        wgpu::CommandEncoderDescriptor encDesc = {};
        wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
        renderer_.composite(encoder, surfaceTexture.texture, compositeEntries);

        // Draw per-pane dividers (pre-built vertex buffers, no allocation)
        for (auto& panePtr : currentTab->layout()->panes()) {
            auto it = paneRenderStates_.find(panePtr->id());
            if (it == paneRenderStates_.end() || !it->second.dividerVB) continue;
            renderer_.drawDivider(encoder, surfaceTexture.texture,
                                   fbWidth_, fbHeight_, it->second.dividerVB);
        }

        // Draw per-pane progress bars at top of pane
        if (tabBarConfig_.progress_bar) for (auto& panePtr : currentTab->layout()->panes()) {
            int st = panePtr->progressState();
            if (st == 0) continue;
            const PaneRect& pr = panePtr->rect();
            float barHeight = progressBarHeight_;
            float barY = static_cast<float>(pr.y);
            float barX = static_cast<float>(pr.x);
            float barW = static_cast<float>(pr.w);
            float edgeSoft = 40.0f * contentScaleX_;

            Renderer::ProgressBarParams pbp{};
            pbp.h = barHeight;
            pbp.a = 1.0f;

            if (st == 1 || st == 2) {
                // Determinate: fill from left, sharp left edge, gradient right edge
                float pct = std::clamp(static_cast<float>(panePtr->progressPct()) / 100.0f, 0.0f, 1.0f);
                pbp.x = barX;
                pbp.y = barY;
                pbp.w = barW;
                pbp.fillFrac = pct;
                pbp.edgeSoftness = edgeSoft;
                pbp.softLeft = 0.0f;
                pbp.softRight = 1.0f;
                pbp.r = (st == 2) ? 0.8f : progressColorR_;
                pbp.g = (st == 2) ? 0.2f : progressColorG_;
                pbp.b = (st == 2) ? 0.2f : progressColorB_;
                renderer_.drawProgressBar(encoder, queue_, surfaceTexture.texture,
                                           fbWidth_, fbHeight_, pbp);
            } else if (st == 3) {
                // Indeterminate: sliding segment with gradient edges
                double now = static_cast<double>(TerminalEmulator::mono()) / 1000.0;
                float t = static_cast<float>(std::fmod(now, 2.0) / 2.0);
                float segFrac = 0.3f;
                float segW = barW * segFrac;
                float overshoot = segW;
                float segX = barX - overshoot + t * (barW + 2.0f * overshoot);
                // Clamp to pane — track which edges are clipped
                float x0 = std::max(segX, barX);
                float x1 = std::min(segX + segW, barX + barW);
                bool clippedLeft = (segX < barX);
                bool clippedRight = (segX + segW > barX + barW);
                if (x1 > x0) {
                    pbp.x = x0;
                    pbp.y = barY;
                    pbp.w = x1 - x0;
                    pbp.fillFrac = 1.0f;
                    pbp.edgeSoftness = edgeSoft;
                    pbp.softLeft = clippedLeft ? 0.0f : 1.0f;
                    pbp.softRight = clippedRight ? 0.0f : 1.0f;
                    pbp.r = progressColorR_;
                    pbp.g = progressColorG_;
                    pbp.b = progressColorB_;
                    renderer_.drawProgressBar(encoder, queue_, surfaceTexture.texture,
                                               fbWidth_, fbHeight_, pbp);
                }
            }
        }

        if (pngNeeded) {
            uint32_t bytesPerRow = ((fbWidth_ * 4 + 255) / 256) * 256;
            uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * fbHeight_;

            wgpu::BufferDescriptor bufDesc = {};
            bufDesc.size = bufferSize;
            bufDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            wgpu::Buffer readbackBuf = device_.CreateBuffer(&bufDesc);

            wgpu::TexelCopyTextureInfo src = {};
            src.texture = surfaceTexture.texture;
            wgpu::TexelCopyBufferInfo dst = {};
            dst.buffer = readbackBuf;
            dst.layout.bytesPerRow = bytesPerRow;
            dst.layout.rowsPerImage = fbHeight_;

            wgpu::Extent3D extent = {fbWidth_, fbHeight_, 1};
            encoder.CopyTextureToBuffer(&src, &dst, &extent);

            wgpu::CommandBuffer cmds = encoder.Finish();
            queue_.Submit(1, &cmds);

            DebugIPC* ipc = debugIPC_.get();
            uint32_t w = fbWidth_, h = fbHeight_;
            debugIPC_->markReadbackInProgress();

            readbackBuf.MapAsync(wgpu::MapMode::Read, 0, bufferSize,
                wgpu::CallbackMode::AllowSpontaneous,
                [readbackBuf, ipc, w, h, bytesPerRow](wgpu::MapAsyncStatus status, wgpu::StringView) mutable {
                    if (status != wgpu::MapAsyncStatus::Success) return;
                    const uint8_t* mapped = static_cast<const uint8_t*>(
                        readbackBuf.GetConstMappedRange(0, static_cast<size_t>(bytesPerRow) * h));
                    if (!mapped) { readbackBuf.Unmap(); return; }

                    std::vector<uint8_t> rgba(w * h * 4);
                    for (uint32_t row = 0; row < h; ++row) {
                        const uint8_t* s = mapped + row * bytesPerRow;
                        uint8_t* d = rgba.data() + row * w * 4;
                        for (uint32_t col = 0; col < w; ++col) {
                            d[col*4+0] = s[col*4+2];
                            d[col*4+1] = s[col*4+1];
                            d[col*4+2] = s[col*4+0];
                            d[col*4+3] = s[col*4+3];
                        }
                    }
                    readbackBuf.Unmap();

                    std::vector<uint8_t> pngData;
                    stbi_write_png_to_func(
                        [](void* ctx, void* data, int size) {
                            auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
                            vec->insert(vec->end(),
                                        static_cast<uint8_t*>(data),
                                        static_cast<uint8_t*>(data) + size);
                        },
                        &pngData, static_cast<int>(w), static_cast<int>(h), 4,
                        rgba.data(), static_cast<int>(w * 4));

                    std::string b64 = base64::encode(pngData.data(), pngData.size());
                    ipc->onPngReady(b64);
                });
        } else {
            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
        }

        // Collect all pending releases from all panes and tab bar
        std::vector<PooledTexture*> toRelease;
        for (auto& panePtr : currentTab->layout()->panes()) {
            auto it = paneRenderStates_.find(panePtr->id());
            if (it != paneRenderStates_.end()) {
                toRelease.insert(toRelease.end(),
                    it->second.pendingRelease.begin(),
                    it->second.pendingRelease.end());
                it->second.pendingRelease.clear();
            }
        }
        toRelease.insert(toRelease.end(), pendingTabBarRelease_.begin(), pendingTabBarRelease_.end());
        pendingTabBarRelease_.clear();
        auto texturesToRelease = toRelease;
        auto computeToRelease  = pendingComputeRelease_;
        pendingComputeRelease_.clear();
        TexturePool*      texturePool = &texturePool_;
        ComputeStatePool* computePool = &renderer_.computePool();
        queue_.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
            [texturePool, texturesToRelease, computeToRelease, computePool]
            (wgpu::QueueWorkDoneStatus, wgpu::StringView) mutable {
                for (auto* t  : texturesToRelease) texturePool->release(t);
                for (auto* cs : computeToRelease)  computePool->release(cs);
            });
    }

    surface_.Present();
    needsRedraw_ = false;
}

static void collectFirstPaneDividers(const LayoutNode* node, int divPx,
                                      std::vector<std::pair<int, PaneRect>>& out)
{
    if (!node || node->isLeaf || divPx <= 0) return;
    const PaneRect& r = node->rect;

    // The "first" (left/top) child owns this divider.
    int firstPaneId = -1;
    const LayoutNode* first = node->first.get();
    while (first && !first->isLeaf) first = first->first.get(); // leftmost leaf
    if (first) firstPaneId = first->paneId;

    if (firstPaneId >= 0) {
        PaneRect divRect;
        if (node->dir == LayoutNode::Dir::Horizontal) {
            int splitX = node->first ? (node->first->rect.x + node->first->rect.w) : 0;
            divRect = {splitX, r.y, divPx, r.h};
        } else {
            int splitY = node->first ? (node->first->rect.y + node->first->rect.h) : 0;
            divRect = {r.x, splitY, r.w, divPx};
        }
        out.push_back({firstPaneId, divRect});
    }

    collectFirstPaneDividers(node->first.get(),  divPx, out);
    collectFirstPaneDividers(node->second.get(), divPx, out);
}

void PlatformDawn::refreshDividers(Tab* tab)
{
    if (!tab || dividerWidth_ <= 0) return;
    Layout* layout = tab->layout();

    // Clear all divider VBs for this tab's panes
    for (auto& panePtr : layout->panes())
        paneRenderStates_[panePtr->id()].dividerVB = {};

    if (layout->panes().size() <= 1 || layout->isZoomed()) return;

    // Collect (paneId, dividerRect) for each split node
    std::vector<std::pair<int, PaneRect>> dividers;
    collectFirstPaneDividers(layout->root(), dividerWidth_, dividers);

    renderer_.updateDividerViewport(queue_, fbWidth_, fbHeight_);

    for (auto& [paneId, dr] : dividers) {
        auto it = paneRenderStates_.find(paneId);
        if (it == paneRenderStates_.end()) continue;
        renderer_.updateDividerBuffer(queue_, it->second.dividerVB,
            static_cast<float>(dr.x), static_cast<float>(dr.y),
            static_cast<float>(dr.w), static_cast<float>(dr.h),
            dividerR_, dividerG_, dividerB_, dividerA_);
    }
}

void PlatformDawn::clearDividers(Tab* tab)
{
    if (!tab) return;
    for (auto& panePtr : tab->layout()->panes())
        paneRenderStates_[panePtr->id()].dividerVB = {};
}

void PlatformDawn::notifyPaneFocusChange(Tab* tab, int prevId, int newId)
{
    if (!tab) return;
    if (prevId >= 0) {
        Pane* p = tab->layout()->pane(prevId);
        if (p && p->activeTerm()) p->activeTerm()->focusEvent(false);
    }
    if (newId >= 0) {
        Pane* p = tab->layout()->pane(newId);
        if (p && p->activeTerm()) p->activeTerm()->focusEvent(true);
    }
}

void PlatformDawn::updateTabTitleFromFocusedPane(int tabIdx)
{
    if (tabIdx < 0 || tabIdx >= static_cast<int>(tabs_.size())) return;
    Tab* tab = tabs_[tabIdx].get();
    Pane* fp = tab->layout()->focusedPane();
    if (!fp) return;

    const std::string& title = fp->title();
    const std::string& icon  = fp->icon();
    tab->setTitle(title);
    if (!icon.empty()) tab->setIcon(icon);
    if (tabIdx == activeTabIdx_ && !title.empty())
        glfwSetWindowTitle(glfwWindow_, title.c_str());
    tabBarDirty_ = true;
    needsRedraw_  = true;
}

void PlatformDawn::releaseTabTextures(Tab* tab)
{
    if (!tab) return;
    for (auto& panePtr : tab->layout()->panes()) {
        auto it = paneRenderStates_.find(panePtr->id());
        if (it == paneRenderStates_.end()) continue;
        PaneRenderState& rs = it->second;
        if (rs.heldTexture) {
            pendingTabBarRelease_.push_back(rs.heldTexture);
            rs.heldTexture = nullptr;
            rs.dirty = true;
        }
    }
}

void PlatformDawn::terminalExited(Terminal* terminal)
{
    // Find which tab and pane owns this terminal
    for (int tabIdx = 0; tabIdx < static_cast<int>(tabs_.size()); ++tabIdx) {
        Tab* tab = tabs_[tabIdx].get();
        for (auto& panePtr : tab->layout()->panes()) {
            if (panePtr->terminal() != terminal) continue;

            int paneId = panePtr->id();
            removePtyPoll(terminal->masterFD());

            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) {
                if (it->second.heldTexture)
                    pendingTabBarRelease_.push_back(it->second.heldTexture);
                for (auto* tx : it->second.pendingRelease)
                    pendingTabBarRelease_.push_back(tx);
                paneRenderStates_.erase(it);
            }

            if (tab->layout()->panes().size() <= 1) {
                // Last pane in the tab — close the tab
                tabs_.erase(tabs_.begin() + tabIdx);
                if (tabs_.empty()) {
                    quit();
                    return;
                }
                if (activeTabIdx_ >= static_cast<int>(tabs_.size()))
                    activeTabIdx_ = static_cast<int>(tabs_.size()) - 1;
                tabBarDirty_ = true;
            } else {
                tab->layout()->removePane(paneId);
                resizeAllPanesInTab(tab);
                notifyPaneFocusChange(tab, -1, tab->layout()->focusedPaneId());
                updateTabTitleFromFocusedPane(tabIdx);
            }

            needsRedraw_ = true;
            return;
        }
    }
    // Fallback: terminal not found in any pane
    quit();
}

void PlatformDawn::spawnTerminalForPane(Pane* pane, int tabIdx)
{
    int paneId = pane->id();

    TerminalCallbacks cbs;
    cbs.event = [this, paneId](TerminalEmulator*, int ev, void*) {
        if (ev == TerminalEmulator::Update || ev == TerminalEmulator::ScrollbackChanged) {
            needsRedraw_ = true;
            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) it->second.dirty = true;
        }
    };
    cbs.copyToClipboard = [this](const std::string& text) {
        glfwSetClipboardString(glfwWindow_, text.c_str());
    };
    cbs.pasteFromClipboard = [this]() -> std::string {
        const char* clip = glfwGetClipboardString(glfwWindow_);
        return clip ? std::string(clip) : std::string();
    };
    cbs.onTitleChanged = [this, paneId, tabIdx](const std::string& title) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setTitle(title);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setTitle(title);
            if (tabIdx == activeTabIdx_) glfwSetWindowTitle(glfwWindow_, title.c_str());
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onIconChanged = [this, paneId, tabIdx](const std::string& icon) {
        if (tabIdx >= static_cast<int>(tabs_.size())) return;
        Tab* t = tabs_[tabIdx].get();
        if (Pane* p = t->layout()->pane(paneId)) p->setIcon(icon);
        if (t->layout()->focusedPaneId() == paneId) {
            t->setIcon(icon);
            tabBarDirty_ = true;
            needsRedraw_ = true;
        }
    };
    cbs.onProgressChanged = [this, paneId](int state, int pct) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setProgress(state, pct);
                tabBarDirty_ = true;
                needsRedraw_ = true;
                break;
            }
        }
    };
    cbs.cellPixelWidth  = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };
    cbs.isDarkMode = []() { return platformIsDarkMode(); };
    cbs.onCWDChanged = [this, paneId](const std::string& dir) {
        for (auto& tab : tabs_) {
            if (Pane* p = tab->layout()->pane(paneId)) {
                p->setCWD(dir);
                break;
            }
        }
    };
    cbs.onDesktopNotification = [](const std::string& title, const std::string& body, const std::string&) {
        platformSendNotification(title, body);
    };

    auto terminal = std::make_unique<Terminal>(this, std::move(cbs));
    terminal->applyColorScheme(terminalOptions_.colors);
    if (!terminal->init(terminalOptions_)) {
        spdlog::error("spawnTerminalForPane: failed to init terminal");
        return;
    }

    const PaneRect& pr = pane->rect();
    int cols = (pr.w > 0 && charWidth_ > 0)  ? static_cast<int>(pr.w / charWidth_)  : 80;
    int rows = (pr.h > 0 && lineHeight_ > 0) ? static_cast<int>(pr.h / lineHeight_) : 24;
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);

    auto& rs = paneRenderStates_[paneId];
    rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
    rs.dirty = true;

    terminal->resize(cols, rows);
    {
        struct winsize ws = {};
        ws.ws_col    = static_cast<unsigned short>(cols);
        ws.ws_row    = static_cast<unsigned short>(rows);
        ws.ws_xpixel = static_cast<unsigned short>(pr.w);
        ws.ws_ypixel = static_cast<unsigned short>(pr.h);
        ioctl(terminal->masterFD(), TIOCSWINSZ, &ws);
    }

    // Set initial title from shell name
    {
        std::string shellName = terminalOptions_.shell;
        auto slash = shellName.rfind('/');
        if (slash != std::string::npos) shellName = shellName.substr(slash + 1);
        pane->setTitle(shellName);
    }

    int masterFD = terminal->masterFD();
    Terminal* termPtr = terminal.get();
    pane->setTerminal(std::move(terminal));
    addPtyPoll(masterFD, termPtr);
}

void PlatformDawn::resizeAllPanesInTab(Tab* tab)
{
    if (!tab) return;
    clearDividers(tab);
    tab->layout()->computeRects(fbWidth_, fbHeight_);

    for (auto& panePtr : tab->layout()->panes()) {
        Pane* pane = panePtr.get();
        pane->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);

        TerminalEmulator* term = pane->activeTerm();
        if (!term) continue;

        int cols = std::max(term->width(),  1);
        int rows = std::max(term->height(), 1);

        auto& rs = paneRenderStates_[pane->id()];
        rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);
        rs.dirty = true;
        if (rs.heldTexture) {
            rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = nullptr;
        }

        if (auto* t = dynamic_cast<Terminal*>(term); t && t->masterFD() >= 0) {
            struct winsize ws = {};
            ws.ws_col    = static_cast<unsigned short>(cols);
            ws.ws_row    = static_cast<unsigned short>(rows);
            ws.ws_xpixel = static_cast<unsigned short>(pane->rect().w);
            ws.ws_ypixel = static_cast<unsigned short>(pane->rect().h);
            ioctl(t->masterFD(), TIOCSWINSZ, &ws);
        }
    }
    refreshDividers(tab);
    needsRedraw_ = true;
}

void PlatformDawn::dispatchAction(const Action::Any& action)
{
    std::visit(overloaded {
        [&](const Action::NewTab&)  { createTab(); },
        [&](const Action::CloseTab&) { closeTab(activeTabIdx_); },
        [&](const Action::ActivateTabRelative& a) {
            int idx = activeTabIdx_ + a.delta;
            if (idx >= 0 && idx < static_cast<int>(tabs_.size())) {
                clearDividers(activeTab());
                releaseTabTextures(activeTab());
                activeTabIdx_ = idx;
                refreshDividers(activeTab());
                tabBarDirty_ = true;
                needsRedraw_ = true;
            }
        },
        [&](const Action::ActivateTab& a) {
            if (a.index >= 0 && a.index < static_cast<int>(tabs_.size())) {
                clearDividers(activeTab());
                releaseTabTextures(activeTab());
                activeTabIdx_ = a.index;
                refreshDividers(activeTab());
                tabBarDirty_ = true;
                needsRedraw_ = true;
            }
        },
        [&](const Action::SplitPane& a) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            Pane* fp = layout->focusedPane();
            if (!fp) return;

            LayoutNode::Dir dir;
            bool newIsFirst = false;
            switch (a.dir) {
            case Action::Direction::Right: dir = LayoutNode::Dir::Horizontal; break;
            case Action::Direction::Left:  dir = LayoutNode::Dir::Horizontal; newIsFirst = true; break;
            case Action::Direction::Down:  dir = LayoutNode::Dir::Vertical;   break;
            case Action::Direction::Up:    dir = LayoutNode::Dir::Vertical;   newIsFirst = true; break;
            default: return;
            }

            int newId = layout->splitPane(fp->id(), dir, 0.5f, newIsFirst);
            if (newId < 0) return;

            Pane* newPane = layout->pane(newId);
            if (!newPane) return;

            layout->computeRects(fbWidth_, fbHeight_);
            int tabIdx = activeTabIdx_;
            int prevId = layout->focusedPaneId();
            spawnTerminalForPane(newPane, tabIdx);
            resizeAllPanesInTab(tab);
            layout->setFocusedPane(newId);
            notifyPaneFocusChange(tab, prevId, newId);
            updateTabTitleFromFocusedPane(activeTabIdx_);
        },
        [&](const Action::ClosePane&) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            if (layout->panes().size() <= 1) return; // keep last pane
            Pane* fp = layout->focusedPane();
            if (!fp) return;
            int paneId = fp->id();

            // Stop PTY poll and release render state
            if (auto* t = dynamic_cast<Terminal*>(fp->activeTerm())) {
                removePtyPoll(t->masterFD());
            }
            auto it = paneRenderStates_.find(paneId);
            if (it != paneRenderStates_.end()) {
                if (it->second.heldTexture)
                    pendingTabBarRelease_.push_back(it->second.heldTexture);
                for (auto* tx : it->second.pendingRelease)
                    pendingTabBarRelease_.push_back(tx);
                paneRenderStates_.erase(it);
            }

            layout->removePane(paneId);
            resizeAllPanesInTab(tab);
            notifyPaneFocusChange(tab, -1, layout->focusedPaneId());
            updateTabTitleFromFocusedPane(activeTabIdx_);
        },
        [&](const Action::ZoomPane&) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            Pane* fp = layout->focusedPane();
            if (!fp) return;
            layout->zoomPane(fp->id());
            resizeAllPanesInTab(tab);
        },
        [&](const Action::FocusPane& a) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();

            if (a.dir == Action::Direction::Next || a.dir == Action::Direction::Prev) {
                const auto& panes = layout->panes();
                if (panes.size() <= 1) return;
                int cur = -1;
                for (int i = 0; i < static_cast<int>(panes.size()); ++i) {
                    if (panes[i]->id() == layout->focusedPaneId()) { cur = i; break; }
                }
                if (cur < 0) return;
                int n = static_cast<int>(panes.size());
                int delta = (a.dir == Action::Direction::Next) ? 1 : -1;
                int next = ((cur + delta) % n + n) % n;
                int prev = layout->focusedPaneId();
                layout->setFocusedPane(panes[next]->id());
                notifyPaneFocusChange(tab, prev, panes[next]->id());
                updateTabTitleFromFocusedPane(activeTabIdx_);
                needsRedraw_ = true;
                return;
            }

            Pane* fp = layout->focusedPane();
            if (!fp) return;
            const PaneRect& r = fp->rect();
            int px = 0, py = 0;
            switch (a.dir) {
            case Action::Direction::Left:  px = r.x - 1;       py = r.y + r.h / 2; break;
            case Action::Direction::Right: px = r.x + r.w + 1; py = r.y + r.h / 2; break;
            case Action::Direction::Up:    px = r.x + r.w / 2; py = r.y - 1;       break;
            case Action::Direction::Down:  px = r.x + r.w / 2; py = r.y + r.h + 1; break;
            default: return;
            }
            int targetId = layout->paneAtPixel(px, py);
            if (targetId >= 0 && targetId != fp->id()) {
                int prev = layout->focusedPaneId();
                layout->setFocusedPane(targetId);
                notifyPaneFocusChange(tab, prev, targetId);
                updateTabTitleFromFocusedPane(activeTabIdx_);
                needsRedraw_ = true;
            }
        },
        [&](const Action::AdjustPaneSize& a) {
            Tab* tab = activeTab();
            if (!tab) return;
            Layout* layout = tab->layout();
            Pane* fp = layout->focusedPane();
            if (!fp) return;

            LayoutNode::Dir splitDir;
            int deltaPixels;
            switch (a.dir) {
            case Action::Direction::Left:
                splitDir   = LayoutNode::Dir::Horizontal;
                deltaPixels = -static_cast<int>(a.amount * charWidth_);
                break;
            case Action::Direction::Right:
                splitDir   = LayoutNode::Dir::Horizontal;
                deltaPixels = static_cast<int>(a.amount * charWidth_);
                break;
            case Action::Direction::Up:
                splitDir   = LayoutNode::Dir::Vertical;
                deltaPixels = -static_cast<int>(a.amount * lineHeight_);
                break;
            case Action::Direction::Down:
                splitDir   = LayoutNode::Dir::Vertical;
                deltaPixels = static_cast<int>(a.amount * lineHeight_);
                break;
            default: return;
            }
            if (layout->growPane(fp->id(), splitDir, deltaPixels))
                resizeAllPanesInTab(tab);
        },
        [&](const Action::Copy&) {
            Terminal* term = activeTerm();
            if (term && term->hasSelection()) {
                std::string text = term->selectedText();
                if (!text.empty())
                    glfwSetClipboardString(glfwWindow_, text.c_str());
            }
        },
        [&](const Action::Paste&) {
            Terminal* term = activeTerm();
            const char* clip = glfwGetClipboardString(glfwWindow_);
            if (term && clip && clip[0])
                term->pasteText(std::string(clip));
        },
        [&](const Action::ScrollUp& a) {
            Terminal* term = activeTerm();
            if (term) term->scrollViewport(a.lines);
        },
        [&](const Action::ScrollDown& a) {
            Terminal* term = activeTerm();
            if (term) term->scrollViewport(-a.lines);
        },
        [&](const Action::ScrollToTop&) {
            Terminal* term = activeTerm();
            if (term) term->scrollViewport(std::numeric_limits<int>::max());
        },
        [&](const Action::ScrollToBottom&) {
            Terminal* term = activeTerm();
            if (term) term->resetViewport();
        },
        [&](const Action::PushOverlay&) { /* TODO */ },
        [&](const Action::PopOverlay&) {
            Tab* tab = activeTab();
            if (tab) tab->popOverlay();
        },
        [&](const Action::IncreaseFontSize&) { adjustFontSize(1.0f);  },
        [&](const Action::DecreaseFontSize&) { adjustFontSize(-1.0f); },
        [&](const Action::ResetFontSize&)    { adjustFontSize(0.0f);  },
    }, action);
}

void PlatformDawn::onKey(int key, int /*scancode*/, int action, int mods)
{
    TerminalEmulator* term = activeTerm();
    if (!term) return;

    spdlog::debug("onKey: key={} action={} mods={}", key, action, mods);
    term->resetViewport();

    if (action == GLFW_RELEASE) {
        controlPressed_ = (mods & GLFW_MOD_CONTROL) != 0;
        return;
    }

    controlPressed_ = (mods & GLFW_MOD_CONTROL) != 0;
    lastMods_ = glfwModsToModifiers(mods);

    Key k = glfwKeyToKey(key);
    spdlog::debug("onKey: mapped key=0x{:x} controlPressed={}", static_cast<int>(k), controlPressed_);

    // Try binding lookup via sequence matcher
    auto result = sequenceMatcher_.advance({k, lastMods_}, bindings_);
    if (result.result == SequenceMatcher::Result::Match) {
        dispatchAction(*result.action);
        return;
    }
    if (result.result == SequenceMatcher::Result::Prefix) {
        // Consumed — waiting for next key in sequence
        return;
    }

    // NoMatch: forward to PTY
    if (controlPressed_ && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        KeyEvent ev;
        ev.key = k;
        ev.text = std::string(1, static_cast<char>(key - GLFW_KEY_A + 1));
        ev.count = 1;
        ev.autoRepeat = (action == GLFW_REPEAT);
        spdlog::debug("onKey: ctrl+letter, sending text=0x{:02x}", static_cast<unsigned char>(ev.text[0]));
        term->keyPressEvent(&ev);
        return;
    }

    if (k != Key_unknown && (key < GLFW_KEY_SPACE || key > GLFW_KEY_GRAVE_ACCENT)) {
        KeyEvent ev;
        ev.key = k;
        ev.count = 1;
        ev.autoRepeat = (action == GLFW_REPEAT);
        spdlog::debug("onKey: non-printable key=0x{:x}, dispatching", static_cast<int>(k));
        term->keyPressEvent(&ev);
    } else {
        spdlog::debug("onKey: printable key={}, deferring to onChar", key);
    }
}

void PlatformDawn::onChar(unsigned int codepoint)
{
    TerminalEmulator* term = activeTerm();
    if (!term) return;

    spdlog::debug("onChar: codepoint=U+{:04X} controlPressed={}", codepoint, controlPressed_);
    term->resetViewport();
    if (controlPressed_) return;

    KeyEvent ev;
    ev.key = Key_unknown;
    ev.text = codepointToUtf8(codepoint);
    spdlog::debug("onChar: sending text='{}' ({} bytes)", ev.text, ev.text.size());
    ev.count = 1;
    ev.autoRepeat = false;
    term->keyPressEvent(&ev);
}

void PlatformDawn::adjustFontSize(float delta)
{
    float newSize;
    if (delta == 0.0f) {
        newSize = baseFontSize_; // reset
    } else {
        newSize = fontSize_ + delta * contentScaleX_;
    }
    if (newSize < 6.0f * contentScaleX_ || newSize > 72.0f * contentScaleX_) return;
    fontSize_ = newSize;

    // Recalculate metrics
    const FontData* font = textSystem_.getFont(fontName_);
    if (!font) return;
    float scale = fontSize_ / font->baseSize;
    lineHeight_ = font->lineHeight * scale;
    const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
    charWidth_ = shaped.width;
    if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f;

    // Trigger resize of all panes (recalculates grid dimensions)
    onFramebufferResize(static_cast<int>(fbWidth_), static_cast<int>(fbHeight_));
}

void PlatformDawn::onFramebufferResize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    fbWidth_ = static_cast<uint32_t>(width);
    fbHeight_ = static_cast<uint32_t>(height);

    configureSurface(fbWidth_, fbHeight_);
    renderer_.setViewportSize(fbWidth_, fbHeight_);
    renderer_.updateDividerViewport(queue_, fbWidth_, fbHeight_);

    // Clear divider buffers for all tabs — geometry is now stale
    for (auto& tabPtr : tabs_)
        clearDividers(tabPtr.get());

    Tab* tab = activeTab();
    if (!tab) return;

    tab->layout()->computeRects(fbWidth_, fbHeight_);

    for (auto& panePtr : tab->layout()->panes()) {
        Pane* pane = panePtr.get();
        pane->resizeToRect(charWidth_, lineHeight_, padLeft_, padTop_, padRight_, padBottom_);

        TerminalEmulator* term = pane->activeTerm();
        if (!term) continue;

        int cols = term->width();
        int rows = term->height();
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;

        auto& rs = paneRenderStates_[pane->id()];
        rs.resolvedCells.resize(static_cast<size_t>(cols) * rows);

        // Update PTY window size
        Terminal* t = dynamic_cast<Terminal*>(term);
        if (t && t->masterFD() >= 0) {
            struct winsize ws;
            ws.ws_col = static_cast<unsigned short>(cols);
            ws.ws_row = static_cast<unsigned short>(rows);
            ws.ws_xpixel = static_cast<unsigned short>(pane->rect().w);
            ws.ws_ypixel = static_cast<unsigned short>(pane->rect().h);
            ioctl(t->masterFD(), TIOCSWINSZ, &ws);
        }
    }

    // Release all held textures — they're now the wrong size for the new framebuffer.
    for (auto& [id, rs] : paneRenderStates_) {
        if (rs.heldTexture) {
            rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = nullptr;
        }
        rs.dirty = true;
    }
    if (tabBarTexture_) {
        pendingTabBarRelease_.push_back(tabBarTexture_);
        tabBarTexture_ = nullptr;
    }
    tabBarDirty_ = true;
    refreshDividers(tab);
    needsRedraw_ = true;
}

void PlatformDawn::onMouseButton(int button, int action, int mods)
{
    Tab* tab = activeTab();
    if (!tab) return;

    lastMods_ = glfwModsToModifiers(mods);

    double x, y;
    glfwGetCursorPos(glfwWindow_, &x, &y);
    double sx = x * contentScaleX_;
    double sy = y * contentScaleY_;

    // Check if click is in tab bar
    if (action == GLFW_PRESS && tabBarVisible()) {
        PaneRect tbRect = tab->layout()->tabBarRect(fbWidth_, fbHeight_);
        if (!tbRect.isEmpty() &&
            sx >= tbRect.x && sx < tbRect.x + tbRect.w &&
            sy >= tbRect.y && sy < tbRect.y + tbRect.h) {
            // Determine which tab was clicked by accumulated widths
            if (tabBarCharWidth_ > 0.0f) {
                int clickCol = static_cast<int>((sx - tbRect.x) / tabBarCharWidth_);
                int col = 0;
                for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
                    Tab* t = tabs_[i].get();
                    // Compute tab display width: same as renderTabBar logic
                    std::string text = " ";
                    if (!t->icon().empty()) text += t->icon() + " ";
                    text += "[" + std::to_string(i + 1) + "] ";
                    if (!t->title().empty()) text += t->title();
                    text += " ";
                    // Count UTF-8 chars
                    int w = 0;
                    const char* p = text.c_str();
                    while (*p) {
                        uint8_t b = static_cast<uint8_t>(*p);
                        if (b < 0x80) { w++; p++; }
                        else if ((b & 0xE0) == 0xC0) { w++; p += 2; }
                        else if ((b & 0xF0) == 0xE0) { w++; p += 3; }
                        else { w++; p += 4; }
                    }
                    w += 1; // separator
                    if (clickCol >= col && clickCol < col + w) {
                        if (button == GLFW_MOUSE_BUTTON_LEFT) {
                            clearDividers(activeTab());
                            releaseTabTextures(activeTab());
                            activeTabIdx_ = i;
                            refreshDividers(activeTab());
                            tabBarDirty_ = true;
                            needsRedraw_ = true;
                        } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
                            closeTab(i);
                        }
                        return;
                    }
                    col += w;
                }
            }
            return;
        }
    }

    // Click on an inactive pane — switch focus before routing the event
    if (action == GLFW_PRESS && !tab->hasOverlay()) {
        int clickedId = tab->layout()->paneAtPixel(static_cast<int>(sx), static_cast<int>(sy));
        if (clickedId >= 0 && clickedId != tab->layout()->focusedPaneId()) {
            int prev = tab->layout()->focusedPaneId();
            tab->layout()->setFocusedPane(clickedId);
            notifyPaneFocusChange(tab, prev, clickedId);
            updateTabTitleFromFocusedPane(activeTabIdx_);
        }
    }

    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? fp->activeTerm() : nullptr);
    if (!term) return;

    // Adjust for pane origin
    PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
    double relX = sx - pr.x;
    double relY = sy - pr.y;

    MouseEvent ev;
    ev.x = static_cast<int>(relX / charWidth_);
    ev.y = static_cast<int>(relY / lineHeight_);
    ev.globalX = static_cast<int>(sx);
    ev.globalY = static_cast<int>(sy);
    ev.modifiers = lastMods_;

    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:  ev.button = LeftButton; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: ev.button = MidButton; break;
    case GLFW_MOUSE_BUTTON_RIGHT: ev.button = RightButton; break;
    default: ev.button = NoButton; break;
    }
    ev.buttons = ev.button;

    // Cmd/Ctrl+click on hyperlinks opens the URL
    if (action == GLFW_RELEASE && ev.button == LeftButton &&
        (ev.modifiers & (MetaModifier | CtrlModifier))) {
        int col = ev.x, row = ev.y;
        if (col >= 0 && col < term->width() && row >= 0 && row < term->height()) {
            const CellExtra* extra = term->grid().getExtra(col, row);
            if (extra && extra->hyperlinkId) {
                const std::string* uri = term->hyperlinkURI(extra->hyperlinkId);
                if (uri && !uri->empty()) {
                    platformOpenURL(*uri);
                    return;
                }
            }
        }
    }

    if (action == GLFW_PRESS) term->mousePressEvent(&ev);
    else term->mouseReleaseEvent(&ev);
}

void PlatformDawn::onCursorPos(double x, double y)
{
    Tab* tab = activeTab();
    if (!tab) return;
    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? fp->activeTerm() : nullptr);
    if (!term) return;

    double sx = x * contentScaleX_;
    double sy = y * contentScaleY_;

    PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
    double relX = sx - pr.x;
    double relY = sy - pr.y;

    MouseEvent ev;
    ev.x = static_cast<int>(relX / charWidth_);
    ev.y = static_cast<int>(relY / lineHeight_);
    ev.globalX = static_cast<int>(sx);
    ev.globalY = static_cast<int>(sy);
    ev.button = NoButton;
    ev.modifiers = lastMods_;
    if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
        ev.buttons |= LeftButton;
    if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
        ev.buttons |= MidButton;
    if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
        ev.buttons |= RightButton;
    term->mouseMoveEvent(&ev);
}

// ========================================================================
// Tab bar
// ========================================================================

void PlatformDawn::initTabBar(const TabBarConfig& cfg)
{
    if (cfg.style == "hidden") {
        for (auto& tab : tabs_)
            tab->layout()->setTabBar(0, cfg.position);
        return;
    }

    // For "auto" mode with 1 tab, still load fonts but set height to 0

    // Resolve font
    std::string fontPath = cfg.font.empty() ? primaryFontPath_
                                             : resolveFontFamily(cfg.font);
    if (fontPath.empty()) fontPath = primaryFontPath_;
    float fontSize = (cfg.font_size > 0.0f) ? cfg.font_size * contentScaleX_ : fontSize_;
    tabBarFontSize_ = fontSize;

    auto fontData = loadFontFile(fontPath);
    if (!fontData.empty()) {
        std::vector<std::vector<uint8_t>> fl = {fontData};
        auto nerdPath = (fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf").string();
        auto nerdData = loadFontFile(nerdPath);
        if (!nerdData.empty()) fl.push_back(std::move(nerdData));
        textSystem_.registerFont(tabBarFontName_, fl, 48.0f);
        textSystem_.setPrimaryFontPath(tabBarFontName_, fontPath);
    }

    const FontData* font = textSystem_.getFont(tabBarFontName_);
    if (font) {
        renderer_.uploadFontAtlas(queue_, tabBarFontName_, *font);
        float scale = tabBarFontSize_ / font->baseSize;
        tabBarLineHeight_ = font->lineHeight * scale;
        const auto& shaped = textSystem_.shapeText(tabBarFontName_, "M", tabBarFontSize_);
        tabBarCharWidth_ = shaped.width;
        if (tabBarCharWidth_ < 1.0f) tabBarCharWidth_ = tabBarFontSize_ * 0.6f;
    } else {
        tabBarLineHeight_ = lineHeight_;
        tabBarCharWidth_  = charWidth_;
    }

    int tabBarH = tabBarVisible() ? static_cast<int>(std::ceil(tabBarLineHeight_)) : 0;
    for (auto& tab : tabs_)
        tab->layout()->setTabBar(tabBarH, cfg.position);

    // Parse colors
    tbBgColor_         = parseHexColor(cfg.colors.background);
    tbActiveBgColor_   = parseHexColor(cfg.colors.active_bg);
    tbActiveFgColor_   = parseHexColor(cfg.colors.active_fg);
    tbInactiveBgColor_ = parseHexColor(cfg.colors.inactive_bg);
    tbInactiveFgColor_ = parseHexColor(cfg.colors.inactive_fg);

    // Parse progress bar settings
    progressBarHeight_ = cfg.progress_height * contentScaleX_;
    {
        uint8_t r, g, b;
        if (color::parseHex(cfg.progress_color, r, g, b)) {
            progressColorR_ = r / 255.0f;
            progressColorG_ = g / 255.0f;
            progressColorB_ = b / 255.0f;
        }
    }

    tabBarDirty_ = true;
}

void PlatformDawn::renderTabBar()
{
    if (!tabBarVisible()) return;
    if (tabs_.empty()) return;

    Tab* active = activeTab();
    if (!active) return;
    PaneRect tbRect = active->layout()->tabBarRect(fbWidth_, fbHeight_);
    if (tbRect.isEmpty()) return;

    int cols = std::max(1, static_cast<int>(tbRect.w / tabBarCharWidth_));
    tabBarCols_ = cols;

    // Build resolved cells for 1 row
    std::vector<ResolvedCell> cells(static_cast<size_t>(cols));
    std::vector<GlyphEntry> tabBarGlyphs;
    for (auto& c : cells) {
        c.glyph_offset = 0;
        c.glyph_count = 0;
        c.fg_color = tbInactiveFgColor_;
        c.bg_color = tbBgColor_;
        c.underline_info = 0;
    }

    // Powerline separator U+E0B0
    const std::string SEP_RIGHT = "\xee\x82\xb0";

    // Indeterminate animation glyphs
    static const char32_t kAnimGlyphs[] = {
        0xf0130, 0xf0a9e, 0xf0a9f, 0xf0aa0, 0xf0aa1,
        0xf0aa2, 0xf0aa3, 0xf0aa4, 0xf0aa5
    };
    static constexpr int kAnimCount = 9;

    auto cp32ToUtf8 = [](char32_t cp) -> std::string {
        std::string s;
        if (cp < 0x80) { s += static_cast<char>(cp); }
        else if (cp < 0x800) { s += static_cast<char>(0xC0|(cp>>6)); s += static_cast<char>(0x80|(cp&0x3F)); }
        else if (cp < 0x10000) { s += static_cast<char>(0xE0|(cp>>12)); s += static_cast<char>(0x80|((cp>>6)&0x3F)); s += static_cast<char>(0x80|(cp&0x3F)); }
        else { s += static_cast<char>(0xF0|(cp>>18)); s += static_cast<char>(0x80|((cp>>12)&0x3F)); s += static_cast<char>(0x80|((cp>>6)&0x3F)); s += static_cast<char>(0x80|(cp&0x3F)); }
        return s;
    };

    auto progressGlyph = [&](Tab* tab) -> std::string {
        Pane* focusedPane = tab->layout()->focusedPane();
        int st = focusedPane ? focusedPane->progressState() : 0;
        int pct = focusedPane ? focusedPane->progressPct() : 0;
        if (st == 0) return "";
        int idx;
        if (st == 3) {
            // Bounce: 0..8..0..8.. (period = 2 * (kAnimCount - 1))
            int period = 2 * (kAnimCount - 1);
            int pos = tabBarAnimFrame_ % period;
            idx = (pos < kAnimCount) ? pos : period - pos;
        } else if (st == 1 || st == 2) {
            idx = std::clamp(pct * kAnimCount / 100, 0, kAnimCount - 1);
        } else {
            return "";
        }
        return cp32ToUtf8(kAnimGlyphs[idx]);
    };

    // Helper: count UTF-8 codepoints in a string
    auto cpLen = [](const std::string& s) -> int {
        int w = 0;
        const char* p = s.c_str();
        while (*p) {
            uint8_t b = static_cast<uint8_t>(*p);
            if (b < 0x80) p++; else if ((b & 0xE0) == 0xC0) p += 2;
            else if ((b & 0xF0) == 0xE0) p += 3; else p += 4;
            w++;
        }
        return w;
    };

    // Helper: truncate UTF-8 string to maxCp codepoints, append ellipsis if truncated
    auto truncUtf8 = [](const std::string& s, int maxCp) -> std::string {
        if (maxCp <= 0) return {};
        int cp = 0;
        const char* p = s.c_str();
        while (*p && cp < maxCp) {
            uint8_t b = static_cast<uint8_t>(*p);
            if (b < 0x80) p++; else if ((b & 0xE0) == 0xC0) p += 2;
            else if ((b & 0xF0) == 0xE0) p += 3; else p += 4;
            cp++;
        }
        if (*p) return std::string(s.c_str(), p) + "\xe2\x80\xa6";
        return s;
    };

    struct TabInfo {
        std::string prefix;  // " [N] " or " icon [N] "
        std::string title;   // full title
        std::string text;    // final rendered text (prefix + truncated title + " ")
        int width;
        bool isActive;
        uint32_t bgColor, fgColor;
    };

    // Build tab info with full titles
    std::vector<TabInfo> tabInfos;
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        Tab* tab = tabs_[i].get();
        bool isActive = (i == activeTabIdx_);
        TabInfo ti;
        ti.isActive = isActive;
        ti.bgColor = isActive ? tbActiveBgColor_ : tbInactiveBgColor_;
        ti.fgColor = isActive ? tbActiveFgColor_ : tbInactiveFgColor_;

        ti.prefix = " ";
        std::string pg = tabBarConfig_.progress_icon ? progressGlyph(tab) : "";
        if (!pg.empty()) { ti.prefix += pg; ti.prefix += " "; }
        if (!tab->icon().empty()) { ti.prefix += tab->icon(); ti.prefix += " "; }
        ti.prefix += "[";
        ti.prefix += std::to_string(i + 1);
        ti.prefix += "] ";
        ti.title = tab->title();
        tabInfos.push_back(std::move(ti));
    }

    int numTabs = static_cast<int>(tabInfos.size());
    int sepWidth = 1; // powerline separator per tab
    int availCols = cols;

    // Determine max title length that fits all tabs
    // Start with configured max, shrink until everything fits or titles are gone
    int maxTitleLen = tabBarConfig_.max_title_length > 0 ? tabBarConfig_.max_title_length : 9999;
    for (;;) {
        int total = 0;
        for (auto& ti : tabInfos) {
            std::string truncTitle = truncUtf8(ti.title, maxTitleLen);
            ti.text = ti.prefix + truncTitle + (truncTitle.empty() ? "" : " ");
            if (ti.text.back() != ' ') ti.text += " ";
            ti.width = cpLen(ti.text);
            total += ti.width + sepWidth;
        }
        if (total <= availCols || maxTitleLen <= 0) break;
        maxTitleLen--;
    }

    // Check if we still overflow at minimum (no title text at all)
    int totalWidth = 0;
    for (auto& ti : tabInfos) totalWidth += ti.width + sepWidth;

    // Determine visible tab range if overflow
    int visStart = 0, visEnd = numTabs;
    bool overflowLeft = false, overflowRight = false;
    if (totalWidth > availCols && numTabs > 1) {
        // Start from active tab, expand outward until we fill
        visStart = activeTabIdx_;
        visEnd = activeTabIdx_ + 1;
        int used = tabInfos[activeTabIdx_].width + sepWidth;
        int indicatorWidth = 2; // "< " or " >" indicator

        while (visStart > 0 || visEnd < numTabs) {
            bool expanded = false;
            // Try expanding right
            if (visEnd < numTabs) {
                int need = tabInfos[visEnd].width + sepWidth + (visEnd + 1 < numTabs ? indicatorWidth : 0);
                if (used + need + (visStart > 0 ? indicatorWidth : 0) <= availCols) {
                    used += tabInfos[visEnd].width + sepWidth;
                    visEnd++;
                    expanded = true;
                }
            }
            // Try expanding left
            if (visStart > 0) {
                int need = tabInfos[visStart - 1].width + sepWidth + (visStart - 1 > 0 ? indicatorWidth : 0);
                if (used + need + (visEnd < numTabs ? indicatorWidth : 0) <= availCols) {
                    visStart--;
                    used += tabInfos[visStart].width + sepWidth;
                    expanded = true;
                }
            }
            if (!expanded) break;
        }
        overflowLeft = (visStart > 0);
        overflowRight = (visEnd < numTabs);
    }

    FontData* font = const_cast<FontData*>(textSystem_.getFont(tabBarFontName_));
    if (!font) return;
    float scale = tabBarFontSize_ / font->baseSize;

    auto resolveTabBarGlyph = [&](const ShapedText& shaped) -> const GlyphInfo* {
        if (shaped.glyphs.empty()) return nullptr;
        uint64_t glyphId = shaped.glyphs[0].glyphId;
        if ((glyphId & 0xFFFFFFFF) == 0) return nullptr; // .notdef
        auto it = font->glyphs.find(glyphId);
        if (it == font->glyphs.end() || it->second.is_empty) return nullptr;
        return &it->second;
    };

    auto placeChar = [&](int& col, const std::string& utf8ch, uint32_t fg, uint32_t bg, bool stretchY = false) {
        if (col >= cols) return;
        ResolvedCell& rc = cells[static_cast<size_t>(col)];
        rc.fg_color = fg;
        rc.bg_color = bg;
        // System font fallback happens automatically inside shapeText
        const ShapedText& shaped = textSystem_.shapeText(tabBarFontName_, utf8ch, tabBarFontSize_);
        const GlyphInfo* gi = resolveTabBarGlyph(shaped);

        if (gi) {
            GlyphEntry entry;
            entry.atlas_offset = gi->atlas_offset;
            entry.ext_min_x = gi->ext_min_x;
            entry.ext_min_y = gi->ext_min_y;
            entry.ext_max_x = gi->ext_max_x;
            entry.ext_max_y = gi->ext_max_y;
            entry.upem = gi->upem;
            if (stretchY) {
                entry.upem = gi->upem | 0x80000000u;
            }
            entry.x_offset = 0.0f;
            entry.y_offset = 0.0f;
            rc.glyph_offset = static_cast<uint32_t>(tabBarGlyphs.size());
            rc.glyph_count = 1;
            tabBarGlyphs.push_back(entry);
        }
        col++;
    };

    int col = 0;

    // Left overflow indicator
    if (overflowLeft) {
        placeChar(col, "\xe2\x97\x80", tbInactiveFgColor_, tbBgColor_); // U+25C0 ◀
        placeChar(col, " ", tbInactiveFgColor_, tbBgColor_);
    }

    for (int i = visStart; i < visEnd; ++i) {
        auto& ti = tabInfos[i];
        const char* p = ti.text.c_str();
        while (*p && col < cols) {
            uint8_t b = static_cast<uint8_t>(*p);
            int len = 1;
            if ((b & 0xE0) == 0xC0) len = 2;
            else if ((b & 0xF0) == 0xE0) len = 3;
            else if ((b & 0xF8) == 0xF0) len = 4;
            std::string ch(p, static_cast<size_t>(len));
            placeChar(col, ch, ti.fgColor, ti.bgColor);
            p += len;
        }
        // Powerline separator
        uint32_t nextBg = (i + 1 < visEnd)
            ? tabInfos[i + 1].bgColor : tbBgColor_;
        placeChar(col, SEP_RIGHT, ti.bgColor, nextBg, true);
    }

    // Right overflow indicator
    if (overflowRight && col + 2 <= cols) {
        placeChar(col, " ", tbInactiveFgColor_, tbBgColor_);
        placeChar(col, "\xe2\x96\xb6", tbInactiveFgColor_, tbBgColor_); // U+25B6 ▶
    }

    for (; col < cols; col++) {
        cells[static_cast<size_t>(col)].bg_color = tbBgColor_;
        cells[static_cast<size_t>(col)].fg_color = tbInactiveFgColor_;
    }

    renderer_.updateFontAtlas(queue_, tabBarFontName_, *font);

    ComputeState* cs = renderer_.computePool().acquire(static_cast<uint32_t>(cols));
    uint32_t tbGlyphCount = std::max(static_cast<uint32_t>(tabBarGlyphs.size()), 1u);
    renderer_.computePool().ensureGlyphCapacity(cs, tbGlyphCount);
    renderer_.uploadResolvedCells(queue_, cs, cells.data(), static_cast<uint32_t>(cols));
    if (!tabBarGlyphs.empty())
        renderer_.uploadGlyphs(queue_, cs, tabBarGlyphs.data(), static_cast<uint32_t>(tabBarGlyphs.size()));

    TerminalComputeParams params = {};
    params.cols = static_cast<uint32_t>(cols);
    params.rows = 1;
    params.cell_width = tabBarCharWidth_;
    params.cell_height = tabBarLineHeight_;
    params.viewport_w = static_cast<float>(tbRect.w);
    params.viewport_h = static_cast<float>(tbRect.h);
    params.font_ascender = font->ascender * scale;
    params.font_size = tabBarFontSize_;
    params.pane_origin_x = 0.0f;
    params.pane_origin_y = 0.0f;
    params.max_text_vertices = cs->maxTextVertices;

    PooledTexture* newTexture = texturePool_.acquire(
        static_cast<uint32_t>(tbRect.w),
        static_cast<uint32_t>(std::ceil(tabBarLineHeight_)));

    wgpu::CommandEncoderDescriptor encDesc = {};
    wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
    static constexpr float kNoTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    renderer_.renderToPane(encoder, queue_, tabBarFontName_, params, cs, newTexture->view, kNoTint, {});
    wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);
    pendingComputeRelease_.push_back(cs);

    if (tabBarTexture_) pendingTabBarRelease_.push_back(tabBarTexture_);
    tabBarTexture_ = newTexture;
    tabBarDirty_ = false;
}

// ========================================================================
// Factory function
// ========================================================================

std::unique_ptr<Platform> createPlatform(int argc, char** argv)
{
    return std::make_unique<PlatformDawn>(argc, argv);
}
