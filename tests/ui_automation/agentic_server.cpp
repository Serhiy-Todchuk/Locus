// S5.Z task 9 -- TCP/JSON interactive driver for agentic testing.

#ifndef NOMINMAX
#define NOMINMAX
#endif

// IMPORTANT: include winsock2.h BEFORE anything that drags in windows.h.
// uia_session.h pulls windows.h via its own includes; if windows.h comes
// first it includes the old winsock.h and we get duplicate-symbol errors
// when winsock2.h is encountered later. Including winsock2.h first sets the
// _WINSOCKAPI_ guard so windows.h skips winsock.h cleanly.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "agentic_server.h"
#include "op_dispatcher.h"
#include "uia_session.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

namespace locus::uia {

namespace {

std::string short_random_id()
{
    std::mt19937_64 rng((unsigned long long)std::chrono::steady_clock::now()
                            .time_since_epoch().count());
    std::ostringstream os;
    os << std::hex << (rng() & 0xFFFFFFu);
    return os.str();
}

std::string timestamp_iso()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms);
    return buf;
}

// Best-effort socket reader that returns full newline-terminated lines.
// Owns a small carry buffer so partial reads work. Returns false on connection
// close / error.
class LineReader {
public:
    explicit LineReader(SOCKET s) : sock_(s) {}

    bool read_line(std::string& out)
    {
        out.clear();
        for (;;) {
            auto nl = buf_.find('\n');
            if (nl != std::string::npos) {
                out.assign(buf_, 0, nl);
                buf_.erase(0, nl + 1);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            char chunk[4096];
            int n = recv(sock_, chunk, (int)sizeof(chunk), 0);
            if (n <= 0) return false;
            buf_.append(chunk, chunk + n);
        }
    }

private:
    SOCKET      sock_;
    std::string buf_;
};

bool send_all(SOCKET s, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

bool send_line(SOCKET s, const std::string& data)
{
    return send_all(s, data) && send_all(s, "\n");
}

std::string make_response(bool ok, const std::string& detail,
                          const std::string& error,
                          const nlohmann::json& data,
                          const nlohmann::json& id)
{
    nlohmann::json r;
    r["ok"] = ok;
    if (!detail.empty()) r["detail"] = detail;
    if (!error.empty())  r["error"]  = error;
    if (!data.is_null()) r["data"]   = data;
    if (!id.is_null())   r["id"]     = id;
    // Tight serialisation; embedded newlines inside strings become \n escape
    // sequences automatically -- nlohmann::json::dump escapes per RFC 8259.
    return r.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Workspace prep
// ---------------------------------------------------------------------------

WorkspacePrep prepare_agentic_workspace(const std::string& workspace_spec,
                                        const fs::path& temp_root,
                                        bool seed_config,
                                        bool allow_first_time_prompts)
{
    WorkspacePrep r;

    fs::path workspace_dir;
    bool is_tmp = false;
    if (workspace_spec == "tmp" || workspace_spec.empty()) {
        std::error_code ec;
        fs::create_directories(temp_root, ec);
        workspace_dir = temp_root / ("agentic_" + short_random_id());
        fs::create_directories(workspace_dir, ec);
        is_tmp = true;
    } else {
        workspace_dir = fs::absolute(fs::path(workspace_spec));
        if (!fs::exists(workspace_dir)) {
            r.error = "workspace does not exist: " + workspace_dir.string();
            return r;
        }
    }

    if (seed_config && !allow_first_time_prompts) {
        try {
            auto cfg_path = workspace_dir / ".locus" / "config.json";
            // For real workspaces (non-tmp), refuse to clobber an existing
            // config -- agentic mode shouldn't silently change user state.
            if (!is_tmp && fs::exists(cfg_path)) {
                // Leave it alone.
            } else {
                std::error_code ec;
                fs::create_directories(cfg_path.parent_path(), ec);
                nlohmann::json cfg;
                cfg["index"]["semantic_search"]["enabled"] = false;
                cfg["tool_approvals"] = {
                    {"write_file",     "auto"},
                    {"edit_file",      "auto"},
                    {"delete_file",    "auto"},
                    {"run_command",    "auto"},
                    {"run_command_bg", "auto"}
                };
                cfg["sessions"]["auto_cleanup_enabled"] = false;
                cfg["sessions"]["restore_last"]        = false;
                std::ofstream f(cfg_path);
                f << cfg.dump(2) << '\n';
            }
        } catch (const std::exception& ex) {
            // Best-effort; downstream warnings will catch a real problem.
            (void)ex;
        }
    }

    r.resolved_dir = workspace_dir;
    r.is_tmp       = is_tmp;
    return r;
}

// ---------------------------------------------------------------------------
// Server loop
// ---------------------------------------------------------------------------

namespace {

struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~WinsockInit() { if (ok) WSACleanup(); }
};

void log_op(std::ofstream& log, const std::string& op,
            const nlohmann::json& args, const StepResult& sr)
{
    log << timestamp_iso() << "  op=" << op;
    if (args.is_object() && !args.empty()) {
        // One-line args, capped so a long file content doesn't blast the log.
        std::string s = args.dump();
        if (s.size() > 400) s = s.substr(0, 400) + "...";
        log << " args=" << s;
    }
    log << "  -> " << (sr.ok ? "ok" : "FAIL");
    if (!sr.detail.empty())  log << " detail=" << sr.detail;
    if (!sr.failure.empty()) log << " error="  << sr.failure;
    log << "\n";
    log.flush();
}

} // namespace

int run_agentic_server(const AgenticOptions& opts)
{
    WinsockInit wsa;
    if (!wsa.ok) {
        std::cerr << "agentic: WSAStartup failed\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(opts.output_dir, ec);
    std::ofstream log(opts.output_dir / "agentic.log");
    log << "agentic-server starting\n";
    log << "  workspace: " << opts.workspace_dir.string() << "\n";
    log << "  output:    " << opts.output_dir.string()    << "\n";
    log << "  locus_gui: " << opts.locus_gui_path.string() << "\n";
    log << "  bind:      " << opts.bind_host << ":" << opts.port << "\n";
    log.flush();

    // The UiaSession owns the COM apartment + the launched GUI process. We
    // construct it once for the lifetime of the server -- a `quit` op closes
    // the GUI but leaves the session ready for a subsequent `launch`.
    UiaSession uia;
    if (!uia.ready()) {
        log << "FAIL: UIA COM init failed: " << uia.last_error() << "\n";
        std::cerr << "agentic: UIA init failed: " << uia.last_error() << "\n";
        return 1;
    }

    OpDispatcher dispatcher(uia, opts.locus_gui_path, opts.output_dir,
                            opts.workspace_dir.string(),
                            opts.allow_first_time_prompts);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        log << "FAIL: socket() returned INVALID_SOCKET\n";
        std::cerr << "agentic: socket() failed\n";
        return 1;
    }

    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)opts.port);
    if (opts.bind_host.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, opts.bind_host.c_str(), &addr.sin_addr) != 1) {
            log << "FAIL: invalid bind_host '" << opts.bind_host << "'\n";
            std::cerr << "agentic: bad bind host\n";
            closesocket(listener);
            return 1;
        }
    }

    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        log << "FAIL: bind() WSA error " << err << "\n";
        std::cerr << "agentic: bind " << opts.bind_host << ":" << opts.port
                  << " failed (WSA " << err << ")\n";
        closesocket(listener);
        return 1;
    }
    if (listen(listener, 4) == SOCKET_ERROR) {
        log << "FAIL: listen() failed\n";
        std::cerr << "agentic: listen() failed\n";
        closesocket(listener);
        return 1;
    }

    // Write a port file so the client can rendezvous when port=0 (auto-pick)
    // or just to confirm the chosen port.
    sockaddr_in actual{};
    int alen = sizeof(actual);
    getsockname(listener, (sockaddr*)&actual, &alen);
    int actual_port = ntohs(actual.sin_port);
    {
        std::ofstream pf(opts.output_dir / "agentic.port");
        pf << actual_port << "\n";
    }
    std::cout << "agentic-server: listening on "
              << opts.bind_host << ":" << actual_port
              << " workspace=" << opts.workspace_dir.string() << "\n";
    std::cout.flush();
    log << "listening on " << opts.bind_host << ":" << actual_port << "\n";
    log.flush();

    bool shutdown_requested = false;

    while (!shutdown_requested) {
        sockaddr_in client_addr{};
        int clen = sizeof(client_addr);
        SOCKET client = accept(listener, (sockaddr*)&client_addr, &clen);
        if (client == INVALID_SOCKET) {
            log << "accept: WSA error " << WSAGetLastError() << "\n";
            continue;
        }
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   (const char*)&one, sizeof(one));

        LineReader reader(client);
        std::string line;
        while (reader.read_line(line)) {
            if (line.empty()) continue;

            nlohmann::json req;
            try {
                req = nlohmann::json::parse(line);
            } catch (const std::exception& ex) {
                send_line(client,
                    make_response(false, {}, std::string("invalid JSON: ") + ex.what(),
                                  nullptr, nullptr));
                continue;
            }
            if (!req.is_object()) {
                send_line(client,
                    make_response(false, {}, "request must be a JSON object",
                                  nullptr, nullptr));
                continue;
            }

            std::string op = req.value("op", std::string{});
            nlohmann::json args = req.contains("args") ? req["args"] : nlohmann::json::object();
            nlohmann::json id   = req.contains("id")   ? req["id"]   : nlohmann::json(nullptr);

            // Server-handled ops -- not passed to OpDispatcher.
            if (op == "shutdown") {
                StepResult sr{ true, {}, "shutdown acknowledged", nullptr };
                log_op(log, op, args, sr);
                send_line(client,
                    make_response(true, sr.detail, {}, nullptr, id));
                shutdown_requested = true;
                break;
            }
            if (op == "ping") {
                send_line(client,
                    make_response(true, "pong", {}, nullptr, id));
                continue;
            }
            if (op == "info") {
                nlohmann::json data;
                data["workspace_dir"] = opts.workspace_dir.string();
                data["output_dir"]    = opts.output_dir.string();
                data["locus_gui"]     = opts.locus_gui_path.string();
                data["port"]          = actual_port;
                data["step_index"]    = dispatcher.step_index();
                send_line(client,
                    make_response(true, "info", {}, std::move(data), id));
                continue;
            }
            if (op.empty()) {
                send_line(client,
                    make_response(false, {}, "missing 'op' field", nullptr, id));
                continue;
            }

            StepResult sr = dispatcher.dispatch(op, args);
            log_op(log, op, args, sr);
            send_line(client,
                sr.ok
                ? make_response(true, sr.detail, {}, sr.data, id)
                : make_response(false, sr.detail, sr.failure, sr.data, id));
        }
        closesocket(client);
    }

    closesocket(listener);
    log << "shutting down, closing GUI\n";
    log.flush();
    uia.close();
    log << "done.\n";
    return 0;
}

} // namespace locus::uia
