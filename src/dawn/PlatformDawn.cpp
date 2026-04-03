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

    wgpu::Device device() const { return device_; }
    wgpu::Queue queue() const { return queue_; }
    TexturePool& texturePool() { return texturePool_; }

    const std::string& exeDir() const { return exeDir_; }

    // Called from GLFW callbacks
    void onKey(int key, int scancode, int action, int mods);
    void onChar(unsigned int codepoint);
    void onFramebufferResize(int width, int height);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);

    void renderFrame();
    bool shouldClose() { return glfwWindow_ && glfwWindowShouldClose(glfwWindow_); }

    std::string gridToJson(int id);

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

    Tab* activeTab() {
        if (tabs_.empty() || activeTabIdx_ < 0 || activeTabIdx_ >= static_cast<int>(tabs_.size()))
            return nullptr;
        return tabs_[activeTabIdx_].get();
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
    float charWidth_ = 0.0f;
    float lineHeight_ = 0.0f;
    float contentScaleX_ = 1.0f, contentScaleY_ = 1.0f;
    uint32_t fbWidth_ = 0, fbHeight_ = 0;
    std::string primaryFontPath_;
    FontFallback fontFallback_;
    bool needsRedraw_ = true;
    bool controlPressed_ = false;
    unsigned int lastMods_ = 0;

    // Per-pane render state
    struct PaneRenderState {
        std::vector<ResolvedCell> resolvedCells;
        int lastCursorX = -1, lastCursorY = -1;
        bool lastCursorVisible = true;
        std::unordered_set<char32_t> fallbackAttempted;
        PooledTexture* heldTexture = nullptr;
        bool dirty = true;
        std::vector<PooledTexture*> pendingRelease;
    };
    std::unordered_map<int, PaneRenderState> paneRenderStates_;

    // Multiple PTY poll handles — keyed by master fd
    std::unordered_map<int, uv_poll_t*> ptyPolls_;
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

        if (!hasBoldFont) {
            textSystem_.addSyntheticBoldVariant(fontName_, options.boldStrength, options.boldStrength);
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
    }

    // Create a layout and tab for this terminal
    auto layout = std::make_unique<Layout>();
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
    cbs.onTitleChanged = [this](const std::string& title) {
        glfwSetWindowTitle(glfwWindow_, title.c_str());
    };
    cbs.cellPixelWidth = [this]() -> float { return charWidth_; };
    cbs.cellPixelHeight = [this]() -> float { return lineHeight_; };

    auto terminal = std::make_unique<Terminal>(this, std::move(cbs));

    if (!terminal->init(options)) {
        spdlog::error("Failed to init terminal");
        return nullptr;
    }

    // Compute cols/rows from pane rect
    const PaneRect& pr = pane->rect();
    int cols = (pr.w > 0 && charWidth_ > 0) ? static_cast<int>(pr.w / charWidth_) : static_cast<int>(fbWidth_ / charWidth_);
    int rows = (pr.h > 0 && lineHeight_ > 0) ? static_cast<int>(pr.h / lineHeight_) : static_cast<int>(fbHeight_ / lineHeight_);
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;

    renderer_.resizeComputeBuffers(device_, cols, rows);

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

    pane->setTerminal(std::move(terminal));  // Pane owns the Terminal now

    if (loop_) {
        addPtyPoll(masterFD, termPtr);
    }

    // Build and store tab
    auto tab = std::make_unique<Tab>(std::move(layout));
    tabs_.push_back(std::move(tab));
    // Make first tab active (subsequent calls add more tabs but we keep activeTabIdx_ at 0 for now)
    activeTabIdx_ = 0;

    // Terminal is owned by the Pane/Tab/Layout — return nullptr, not a second owner.
    return nullptr;
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
        });
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

    for (int col = 0; col < cols; ++col) {
        ResolvedCell& rc = rs.resolvedCells[baseIdx + col];
        const Cell& cell = rowData[col];

        if (cell.wc == 0 || cell.attrs.wideSpacer()) {
            rc.atlas_offset = 0;
            rc.ext_min_x = rc.ext_min_y = rc.ext_max_x = rc.ext_max_y = 0;
            rc.upem = 1;
            rc.fg_color = 0xFFFFFFFF;
            rc.bg_color = cell.attrs.packBgAsU32();
            continue;
        }

        std::string cpStr = codepointToUtf8(cell.wc);
        auto resolveGlyph = [&](const ShapedText& shaped) -> const GlyphInfo* {
            if (shaped.glyphs.empty()) return nullptr;
            uint64_t glyphId = shaped.glyphs[0].glyphId;
            if ((glyphId & 0xFFFFFFFF) == 0) return nullptr;
            auto it = font->glyphs.find(glyphId);
            if (it == font->glyphs.end() || it->second.is_empty) return nullptr;
            return &it->second;
        };

        const int boldHint = cell.attrs.bold() ? 1 : 0;
        const ShapedText& shaped = textSystem_.shapeText(fontName_, cpStr, fontSize_, 0, 0, boldHint);
        const GlyphInfo* gi = resolveGlyph(shaped);

        if (!gi && !unicode::isSpace(cell.wc) && rs.fallbackAttempted.find(cell.wc) == rs.fallbackAttempted.end()) {
            rs.fallbackAttempted.insert(cell.wc);
            auto fallbackData = fontFallback_.fontDataForCodepoint(primaryFontPath_, cell.wc);
            if (!fallbackData.empty()) {
                textSystem_.addFallbackFont(fontName_, fallbackData);
                font = const_cast<FontData*>(textSystem_.getFont(fontName_));
                const ShapedText& retry = textSystem_.shapeText(fontName_, cpStr, fontSize_, 0, 0, boldHint);
                gi = resolveGlyph(retry);
                if (!gi) {
                    spdlog::warn("FontFallback: still missing after fallback for U+{:04X}", static_cast<uint32_t>(cell.wc));
                }
            }
        }

        if (!gi) {
            rc.atlas_offset = 0;
            rc.ext_min_x = rc.ext_min_y = rc.ext_max_x = rc.ext_max_y = 0;
            rc.upem = 1;
            rc.fg_color = cell.attrs.packFgAsU32();
            rc.bg_color = cell.attrs.packBgAsU32();
            continue;
        }
        rc.atlas_offset = gi->atlas_offset;
        rc.ext_min_x = gi->ext_min_x;
        rc.ext_min_y = gi->ext_min_y;
        rc.ext_max_x = gi->ext_max_x;
        rc.ext_max_y = gi->ext_max_y;
        rc.upem = gi->upem;
        rc.fg_color = cell.attrs.packFgAsU32();
        rc.bg_color = cell.attrs.packBgAsU32();
    }
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

    for (auto& panePtr : currentTab->layout()->panes()) {
        Pane* pane = panePtr.get();
        if (pane->rect().isEmpty()) continue;
        int paneId = pane->id();
        TerminalEmulator* term = pane->activeTerm();
        if (!term) continue;

        PaneRenderState& rs = paneRenderStates_[paneId];
        const IGrid& g = term->grid();

        int curX = term->cursorX(), curY = term->cursorY();
        int prevCursorY = rs.lastCursorY;
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

            if (term->viewportOffset() != 0) {
                for (int row = 0; row < g.rows(); ++row)
                    resolveRow(paneId, row, font, scale);
            } else {
                for (int row = 0; row < g.rows(); ++row) {
                    if (g.isRowDirty(row) || (cursorMoved && (row == curY || row == prevCursorY)))
                        resolveRow(paneId, row, font, scale);
                }
            }
            const_cast<IGrid&>(g).clearAllDirty();

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

            // Apply cursor
            int cursorIdx = -1;
            ResolvedCell savedCursorCell;
            if (term->viewportOffset() == 0 && term->cursorVisible() &&
                term->cursorX() >= 0 && term->cursorX() < g.cols() &&
                term->cursorY() >= 0 && term->cursorY() < g.rows()) {
                cursorIdx = term->cursorY() * g.cols() + term->cursorX();
                savedCursorCell = rs.resolvedCells[cursorIdx];
                rs.resolvedCells[cursorIdx].bg_color = 0xFFCCCCCC;
                rs.resolvedCells[cursorIdx].fg_color = 0xFF000000;
            }

            uint32_t totalCells = static_cast<uint32_t>(g.cols()) * g.rows();
            renderer_.uploadResolvedCells(queue_, rs.resolvedCells.data(), totalCells);

            // Restore cursor cell
            if (cursorIdx >= 0) {
                rs.resolvedCells[cursorIdx] = savedCursorCell;
            }

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
            params.pane_origin_x = 0.0f;
            params.pane_origin_y = 0.0f;

            PooledTexture* newTexture = texturePool_.acquire(
                static_cast<uint32_t>(pane->rect().w),
                static_cast<uint32_t>(pane->rect().h));

            wgpu::CommandEncoderDescriptor encDesc = {};
            wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
            renderer_.renderToPane(encoder, queue_, fontName_, params, newTexture->view, imageCmds);
            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);

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

    // Composite + submit
    if (!compositeEntries.empty()) {
        wgpu::CommandEncoderDescriptor encDesc = {};
        wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
        renderer_.composite(encoder, surfaceTexture.texture, compositeEntries);

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

        // Collect all pending releases from all panes
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
        TexturePool* pool = &texturePool_;
        queue_.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
            [pool, toRelease](wgpu::QueueWorkDoneStatus, wgpu::StringView) mutable {
                for (auto* t : toRelease) pool->release(t);
                pool->tick();
            });
    }

    surface_.Present();
    needsRedraw_ = false;
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

    // Cmd+V: paste from clipboard
    if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_V) {
        const char* clip = glfwGetClipboardString(glfwWindow_);
        if (clip && clip[0]) {
            static_cast<Terminal*>(term)->pasteText(std::string(clip));
        }
        return;
    }

    // Cmd+C: copy selection
    if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_C) {
        if (term->hasSelection()) {
            std::string text = term->selectedText();
            if (!text.empty()) {
                glfwSetClipboardString(glfwWindow_, text.c_str());
            }
        }
        return;
    }

    Key k = glfwKeyToKey(key);
    spdlog::debug("onKey: mapped key=0x{:x} controlPressed={}", static_cast<int>(k), controlPressed_);

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

void PlatformDawn::onFramebufferResize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    fbWidth_ = static_cast<uint32_t>(width);
    fbHeight_ = static_cast<uint32_t>(height);

    configureSurface(fbWidth_, fbHeight_);
    renderer_.setViewportSize(fbWidth_, fbHeight_);

    Tab* tab = activeTab();
    if (!tab) return;

    tab->layout()->computeRects(fbWidth_, fbHeight_);

    for (auto& panePtr : tab->layout()->panes()) {
        Pane* pane = panePtr.get();
        pane->resizeToRect(charWidth_, lineHeight_);

        TerminalEmulator* term = pane->activeTerm();
        if (!term) continue;

        int cols = term->width();
        int rows = term->height();
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;

        renderer_.resizeComputeBuffers(device_, cols, rows);

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

    needsRedraw_ = true;
}

void PlatformDawn::onMouseButton(int button, int action, int mods)
{
    Tab* tab = activeTab();
    if (!tab) return;
    Pane* fp = tab->hasOverlay() ? nullptr : tab->layout()->focusedPane();
    TerminalEmulator* term = tab->hasOverlay()
        ? static_cast<TerminalEmulator*>(tab->topOverlay())
        : (fp ? fp->activeTerm() : nullptr);
    if (!term) return;

    lastMods_ = glfwModsToModifiers(mods);

    MouseEvent ev;
    double x, y;
    glfwGetCursorPos(glfwWindow_, &x, &y);
    double sx = x * contentScaleX_;
    double sy = y * contentScaleY_;

    // Adjust for pane origin
    PaneRect pr = fp ? fp->rect() : PaneRect{0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_)};
    double relX = sx - pr.x;
    double relY = sy - pr.y;

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
// Factory function
// ========================================================================

std::unique_ptr<Platform> createPlatform(int argc, char** argv)
{
    return std::make_unique<PlatformDawn>(argc, argv);
}
