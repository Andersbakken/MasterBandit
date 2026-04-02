#include "CLIClient.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <glob.h>
#include <getopt.h>

#include <uv.h>
#include <libwebsockets.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// State shared between callback and main loop
// ============================================================================

struct CLIState {
    uv_loop_t loop;
    struct lws_context* ctx = nullptr;
    struct lws* wsi = nullptr;

    std::string socketPath;
    std::string command;        // "screenshot", "key", "logs"
    json requestJson;           // the JSON to send on connect
    bool streaming = false;     // true for logs mode
    bool connected = false;
    bool done = false;
    int exitCode = 0;

    // TX buffer (filled before writable callback)
    std::string pendingTx;
    bool txPending = false;

    // RX reassembly
    std::string rxBuffer;
};

static CLIState* sState = nullptr;

// ============================================================================
// Signal handling for logs mode
// ============================================================================

static uv_signal_t sSigint;

static void onSigint(uv_signal_t* handle, int /*signum*/)
{
    auto* st = static_cast<CLIState*>(handle->data);
    if (st->streaming && st->wsi) {
        // Send unsubscribe, then close
        json unsub;
        unsub["cmd"] = "unsubscribe";
        unsub["channel"] = "logs";
        st->pendingTx = unsub.dump();
        st->txPending = true;
        st->done = true;
        lws_callback_on_writable(st->wsi);
    } else {
        st->done = true;
        uv_stop(&st->loop);
    }
}

// ============================================================================
// lws client callback
// ============================================================================

static int cliWsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                         void* /*user*/, void* in, size_t len)
{
    struct lws_context* ctx = lws_get_context(wsi);
    CLIState* st = static_cast<CLIState*>(lws_context_user(ctx));
    if (!st) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED: {
        st->connected = true;
        st->wsi = wsi;
        // Queue the request for sending
        st->pendingTx = st->requestJson.dump();
        st->txPending = true;
        lws_callback_on_writable(wsi);
        break;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (st->txPending && !st->pendingTx.empty()) {
            std::vector<unsigned char> buf(LWS_PRE + st->pendingTx.size());
            memcpy(buf.data() + LWS_PRE, st->pendingTx.data(), st->pendingTx.size());
            lws_write(wsi, buf.data() + LWS_PRE, st->pendingTx.size(), LWS_WRITE_TEXT);
            st->pendingTx.clear();
            st->txPending = false;

            if (st->done) {
                // We just sent unsubscribe; close now
                lws_set_timeout(wsi, PENDING_TIMEOUT_CLOSE_SEND, 1);
                return -1;
            }
        }
        break;
    }
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        st->rxBuffer.append(static_cast<const char*>(in), len);
        if (lws_is_final_fragment(wsi)) {
            // Print the response
            printf("%s\n", st->rxBuffer.c_str());
            fflush(stdout);

            if (!st->streaming) {
                // One-shot: we got our response, done
                st->done = true;
                return -1;
            }
            st->rxBuffer.clear();
        }
        break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        const char* msg = in ? static_cast<const char*>(in) : "unknown error";
        fprintf(stderr, "Connection error: %s\n", msg);
        st->exitCode = 1;
        st->done = true;
        uv_stop(&st->loop);
        break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLOSED_CLIENT_HTTP: {
        st->wsi = nullptr;
        if (!st->done) {
            st->done = true;
        }
        uv_stop(&st->loop);
        break;
    }
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols sCliProtocols[] = {
    { "mb-debug", cliWsCallback, 0, 65536 },
    LWS_PROTOCOL_LIST_TERM
};

// ============================================================================
// Socket discovery
// ============================================================================

static std::string discoverSocket(const std::string& pidStr, const std::string& explicitPath)
{
    if (!explicitPath.empty()) {
        return explicitPath;
    }
    if (!pidStr.empty()) {
        return "/tmp/mb-" + pidStr + ".sock";
    }

    // Glob for /tmp/mb-*.sock
    glob_t g;
    int ret = glob("/tmp/mb-*.sock", 0, nullptr, &g);
    if (ret != 0 || g.gl_pathc == 0) {
        fprintf(stderr, "No mb sockets found in /tmp/\n");
        if (ret == 0) globfree(&g);
        return {};
    }
    if (g.gl_pathc > 1) {
        fprintf(stderr, "Multiple mb sockets found; use --pid or --sock to disambiguate:\n");
        for (size_t i = 0; i < g.gl_pathc; i++) {
            fprintf(stderr, "  %s\n", g.gl_pathv[i]);
        }
        globfree(&g);
        return {};
    }
    std::string path = g.gl_pathv[0];
    globfree(&g);
    return path;
}

// ============================================================================
// Arg parsing helpers
// ============================================================================

static void cliUsage()
{
    fprintf(stderr,
        "Usage: mb --ctl <command> [options]\n"
        "\n"
        "Commands:\n"
        "  screenshot [--format grid|png]   Capture terminal (default: grid)\n"
        "  key --text <text>                Send text input\n"
        "  key --key <name> [--mod <mod>]   Send a named key with optional modifier\n"
        "  logs                             Stream log messages (Ctrl+C to stop)\n"
        "\n"
        "Socket selection:\n"
        "  --pid <pid>    Connect to /tmp/mb-<pid>.sock\n"
        "  --sock <path>  Connect to an explicit socket path\n"
    );
}

// ============================================================================
// runCLI
// ============================================================================

int runCLI(int argc, char** argv)
{
    // We expect argv to contain the full program args.
    // Find the position of "--ctl" and parse from there.

    int ctlIdx = -1;
    std::string pidStr;
    std::string sockPath;

    // First pass: extract --pid, --sock, find --ctl position
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctl") == 0) {
            ctlIdx = i;
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pidStr = argv[++i];
        } else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            sockPath = argv[++i];
        }
    }

    if (ctlIdx < 0 || ctlIdx + 1 >= argc) {
        cliUsage();
        return 1;
    }

    std::string command = argv[ctlIdx + 1];
    json requestJson;
    bool streaming = false;

    if (command == "screenshot") {
        requestJson["cmd"] = "screenshot";
        // Look for --format after the command
        for (int i = ctlIdx + 2; i < argc; i++) {
            if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
                requestJson["format"] = argv[++i];
            }
        }
    } else if (command == "key") {
        requestJson["cmd"] = "key";
        std::vector<std::string> mods;
        for (int i = ctlIdx + 2; i < argc; i++) {
            if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
                // Process escape sequences in the text
                std::string raw = argv[++i];
                std::string processed;
                for (size_t j = 0; j < raw.size(); j++) {
                    if (raw[j] == '\\' && j + 1 < raw.size()) {
                        switch (raw[j + 1]) {
                        case 'n': processed += '\n'; j++; break;
                        case 't': processed += '\t'; j++; break;
                        case '\\': processed += '\\'; j++; break;
                        default: processed += raw[j]; break;
                        }
                    } else {
                        processed += raw[j];
                    }
                }
                requestJson["text"] = processed;
            } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
                requestJson["key"] = argv[++i];
            } else if (strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
                mods.push_back(argv[++i]);
            }
        }
        if (!mods.empty()) {
            requestJson["mods"] = mods;
        }
    } else if (command == "logs") {
        requestJson["cmd"] = "subscribe";
        requestJson["channel"] = "logs";
        streaming = true;
    } else {
        fprintf(stderr, "Unknown command: %s\n", command.c_str());
        cliUsage();
        return 1;
    }

    // Discover socket
    std::string socketPath = discoverSocket(pidStr, sockPath);
    if (socketPath.empty()) {
        return 1;
    }

    // Set up libuv loop and lws client context
    CLIState state;
    state.socketPath = socketPath;
    state.command = command;
    state.requestJson = requestJson;
    state.streaming = streaming;
    sState = &state;

    uv_loop_init(&state.loop);

    // Set up SIGINT handler for logs mode
    if (streaming) {
        uv_signal_init(&state.loop, &sSigint);
        sSigint.data = &state;
        uv_signal_start(&sSigint, onSigint, SIGINT);
    }

    // Create lws context with libuv foreign loop
    uv_loop_t* loopPtr = &state.loop;
    struct lws_context_creation_info info = {};
    info.options = LWS_SERVER_OPTION_LIBUV;
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = sCliProtocols;
    info.user = &state;
    info.foreign_loops = reinterpret_cast<void**>(&loopPtr);

    state.ctx = lws_create_context(&info);
    if (!state.ctx) {
        fprintf(stderr, "Failed to create WebSocket context\n");
        uv_loop_close(&state.loop);
        return 1;
    }

    // Connect to the Unix socket
    struct lws_client_connect_info ccinfo = {};
    ccinfo.context = state.ctx;
    ccinfo.address = socketPath.c_str();
    ccinfo.port = 0;
    ccinfo.path = "/";
    ccinfo.host = "localhost";
    ccinfo.origin = "localhost";
    ccinfo.protocol = "mb-debug";
    ccinfo.local_protocol_name = "mb-debug";
    ccinfo.ssl_connection = 0;

    // Use Unix socket transport
    ccinfo.iface = socketPath.c_str();

    struct lws* wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        fprintf(stderr, "Failed to initiate connection to %s\n", socketPath.c_str());
        lws_context_destroy(state.ctx);
        uv_loop_close(&state.loop);
        return 1;
    }

    // Run the event loop
    uv_run(&state.loop, UV_RUN_DEFAULT);

    // Cleanup
    lws_context_destroy(state.ctx);
    if (streaming) {
        uv_signal_stop(&sSigint);
        uv_close(reinterpret_cast<uv_handle_t*>(&sSigint), nullptr);
        uv_run(&state.loop, UV_RUN_NOWAIT);
    }
    uv_loop_close(&state.loop);

    return state.exitCode;
}
