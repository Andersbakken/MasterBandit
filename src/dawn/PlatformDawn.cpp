#include "Platform.h"
#include "Terminal.h"
#include "Renderer.h"
#include "text.h"
#include "Log.h"
#include "DebugIPC.h"
#include "NativeSurface.h"

#include <glaze/glaze.hpp>

#include <dawn/webgpu_cpp.h>
#include <dawn/native/DawnNative.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <uv.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <sys/ioctl.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// --- Base64 encoding for PNG screenshots ---

static const char sBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out += sBase64Table[(n >> 18) & 0x3F];
        out += sBase64Table[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? sBase64Table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? sBase64Table[n & 0x3F] : '=';
    }
    return out;
}

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
        // For printable ASCII keys, GLFW_KEY_A == 65 == 'A', etc.
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
    // Common monospace font locations
    static const char* candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
#ifdef __APPLE__
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

// ========================================================================
// TerminalWindow class — owns GLFW window, Dawn resources, Renderer, Terminal
// ========================================================================

class PlatformDawn;

class TerminalWindow : public Terminal {
public:
    TerminalWindow(PlatformDawn* platform, GLFWwindow* glfwWindow,
                   wgpu::Device device, wgpu::Queue queue, wgpu::Surface surface);
    ~TerminalWindow();

    // Terminal overrides
    void event(Event ev, void* data = nullptr) override;
    void render(int y, int x, const std::string& str, int idx, int len,
                int cursor, unsigned int flags) override;

    // Called from GLFW callbacks
    void onKey(int key, int scancode, int action, int mods);
    void onChar(unsigned int codepoint);
    void onFramebufferResize(int width, int height);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);

    void renderTerminal();
    bool shouldClose() const { return glfwWindowShouldClose(glfwWindow_); }

    GLFWwindow* glfwWindow() const { return glfwWindow_; }

    void setDebugIPC(DebugIPC* ipc) { debugIPC_ = ipc; }
    DebugIPC* debugIPC() const { return debugIPC_; }

    // Grid screenshot: serialize visible terminal lines to JSON
    std::string gridToJson(int id);

private:
    void configureSurface(uint32_t width, uint32_t height);

    PlatformDawn* platform_;
    GLFWwindow* glfwWindow_;
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Surface surface_;

    Renderer renderer_;
    TextSystem textSystem_;
    std::string fontName_ = "mono";
    float fontSize_ = 16.0f;
    float charWidth_ = 0.0f;
    float lineHeight_ = 0.0f;
    uint32_t fbWidth_ = 0, fbHeight_ = 0;

    // Accumulated render data from Terminal::render() calls
    struct RenderSegment {
        int screenY;     // line index (0-based from viewport top)
        int screenX;     // starting column
        std::string text;
        int cursorCol;   // cursor column within this segment, or -1
        unsigned int flags;
    };
    std::vector<RenderSegment> renderSegments_;

    bool needsRedraw_ = true;
    bool controlPressed_ = false;

    DebugIPC* debugIPC_ = nullptr;
};

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

    TerminalWindow* window() const { return window_; }

private:
    wgpu::Device device_;
    wgpu::Queue queue_;
    std::unique_ptr<dawn::native::Instance> nativeInstance_;
    TerminalWindow* window_ = nullptr;
    int exitStatus_ = 0;
    bool running_ = false;
    uv_loop_t* loop_ = nullptr;
    uv_poll_t ptyPoll_ = {};
    uv_prepare_t prepareCb_ = {};
    uv_timer_t tickTimer_ = {};
    std::string exeDir_;
    std::unique_ptr<DebugIPC> debugIPC_;
    std::shared_ptr<DebugIPCSink> debugSink_;
};

// ========================================================================
// PlatformDawn implementation
// ========================================================================

PlatformDawn::PlatformDawn(int argc, char** argv)
{
    // Set up spdlog with file sink + debug IPC sink
    try {
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("/tmp/mb.log", true);
        debugSink_ = std::make_shared<DebugIPCSink>();
        std::vector<spdlog::sink_ptr> sinks = {fileSink, debugSink_};
        auto logger = std::make_shared<spdlog::logger>("mb", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::debug);
        spdlog::set_default_logger(logger);
    } catch (...) {}

    exeDir_ = fs::weakly_canonical(fs::path(argv[0])).parent_path().string();

    if (!glfwInit()) {
        spdlog::error("glfwInit failed");
        return;
    }

    // Create Dawn instance
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
}

PlatformDawn::~PlatformDawn()
{
    glfwTerminate();
}

std::unique_ptr<Terminal> PlatformDawn::createTerminal(const TerminalOptions& options)
{
    if (!device_) return nullptr;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* glfwWin = glfwCreateWindow(800, 600, "MasterBandit", nullptr, nullptr);
    if (!glfwWin) {
        spdlog::error("glfwCreateWindow failed");
        return nullptr;
    }

    // Create Dawn surface from GLFW window
    wgpu::Instance instance(nativeInstance_->Get());
    wgpu::Surface surface = createNativeSurface(glfwWin, instance);
    if (!surface) {
        spdlog::error("Failed to create Dawn surface");
        glfwDestroyWindow(glfwWin);
        return nullptr;
    }

    auto window = std::make_unique<TerminalWindow>(this, glfwWin, device_, queue_, surface);
    window_ = window.get();

    // Set up GLFW callbacks with window pointer
    glfwSetWindowUserPointer(glfwWin, window_);

    glfwSetKeyCallback(glfwWin, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        static_cast<TerminalWindow*>(glfwGetWindowUserPointer(w))->onKey(key, scancode, action, mods);
    });

    glfwSetCharCallback(glfwWin, [](GLFWwindow* w, unsigned int codepoint) {
        static_cast<TerminalWindow*>(glfwGetWindowUserPointer(w))->onChar(codepoint);
    });

    glfwSetFramebufferSizeCallback(glfwWin, [](GLFWwindow* w, int width, int height) {
        static_cast<TerminalWindow*>(glfwGetWindowUserPointer(w))->onFramebufferResize(width, height);
    });

    glfwSetMouseButtonCallback(glfwWin, [](GLFWwindow* w, int button, int action, int mods) {
        static_cast<TerminalWindow*>(glfwGetWindowUserPointer(w))->onMouseButton(button, action, mods);
    });

    glfwSetCursorPosCallback(glfwWin, [](GLFWwindow* w, double x, double y) {
        static_cast<TerminalWindow*>(glfwGetWindowUserPointer(w))->onCursorPos(x, y);
    });

    if (!window->init(options)) {
        spdlog::error("Failed to init terminal");
        window_ = nullptr;
        return nullptr;
    }

    return window;
}

int PlatformDawn::exec()
{
    if (!window_) return 1;

    running_ = true;
    loop_ = uv_default_loop();

    // Create DebugIPC with grid screenshot callback
    debugIPC_ = std::make_unique<DebugIPC>(loop_, window_,
        [this](int id) { return window_->gridToJson(id); });
    window_->setDebugIPC(debugIPC_.get());
    if (debugSink_) {
        debugSink_->setIPC(debugIPC_.get());
    }

    // Poll PTY master FD for readable data
    uv_poll_init(loop_, &ptyPoll_, window_->masterFD());
    ptyPoll_.data = this;
    uv_poll_start(&ptyPoll_, UV_READABLE, [](uv_poll_t* handle, int status, int events) {
        if (status < 0) return;
        auto* self = static_cast<PlatformDawn*>(handle->data);
        if (events & UV_READABLE) {
            self->window_->readFromFD();
        }
    });

    // Prepare callback: run before each I/O poll — handle GLFW events + render
    uv_prepare_init(loop_, &prepareCb_);
    prepareCb_.data = this;
    uv_prepare_start(&prepareCb_, [](uv_prepare_t* handle) {
        auto* self = static_cast<PlatformDawn*>(handle->data);
        glfwPollEvents();

        if (self->window_->shouldClose()) {
            uv_stop(self->loop_);
            return;
        }

        self->window_->renderTerminal();
    });

    // Timer to keep the event loop cycling so GLFW events get polled.
    // Without this, libuv blocks in the poll phase waiting for PTY data
    // and never calls glfwPollEvents() for keyboard/mouse input.
    uv_timer_init(loop_, &tickTimer_);
    tickTimer_.data = this;
    uv_timer_start(&tickTimer_, [](uv_timer_t*) {
        // no-op — just keeps the loop alive and cycling
    }, 16, 16); // ~60Hz

    uv_run(loop_, UV_RUN_DEFAULT);

    // Cleanup
    if (debugSink_) {
        debugSink_->setIPC(nullptr);
    }
    debugIPC_.reset();

    uv_timer_stop(&tickTimer_);
    uv_poll_stop(&ptyPoll_);
    uv_prepare_stop(&prepareCb_);
    uv_close(reinterpret_cast<uv_handle_t*>(&tickTimer_), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&ptyPoll_), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(&prepareCb_), nullptr);
    uv_run(loop_, UV_RUN_DEFAULT); // drain close callbacks

    return exitStatus_;
}

void PlatformDawn::quit(int status)
{
    exitStatus_ = status;
    if (loop_) {
        uv_stop(loop_);
    }
}

// ========================================================================
// TerminalWindow implementation
// ========================================================================

TerminalWindow::TerminalWindow(PlatformDawn* platform, GLFWwindow* glfwWindow,
               wgpu::Device device, wgpu::Queue queue, wgpu::Surface surface)
    : Terminal(platform)
    , platform_(platform)
    , glfwWindow_(glfwWindow)
    , device_(device)
    , queue_(queue)
    , surface_(surface)
{
    // Get initial framebuffer size
    int w, h;
    glfwGetFramebufferSize(glfwWindow_, &w, &h);
    fbWidth_ = static_cast<uint32_t>(w);
    fbHeight_ = static_cast<uint32_t>(h);

    configureSurface(fbWidth_, fbHeight_);

    // Find and load a monospace font
    std::string fontPath = findMonospaceFont();
    if (fontPath.empty()) {
        spdlog::error("No monospace font found on system");
        return;
    }
    spdlog::info("Using font: {}", fontPath);

    auto fontData = loadFontFile(fontPath);
    if (fontData.empty()) {
        spdlog::error("Failed to load font: {}", fontPath);
        return;
    }

    std::vector<std::vector<uint8_t>> fontList = {std::move(fontData)};
    textSystem_.registerFont(fontName_, fontList, 48.0f, 4.0f, false);

    const FontData* font = textSystem_.getFont(fontName_);
    if (!font) {
        spdlog::error("Failed to register font");
        return;
    }

    // Compute character metrics
    float scale = fontSize_ / font->baseSize;
    lineHeight_ = font->lineHeight * scale;

    // Use 'M' advance for character width (monospace)
    const auto& shaped = textSystem_.shapeText(fontName_, "M", fontSize_);
    charWidth_ = shaped.width;
    if (charWidth_ < 1.0f) charWidth_ = fontSize_ * 0.6f; // fallback

    spdlog::info("Font metrics: charWidth={:.1f}, lineHeight={:.1f}", charWidth_, lineHeight_);

    // Init renderer
    std::string shaderDir = fs::weakly_canonical(
        fs::path(fs::read_symlink("/proc/self/exe")).parent_path() / "shaders").string();
    if (!fs::exists(shaderDir)) {
        // Fallback: try relative to source
        shaderDir = (fs::path(__FILE__).parent_path().parent_path() / "shaders").string();
    }
    renderer_.init(device_, queue_, shaderDir, fbWidth_, fbHeight_);
    renderer_.uploadFontAtlas(queue_, fontName_, *font, false);

    // Compute terminal dimensions in characters
    int cols = static_cast<int>(fbWidth_ / charWidth_);
    int rows = static_cast<int>(fbHeight_ / lineHeight_);
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;
    resize(cols, rows);

    // Set initial PTY window size
    struct winsize ws = {};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_xpixel = static_cast<unsigned short>(fbWidth_);
    ws.ws_ypixel = static_cast<unsigned short>(fbHeight_);
    ioctl(masterFD(), TIOCSWINSZ, &ws);
}

TerminalWindow::~TerminalWindow()
{
    if (glfwWindow_) {
        glfwDestroyWindow(glfwWindow_);
    }
}

void TerminalWindow::configureSurface(uint32_t width, uint32_t height)
{
    wgpu::SurfaceConfiguration config = {};
    config.device = device_;
    config.format = wgpu::TextureFormat::BGRA8Unorm;
    config.width = width;
    config.height = height;
    config.presentMode = wgpu::PresentMode::Fifo;
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    surface_.Configure(&config);
}

void TerminalWindow::event(Event ev, void* /*data*/)
{
    switch (ev) {
    case Update:
    case ScrollbackChanged:
        needsRedraw_ = true;
        break;
    case VisibleBell:
        break;
    }
}

void TerminalWindow::render(int y, int x, const std::string& str, int idx, int len,
                    int cursor, unsigned int flags)
{
    RenderSegment seg;
    seg.screenY = y;
    seg.screenX = x;
    seg.text = str.substr(idx, len);
    seg.cursorCol = cursor;
    seg.flags = flags;
    renderSegments_.push_back(std::move(seg));
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

std::string TerminalWindow::gridToJson(int id)
{
    renderSegments_.clear();
    Terminal::render();

    glz::generic::object_t resp;
    resp["type"] = "screenshot";
    resp["format"] = "grid";
    if (id) resp["id"] = static_cast<double>(id);
    resp["cols"] = static_cast<double>(width());
    resp["rows"] = static_cast<double>(height());
    resp["cursor"] = glz::generic::object_t{
        {"x", static_cast<double>(cursorX())},
        {"y", static_cast<double>(cursorY())}
    };

    glz::generic::array_t lines;
    for (const auto& seg : renderSegments_) {
        glz::generic::object_t line;
        line["y"] = static_cast<double>(seg.screenX);
        line["x"] = static_cast<double>(seg.screenY);
        line["text"] = sanitizeUtf8(seg.text);
        if (seg.flags & Render_Selected) {
            line["selected"] = true;
        }
        lines.emplace_back(std::move(line));
    }
    resp["lines"] = std::move(lines);

    std::string buf;
    (void)glz::write_json(resp, buf);
    return buf;
}

void TerminalWindow::renderTerminal()
{
    bool pngNeeded = debugIPC_ && debugIPC_->pngScreenshotPending();
    if ((!needsRedraw_ && !pngNeeded) || fbWidth_ == 0 || fbHeight_ == 0) return;
    needsRedraw_ = false;

    // Get swapchain texture
    wgpu::SurfaceTexture surfaceTexture;
    surface_.GetCurrentTexture(&surfaceTexture);
    if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        return;
    }
    wgpu::TextureView view = surfaceTexture.texture.CreateView();

    // Collect render data from Terminal
    renderSegments_.clear();
    Terminal::render(); // calls our render() override per line

    // Build text quads from render segments
    renderer_.clearTextQuads();

    const FontData* font = textSystem_.getFont(fontName_);
    if (!font) return;

    float scale = fontSize_ / font->baseSize;

    for (const auto& seg : renderSegments_) {
        if (seg.text.empty()) continue;

        float baseY = (seg.screenY - this->y()) * lineHeight_;
        float baseX = static_cast<float>(seg.screenX) * charWidth_;

        // Render background for selected text
        if (seg.flags & Render_Selected) {
            float selW = seg.text.size() * charWidth_;
            ScreenQuad bgQuad;
            bgQuad.x = baseX;
            bgQuad.y = baseY;
            bgQuad.w = selW;
            bgQuad.h = lineHeight_;
            bgQuad.u0 = font->whiteU;
            bgQuad.v0 = font->whiteV;
            bgQuad.u1 = font->whiteU;
            bgQuad.v1 = font->whiteV;
            bgQuad.tintR = 0.3f;
            bgQuad.tintG = 0.3f;
            bgQuad.tintB = 0.8f;
            bgQuad.tintA = 1.0f;
            renderer_.queueTextQuad(fontName_, bgQuad);
        }

        // Shape the text segment
        const auto& shaped = textSystem_.shapeText(fontName_, seg.text, fontSize_);

        for (const auto& sg : shaped.glyphs) {
            auto it = font->glyphs.find(sg.glyphId);
            if (it == font->glyphs.end()) continue;
            const GlyphInfo& gi = it->second;
            if (gi.width < 0.5f || gi.height < 0.5f) continue;

            float glyphX = baseX + sg.x + (gi.bearingX - font->pxRange) * scale;
            float glyphY = baseY + font->ascender * scale + sg.y - (gi.bearingY + font->pxRange) * scale;
            float glyphW = gi.width * scale;
            float glyphH = gi.height * scale;

            ScreenQuad quad;
            quad.x = glyphX;
            quad.y = glyphY;
            quad.w = glyphW;
            quad.h = glyphH;
            quad.u0 = gi.u0;
            quad.v0 = gi.v0;
            quad.u1 = gi.u1;
            quad.v1 = gi.v1;
            quad.tintR = 1.0f;
            quad.tintG = 1.0f;
            quad.tintB = 1.0f;
            quad.tintA = 1.0f;
            renderer_.queueTextQuad(fontName_, quad);
        }

        // Render cursor
        if (seg.cursorCol >= 0) {
            float curX = static_cast<float>(seg.cursorCol) * charWidth_;
            ScreenQuad cursorQuad;
            cursorQuad.x = curX;
            cursorQuad.y = baseY;
            cursorQuad.w = charWidth_;
            cursorQuad.h = lineHeight_;
            cursorQuad.u0 = font->whiteU;
            cursorQuad.v0 = font->whiteV;
            cursorQuad.u1 = font->whiteU;
            cursorQuad.v1 = font->whiteV;
            cursorQuad.tintR = 0.8f;
            cursorQuad.tintG = 0.8f;
            cursorQuad.tintB = 0.8f;
            cursorQuad.tintA = 0.5f;
            renderer_.queueTextQuad(fontName_, cursorQuad);
        }
    }

    // Submit GPU work
    wgpu::CommandEncoderDescriptor encDesc = {};
    wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);

    renderer_.renderFrame(encoder, queue_, view);

    // PNG screenshot readback: copy texture to buffer before Present()
    if (debugIPC_ && debugIPC_->pngScreenshotPending()) {
        uint32_t bytesPerRow = ((fbWidth_ * 4 + 255) / 256) * 256; // align to 256
        uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * fbHeight_;

        wgpu::BufferDescriptor bufDesc = {};
        bufDesc.size = bufferSize;
        bufDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        wgpu::Buffer readbackBuf = device_.CreateBuffer(&bufDesc);

        wgpu::TexelCopyTextureInfo src = {};
        src.texture = surfaceTexture.texture;
        src.mipLevel = 0;
        src.origin = {0, 0, 0};

        wgpu::TexelCopyBufferInfo dst = {};
        dst.buffer = readbackBuf;
        dst.layout.offset = 0;
        dst.layout.bytesPerRow = bytesPerRow;
        dst.layout.rowsPerImage = fbHeight_;

        wgpu::Extent3D extent = {fbWidth_, fbHeight_, 1};
        encoder.CopyTextureToBuffer(&src, &dst, &extent);

        wgpu::CommandBuffer cmds = encoder.Finish();
        queue_.Submit(1, &cmds);

        int pngId = debugIPC_->pngScreenshotId();
        DebugIPC* ipc = debugIPC_;
        uint32_t w = fbWidth_;
        uint32_t h = fbHeight_;
        debugIPC_->clearPngPending();

        readbackBuf.MapAsync(wgpu::MapMode::Read, 0, bufferSize,
            wgpu::CallbackMode::AllowProcessEvents,
            [readbackBuf, ipc, w, h, bytesPerRow, pngId](wgpu::MapAsyncStatus status, wgpu::StringView) mutable {
                if (status != wgpu::MapAsyncStatus::Success) {
                    spdlog::error("DebugIPC: MapAsync failed");
                    return;
                }
                const uint8_t* mapped = static_cast<const uint8_t*>(
                    readbackBuf.GetConstMappedRange(0, static_cast<size_t>(bytesPerRow) * h));
                if (!mapped) {
                    readbackBuf.Unmap();
                    return;
                }

                // Swizzle BGRA → RGBA (packed, no row padding)
                std::vector<uint8_t> rgba(w * h * 4);
                for (uint32_t row = 0; row < h; ++row) {
                    const uint8_t* src = mapped + row * bytesPerRow;
                    uint8_t* dst = rgba.data() + row * w * 4;
                    for (uint32_t col = 0; col < w; ++col) {
                        dst[col * 4 + 0] = src[col * 4 + 2]; // R←B
                        dst[col * 4 + 1] = src[col * 4 + 1]; // G
                        dst[col * 4 + 2] = src[col * 4 + 0]; // B←R
                        dst[col * 4 + 3] = src[col * 4 + 3]; // A
                    }
                }
                readbackBuf.Unmap();

                // Encode PNG to memory
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

                std::string b64 = base64Encode(pngData.data(), pngData.size());
                ipc->onPngReady(b64);
            });
    } else {
        wgpu::CommandBuffer commands = encoder.Finish();
        queue_.Submit(1, &commands);
    }

    surface_.Present();
}

void TerminalWindow::onKey(int key, int /*scancode*/, int action, int mods)
{
    spdlog::debug("onKey: key={} action={} mods={}", key, action, mods);

    if (action == GLFW_RELEASE) {
        controlPressed_ = (mods & GLFW_MOD_CONTROL) != 0;
        return;
    }

    controlPressed_ = (mods & GLFW_MOD_CONTROL) != 0;

    Key k = glfwKeyToKey(key);
    spdlog::debug("onKey: mapped key=0x{:x} controlPressed={}", static_cast<int>(k), controlPressed_);

    // For control+letter combinations, generate the control character directly
    if (controlPressed_ && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        KeyEvent ev;
        ev.key = k;
        ev.text = std::string(1, static_cast<char>(key - GLFW_KEY_A + 1));
        ev.count = 1;
        ev.autoRepeat = (action == GLFW_REPEAT);
        spdlog::debug("onKey: ctrl+letter, sending text=0x{:02x}", static_cast<unsigned char>(ev.text[0]));
        keyPressEvent(&ev);
        return;
    }

    // Non-printable keys — send with empty text, Terminal::keyPressEvent handles them
    if (k != Key_unknown && (key < GLFW_KEY_SPACE || key > GLFW_KEY_GRAVE_ACCENT)) {
        KeyEvent ev;
        ev.key = k;
        ev.count = 1;
        ev.autoRepeat = (action == GLFW_REPEAT);
        spdlog::debug("onKey: non-printable key=0x{:x}, dispatching", static_cast<int>(k));
        keyPressEvent(&ev);
    } else {
        spdlog::debug("onKey: printable key={}, deferring to onChar", key);
    }
}

void TerminalWindow::onChar(unsigned int codepoint)
{
    spdlog::debug("onChar: codepoint=U+{:04X} controlPressed={}", codepoint, controlPressed_);
    if (controlPressed_) return; // already handled in onKey

    KeyEvent ev;
    ev.key = Key_unknown;
    ev.text = codepointToUtf8(codepoint);
    spdlog::debug("onChar: sending text='{}' ({} bytes)", ev.text, ev.text.size());
    ev.count = 1;
    ev.autoRepeat = false;
    keyPressEvent(&ev);
}

void TerminalWindow::onFramebufferResize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    fbWidth_ = static_cast<uint32_t>(width);
    fbHeight_ = static_cast<uint32_t>(height);

    configureSurface(fbWidth_, fbHeight_);
    renderer_.setViewportSize(fbWidth_, fbHeight_);

    // Recompute terminal dimensions
    int cols = static_cast<int>(fbWidth_ / charWidth_);
    int rows = static_cast<int>(fbHeight_ / lineHeight_);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    resize(cols, rows);

    // Update PTY size
    struct winsize ws;
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_xpixel = static_cast<unsigned short>(fbWidth_);
    ws.ws_ypixel = static_cast<unsigned short>(fbHeight_);
    ioctl(masterFD(), TIOCSWINSZ, &ws);

    needsRedraw_ = true;
}

void TerminalWindow::onMouseButton(int button, int action, int /*mods*/)
{
    MouseEvent ev;
    double x, y;
    glfwGetCursorPos(glfwWindow_, &x, &y);
    ev.x = static_cast<int>(x / charWidth_);
    ev.y = static_cast<int>(y / lineHeight_);
    ev.globalX = static_cast<int>(x);
    ev.globalY = static_cast<int>(y);

    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:  ev.button = LeftButton; break;
    case GLFW_MOUSE_BUTTON_MIDDLE: ev.button = MidButton; break;
    case GLFW_MOUSE_BUTTON_RIGHT: ev.button = RightButton; break;
    default: ev.button = NoButton; break;
    }
    ev.buttons = ev.button;

    if (action == GLFW_PRESS) mousePressEvent(&ev);
    else mouseReleaseEvent(&ev);
}

void TerminalWindow::onCursorPos(double x, double y)
{
    MouseEvent ev;
    ev.x = static_cast<int>(x / charWidth_);
    ev.y = static_cast<int>(y / lineHeight_);
    ev.globalX = static_cast<int>(x);
    ev.globalY = static_cast<int>(y);
    ev.button = NoButton;
    // Get current button state
    if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
        ev.buttons |= LeftButton;
    if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
        ev.buttons |= MidButton;
    if (glfwGetMouseButton(glfwWindow_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
        ev.buttons |= RightButton;
    mouseMoveEvent(&ev);
}

// ========================================================================
// Factory function
// ========================================================================

std::unique_ptr<Platform> createPlatform(int argc, char** argv)
{
    return std::make_unique<PlatformDawn>(argc, argv);
}
