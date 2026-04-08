#include "CLIClient.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <glob.h>
#include <getopt.h>
#include <poll.h>

#include <libwebsockets.h>
#include <glaze/glaze.hpp>

// ============================================================================
// State shared between callback and main loop
// ============================================================================

struct CLIState {
    struct lws_context* ctx = nullptr;
    struct lws* wsi = nullptr;

    std::string socketPath;
    std::string command;
    glz::generic::object_t requestJson;
    bool streaming = false;
    bool connected = false;
    bool done = false;
    int exitCode = 0;

    std::string pendingTx;
    bool txPending = false;
    std::string rxBuffer;

    // fd set managed by ADD/DEL/CHANGE_MODE_POLL_FD callbacks
    std::vector<struct pollfd> pollfds;
};

static CLIState* sState = nullptr;
static volatile sig_atomic_t sSigintReceived = 0;

static void onSigint(int) { sSigintReceived = 1; }

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
        std::string buf;
        (void)glz::write_json(st->requestJson, buf);
        st->pendingTx = std::move(buf);
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
                lws_set_timeout(wsi, PENDING_TIMEOUT_CLOSE_SEND, 1);
                return -1;
            }
        }
        break;
    }
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        st->rxBuffer.append(static_cast<const char*>(in), len);
        if (lws_is_final_fragment(wsi)) {
            printf("%s\n", st->rxBuffer.c_str());
            fflush(stdout);
            if (!st->streaming) {
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
        break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLOSED_CLIENT_HTTP: {
        st->wsi = nullptr;
        st->done = true;
        break;
    }

    // Foreign event loop fd management
    case LWS_CALLBACK_ADD_POLL_FD: {
        auto* pa = static_cast<struct lws_pollargs*>(in);
        struct pollfd pfd{pa->fd, static_cast<short>(pa->events), 0};
        st->pollfds.push_back(pfd);
        break;
    }
    case LWS_CALLBACK_DEL_POLL_FD: {
        auto* pa = static_cast<struct lws_pollargs*>(in);
        auto it = std::find_if(st->pollfds.begin(), st->pollfds.end(),
            [pa](const struct pollfd& p) { return p.fd == pa->fd; });
        if (it != st->pollfds.end()) st->pollfds.erase(it);
        break;
    }
    case LWS_CALLBACK_CHANGE_MODE_POLL_FD: {
        auto* pa = static_cast<struct lws_pollargs*>(in);
        for (auto& p : st->pollfds)
            if (p.fd == pa->fd) { p.events = static_cast<short>(pa->events); break; }
        break;
    }
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        break;

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
    if (!explicitPath.empty()) return explicitPath;
    if (!pidStr.empty()) return "/tmp/mb-" + pidStr + ".sock";

    glob_t g;
    int ret = glob("/tmp/mb-*.sock", 0, nullptr, &g);
    if (ret != 0 || g.gl_pathc == 0) {
        globfree(&g);
        fprintf(stderr, "No MasterBandit socket found. Use --pid or --socket.\n");
        return {};
    }
    std::string path = g.gl_pathv[0];
    globfree(&g);
    return path;
}

// ============================================================================
// Usage
// ============================================================================

static void cliUsage()
{
    fprintf(stderr,
        "Usage: mb-cli [options] <command> [args]\n"
        "Options:\n"
        "  --pid <pid>       Connect to specific MasterBandit PID\n"
        "  --socket <path>   Connect to specific socket path\n"
        "Commands:\n"
        "  screenshot [--target <pane|id>] [--cell x,y,w,h]  Take PNG screenshot\n"
        "  key <key> [<mod>...]  Inject key event\n"
        "  logs               Stream log output\n"
        "  stats [id]         Print render stats\n"
        "  inject <text>      Inject text into terminal\n"
        "  action <name>      Dispatch action\n"
    );
}

// ============================================================================
// Entry point
// ============================================================================

int runCLI(int argc, char** argv)
{
    static struct option longOpts[] = {
        {"pid",    required_argument, nullptr, 'p'},
        {"socket", required_argument, nullptr, 's'},
        {"target", required_argument, nullptr, 't'},
        {"cell",   required_argument, nullptr, 'c'},
        {"help",   no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    std::string pidStr, sockPath, target, cellStr;
    int opt;
    while ((opt = getopt_long(argc, argv, "p:s:t:c:h", longOpts, nullptr)) != -1) {
        switch (opt) {
        case 'p': pidStr   = optarg; break;
        case 's': sockPath = optarg; break;
        case 't': target   = optarg; break;
        case 'c': cellStr  = optarg; break;
        case 'h': cliUsage(); return 0;
        default:  cliUsage(); return 1;
        }
    }

    if (optind >= argc) { cliUsage(); return 1; }
    std::string command = argv[optind++];

    glz::generic::object_t reqObj;
    bool streaming = false;

    if (command == "screenshot") {
        reqObj["cmd"] = "screenshot_png";
        if (!target.empty()) reqObj["target"] = target;
        if (!cellStr.empty()) {
            int x=0,y=0,w=0,h=0;
            if (sscanf(cellStr.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
                reqObj["cell"] = glz::generic::object_t{{"x",x},{"y",y},{"w",w},{"h",h}};
        }
    } else if (command == "key") {
        if (optind >= argc) { fprintf(stderr, "key requires a key name\n"); return 1; }
        reqObj["cmd"] = "key";
        reqObj["key"] = argv[optind++];
        glz::generic::array_t mods;
        while (optind < argc) mods.push_back(std::string(argv[optind++]));
        if (!mods.empty()) reqObj["mods"] = mods;
    } else if (command == "inject") {
        if (optind >= argc) { fprintf(stderr, "inject requires text\n"); return 1; }
        reqObj["cmd"] = "inject";
        reqObj["data"] = argv[optind++];
    } else if (command == "action") {
        if (optind >= argc) { fprintf(stderr, "action requires a name\n"); return 1; }
        reqObj["cmd"] = "action";
        reqObj["action"] = argv[optind++];
        glz::generic::array_t args;
        while (optind < argc) args.push_back(std::string(argv[optind++]));
        if (!args.empty()) reqObj["args"] = args;
    } else if (command == "logs") {
        reqObj["cmd"] = "subscribe";
        reqObj["channel"] = "logs";
        streaming = true;
    } else if (command == "stats") {
        reqObj["cmd"] = "stats";
    } else {
        fprintf(stderr, "Unknown command: %s\n", command.c_str());
        cliUsage();
        return 1;
    }

    std::string socketPath = discoverSocket(pidStr, sockPath);
    if (socketPath.empty()) return 1;

    CLIState state;
    state.socketPath = socketPath;
    state.command = command;
    state.requestJson = std::move(reqObj);
    state.streaming = streaming;
    sState = &state;

    // SIGINT handler for logs mode
    if (streaming) {
        struct sigaction sa{};
        sa.sa_handler = onSigint;
        sigaction(SIGINT, &sa, nullptr);
    }

    struct lws_context_creation_info info = {};
    info.options = LWS_SERVER_OPTION_UNIX_SOCK;  // no libuv — we drive with poll()
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = sCliProtocols;
    info.user = &state;

    lws_set_log_level(0, nullptr);
    state.ctx = lws_create_context(&info);
    if (!state.ctx) {
        fprintf(stderr, "Failed to create WebSocket context\n");
        return 1;
    }

    std::string unixAddr = "+" + socketPath;
    struct lws_client_connect_info ccinfo = {};
    ccinfo.context = state.ctx;
    ccinfo.address = unixAddr.c_str();
    ccinfo.port = 0;
    ccinfo.path = "/";
    ccinfo.host = "localhost";
    ccinfo.origin = "localhost";
    ccinfo.protocol = "mb-debug";
    ccinfo.local_protocol_name = "mb-debug";
    ccinfo.ssl_connection = 0;

    struct lws* wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        fprintf(stderr, "Failed to initiate connection to %s\n", socketPath.c_str());
        lws_context_destroy(state.ctx);
        return 1;
    }

    // Event loop — poll() drives lws via ADD/DEL_POLL_FD callbacks
    while (!state.done) {
        if (sSigintReceived) {
            sSigintReceived = 0;
            if (state.streaming && state.wsi) {
                glz::generic::object_t unsub{{"cmd","unsubscribe"},{"channel","logs"}};
                std::string buf;
                (void)glz::write_json(unsub, buf);
                state.pendingTx = std::move(buf);
                state.txPending = true;
                state.done = true;
                lws_callback_on_writable(state.wsi);
            } else {
                break;
            }
        }

        if (state.pollfds.empty()) {
            // No fds yet — service lws to trigger ADD_POLL_FD
            lws_service(state.ctx, 50);
            continue;
        }

        int n = poll(state.pollfds.data(), static_cast<nfds_t>(state.pollfds.size()), 50);
        if (n < 0 && errno == EINTR) continue;

        for (auto& pfd : state.pollfds) {
            if (pfd.revents) {
                struct lws_pollfd lpfd{ pfd.fd, pfd.events, pfd.revents };
                lws_service_fd(state.ctx, &lpfd);
                pfd.revents = 0;
                if (state.done) break;
            }
        }
    }

    lws_context_destroy(state.ctx);
    return state.exitCode;
}
