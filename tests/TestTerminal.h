#pragma once
#include "TerminalEmulator.h"
#include <string>

// Lightweight wrapper around TerminalEmulator for use in unit tests.
// No PTY, no GPU — feed bytes in, inspect the cell grid.
struct TestTerminal {

    // TerminalEmulator subclass that captures writeToOutput and exposes
    // protected members we want to assert on in tests.
    struct InnerTerminal : public TerminalEmulator {
        std::string capturedOutput;

        InnerTerminal(TerminalCallbacks cb)
            : TerminalEmulator(std::move(cb)) {}

        void writeToOutput(const char* data, size_t len) override
        {
            capturedOutput.append(data, len);
        }

        // Expose protected accessors as public for tests.
        using TerminalEmulator::bracketedPaste;
        using TerminalEmulator::resetScrollback;
        using TerminalEmulator::callbacks;
    };

    // Declared before `term` so they are initialized first and can be
    // safely captured by the callback lambdas passed to term's constructor.
    std::string capturedTitle;
    bool        capturedTitleHasValue = false;
    std::string capturedIcon;
    bool        capturedIconHasValue  = false;
    std::string capturedCWD;
    std::string capturedNotifyTitle;
    std::string capturedNotifyBody;
    std::string capturedNotifyId;
    uint8_t     capturedNotifyUrgency { 1 };
    bool        capturedNotifyCloseResponse { false };
    bool        capturedNotifyActionFocus  { true };
    bool        capturedNotifyActionReport { false };
    std::vector<std::string> capturedNotifyButtons;
    std::string capturedCloseId;            // last p=close target id
    std::string capturedAliveResponderId;   // last p=alive responder id
    int         closeNotificationCalls { 0 };
    int         queryAliveCalls { 0 };
    std::string capturedPointerShape;
    int         pointerShapeCallCount = 0;
    // OSC 52: text sent to copyToClipboard; clipboardContent is what
    // pasteFromClipboard returns when queried.
    std::string capturedClipboard;
    std::string clipboardContent;
    // OSC 9;4 progress: last (state, pct) delivered; callCount tracks invocations.
    int         capturedProgressState = -1;
    int         capturedProgressPct   = -1;
    int         progressCallCount     = 0;

    InnerTerminal term;

    TestTerminal(int cols = 80, int rows = 24)
        : term([this]() {
            TerminalCallbacks cb;
            cb.onTitleChanged = [this](std::optional<std::string> t) {
                capturedTitle = t.value_or(std::string{});
                capturedTitleHasValue = t.has_value();
            };
            cb.onIconChanged  = [this](std::optional<std::string> i) {
                capturedIcon = i.value_or(std::string{});
                capturedIconHasValue = i.has_value();
            };
            cb.onCWDChanged   = [this](const std::string& d) { capturedCWD = d; };
            cb.onDesktopNotification = [this](const TerminalCallbacks::DesktopNotification& n) {
                capturedNotifyTitle = n.title;
                capturedNotifyBody = n.body;
                capturedNotifyId = n.id;
                capturedNotifyUrgency = n.urgency;
                capturedNotifyCloseResponse = n.closeResponseRequested;
                capturedNotifyActionFocus  = n.actionFocus;
                capturedNotifyActionReport = n.actionReport;
                capturedNotifyButtons = n.buttons;
            };
            cb.onCloseNotification = [this](const std::string& id) {
                capturedCloseId = id;
                ++closeNotificationCalls;
            };
            cb.onQueryAliveNotifications = [this](const std::string& responderId) {
                capturedAliveResponderId = responderId;
                ++queryAliveCalls;
            };
            cb.onMouseCursorShape = [this](const std::string& s) {
                capturedPointerShape = s;
                ++pointerShapeCallCount;
            };
            cb.copyToClipboard = [this](const std::string& text) {
                capturedClipboard = text;
            };
            cb.pasteFromClipboard = [this]() {
                return clipboardContent;
            };
            cb.onProgressChanged = [this](int state, int pct) {
                capturedProgressState = state;
                capturedProgressPct = pct;
                ++progressCallCount;
            };
            return cb;
          }())
    {
        term.resize(cols, rows);
        term.resetScrollback(4096);
    }

    void feed(const std::string& s)
    {
        term.injectData(s.data(), s.size());
        // Drain anything that injectData buffered for a DEC mode 2026
        // sync block. Production parses across multiple PTY reads and
        // flushes when 2026l arrives; tests usually call feed() once
        // and then inspect state, so we flush eagerly to keep the
        // test mental model 1:1 with what callers expect.
        term.flushPendingActions();
    }

    void esc(const std::string& s) { feed("\x1b" + s); }
    void csi(const std::string& s) { feed("\x1b[" + s); }
    void osc(const std::string& s) { feed("\x1b]" + s + "\x07"); } // BEL-terminated
    void dcs(const std::string& s) { feed("\x1bP" + s + "\x1b\\"); } // ST-terminated

    const Cell& cell(int col, int row) const
    {
        return term.grid().cell(col, row);
    }

    char32_t wc(int col, int row) const
    {
        return term.grid().cell(col, row).wc;
    }

    const CellAttrs& attrs(int col, int row) const
    {
        return term.grid().cell(col, row).attrs;
    }

    // Returns visible text of a row, trimmed of trailing spaces/nulls.
    std::string rowText(int row) const
    {
        std::string result;
        for (int col = 0; col < term.width(); ++col) {
            char32_t cp = term.grid().cell(col, row).wc;
            if (cp == 0) cp = ' ';
            if (cp < 0x80) {
                result += static_cast<char>(cp);
            } else if (cp < 0x800) {
                result += static_cast<char>(0xC0 | (cp >> 6));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                result += static_cast<char>(0xE0 | (cp >> 12));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                result += static_cast<char>(0xF0 | (cp >> 18));
                result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        auto end = result.find_last_not_of(' ');
        return end == std::string::npos ? "" : result.substr(0, end + 1);
    }

    void clearOutput() { term.capturedOutput.clear(); }
    const std::string& output() const { return term.capturedOutput; }

    void sendKey(Key key, uint32_t mods = 0,
                 KeyAction action = KeyAction_Press,
                 const std::string& text = "",
                 uint32_t shiftedKey = 0)
    {
        KeyEvent ev;
        ev.key = key;
        ev.modifiers = mods;
        ev.action = action;
        ev.text = text;
        ev.shiftedKey = shiftedKey;
        ev.count = 1;
        ev.autoRepeat = (action == KeyAction_Repeat);
        term.keyPressEvent(&ev);
    }
};
