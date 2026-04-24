#include "CLIClient.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <glob.h>
#include <poll.h>
#include <limits.h>

#include <cxxopts.hpp>
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
// Entry point
// ============================================================================

int runCLI(int argc, char** argv)
{
    cxxopts::Options opts("mb --ctl", "MasterBandit IPC client");
    opts.add_options()
        ("ctl", "IPC client mode")
        ("p,pid", "Connect to specific MasterBandit PID", cxxopts::value<std::string>()->default_value(""))
        ("s,socket", "Connect to specific socket path", cxxopts::value<std::string>()->default_value(""))
        ("t,target", "Screenshot target (pane name or id)", cxxopts::value<std::string>()->default_value(""))
        ("c,cell", "Screenshot cell rect: x,y,w,h", cxxopts::value<std::string>()->default_value(""))
        ("f,format", "Screenshot format: png | grid", cxxopts::value<std::string>()->default_value("png"))
        ("h,help", "Print usage")
        ("command", "Command to run", cxxopts::value<std::string>())
        ("args", "Command arguments", cxxopts::value<std::vector<std::string>>()->default_value(""));
    opts.parse_positional({"command", "args"});
    opts.positional_help("<command> [args...]");

    cxxopts::ParseResult result;
    try {
        result = opts.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception& e) {
        fprintf(stderr, "%s\n%s\n", e.what(), opts.help().c_str());
        return 1;
    }

    if (result.count("help") || !result.count("command")) {
        fprintf(stderr, "%s\nCommands:\n"
            "  screenshot [--format png|grid] [--target <pane|id>] [--cell x,y,w,h]\n"
            "                                    Screenshot; --format grid dumps cells\n"
            "  key <key> [<mod>...]              Inject key event\n"
            "  logs                              Stream log output\n"
            "  stats                             Print render stats + observability counters\n"
            "  inject <text>                     Inject text into terminal\n"
            "  feed <path>                       Inject contents of a file into the active terminal\n"
            "  wait-idle [<timeout_ms> [<settle_ms>]]  Block until parser quiet + frame drawn\n"
            "  action <name> [args]              Dispatch action\n",
            opts.help().c_str());
        return result.count("help") ? 0 : 1;
    }

    std::string command = result["command"].as<std::string>();
    auto positionalArgs = result["args"].as<std::vector<std::string>>();
    std::string pidStr = result["pid"].as<std::string>();
    std::string sockPath = result["socket"].as<std::string>();
    std::string target = result["target"].as<std::string>();
    std::string cellStr = result["cell"].as<std::string>();
    std::string format = result["format"].as<std::string>();

    glz::generic::object_t reqObj;
    bool streaming = false;

    if (command == "screenshot") {
        // The server accepts `{cmd: "screenshot", format: "grid"|"png"}`.
        // Keep defaulting to PNG for scripts that ran the old CLI; the
        // user picks "grid" with --format grid for a cell dump.
        reqObj["cmd"] = "screenshot";
        reqObj["format"] = format;
        if (!target.empty()) reqObj["target"] = target;
        if (!cellStr.empty()) {
            int x=0,y=0,w=0,h=0;
            if (sscanf(cellStr.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
                reqObj["cell"] = glz::generic::object_t{{"x",x},{"y",y},{"w",w},{"h",h}};
        }
    } else if (command == "key") {
        if (positionalArgs.empty()) { fprintf(stderr, "key requires a key name\n"); return 1; }
        reqObj["cmd"] = "key";
        reqObj["key"] = positionalArgs[0];
        glz::generic::array_t mods;
        for (size_t i = 1; i < positionalArgs.size(); ++i)
            mods.push_back(positionalArgs[i]);
        if (!mods.empty()) reqObj["mods"] = mods;
    } else if (command == "inject") {
        if (positionalArgs.empty()) { fprintf(stderr, "inject requires text\n"); return 1; }
        reqObj["cmd"] = "inject";
        reqObj["data"] = positionalArgs[0];
    } else if (command == "action") {
        if (positionalArgs.empty()) { fprintf(stderr, "action requires a name\n"); return 1; }
        reqObj["cmd"] = "action";
        reqObj["action"] = positionalArgs[0];
        glz::generic::array_t args;
        for (size_t i = 1; i < positionalArgs.size(); ++i)
            args.push_back(positionalArgs[i]);
        if (!args.empty()) reqObj["args"] = args;
    } else if (command == "logs") {
        reqObj["cmd"] = "subscribe";
        reqObj["channel"] = "logs";
        streaming = true;
    } else if (command == "stats") {
        reqObj["cmd"] = "stats";
    } else if (command == "feed") {
        if (positionalArgs.empty() || positionalArgs[0].empty()) {
            fprintf(stderr, "feed requires a path\n"); return 1;
        }
        char absPath[PATH_MAX];
        const char* p = realpath(positionalArgs[0].c_str(), absPath);
        if (!p) {
            fprintf(stderr, "feed: cannot resolve path: %s\n", strerror(errno));
            return 1;
        }
        reqObj["cmd"] = "feed";
        reqObj["path"] = std::string(absPath);
        if (positionalArgs.size() >= 2 && !positionalArgs[1].empty()) {
            try { reqObj["repeat"] = std::stod(positionalArgs[1]); }
            catch (const std::exception&) {
                fprintf(stderr, "feed: repeat must be a number\n"); return 1;
            }
        }
    } else if (command == "wait-idle") {
        reqObj["cmd"] = "wait-idle";
        auto parseNum = [](const std::string& s, double& out) -> bool {
            if (s.empty()) return false;
            try { out = std::stod(s); return true; }
            catch (const std::exception&) { return false; }
        };
        if (positionalArgs.size() >= 1) {
            double v = 0;
            if (parseNum(positionalArgs[0], v)) reqObj["timeout_ms"] = v;
        }
        if (positionalArgs.size() >= 2) {
            double v = 0;
            if (parseNum(positionalArgs[1], v)) reqObj["settle_ms"] = v;
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", command.c_str());
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

        for (size_t i = 0; i < state.pollfds.size(); ++i) {
            if (state.pollfds[i].revents) {
                struct lws_pollfd lpfd{ state.pollfds[i].fd, state.pollfds[i].events, state.pollfds[i].revents };
                // lws_service_fd may trigger ADD/DEL_POLL_FD callbacks that mutate pollfds
                lws_service_fd(state.ctx, &lpfd);
                if (i < state.pollfds.size())
                    state.pollfds[i].revents = 0;
                if (state.done) break;
            }
        }
    }

    lws_context_destroy(state.ctx);
    return state.exitCode;
}
