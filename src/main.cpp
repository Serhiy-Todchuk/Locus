#include "workspace.h"
#include "file_watcher.h"
#include "indexer.h"
#include "llm_client.h"
#include "tool_registry.h"
#include "tools.h"
#include "agent_core.h"
#include "frontends/cli_frontend.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <csignal>
#include <clocale>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Globals for signal handling
// ---------------------------------------------------------------------------

static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int /*sig*/)
{
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CLI arguments
// ---------------------------------------------------------------------------

struct CliArgs {
    std::string workspace_path;
    std::string endpoint = "http://127.0.0.1:1234";
    std::string model;   // empty = server default
    int         context  = 0;     // 0 = auto-detect from server, fallback 8192
    bool        verbose  = false;
    bool        reindex  = false;
};

static CliArgs parse_args(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: locus <workspace_path> [--endpoint URL] [--model NAME] [--context N] [-verbose] [-reindex]\n";
        std::exit(1);
    }

    CliArgs args;
    args.workspace_path = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-verbose") {
            args.verbose = true;
        } else if (a == "-reindex") {
            args.reindex = true;
        } else if (a == "--endpoint" && i + 1 < argc) {
            args.endpoint = argv[++i];
        } else if (a == "--model" && i + 1 < argc) {
            args.model = argv[++i];
        } else if (a == "--context" && i + 1 < argc) {
            args.context = std::stoi(argv[++i]);
        }
    }

    return args;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static void init_logging(const fs::path& locus_dir, bool verbose)
{
    try {
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto file_sink   = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (locus_dir / "locus.log").string(),
            10 * 1024 * 1024,   // 10 MB per file
            3                    // 3 rotating files
        );

        // stderr: info by default, trace with -verbose
        stderr_sink->set_level(verbose ? spdlog::level::trace : spdlog::level::info);
        // file: always trace (full diagnostic detail)
        file_sink->set_level(spdlog::level::trace);

        auto logger = std::make_shared<spdlog::logger>(
            "locus",
            spdlog::sinks_init_list{stderr_sink, file_sink}
        );
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [t:%t] [%^%l%$] %v");

        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::warn);
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log init failed: " << ex.what() << "\n";
        std::exit(1);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Enable UTF-8 console I/O (Ukrainian, etc.).
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    // Enable VT processing for proper Unicode rendering in modern terminals.
    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(h_out, &mode))
            SetConsoleMode(h_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
    std::setlocale(LC_ALL, ".UTF-8");

    CliArgs args = parse_args(argc, argv);

    // Install Ctrl+C handler.
    std::signal(SIGINT, signal_handler);

    // Resolve and validate workspace path.
    std::error_code ec;
    fs::path workspace_path = fs::absolute(args.workspace_path, ec);
    if (ec || !fs::is_directory(workspace_path)) {
        std::cerr << "Error: not a directory: " << args.workspace_path << "\n";
        return 1;
    }

    // Ensure .locus/ exists before log init.
    fs::path locus_dir = workspace_path / ".locus";
    fs::create_directories(locus_dir, ec);
    if (ec) {
        std::cerr << "Error: cannot create .locus/ directory: " << ec.message() << "\n";
        return 1;
    }

    init_logging(locus_dir, args.verbose);

    spdlog::info("Locus starting");
    spdlog::info("Workspace: {}", workspace_path.string());
    if (args.verbose)
        spdlog::trace("Verbose logging active");

    // -reindex: delete existing index.db to force a full rebuild.
    if (args.reindex) {
        auto db_path = locus_dir / "index.db";
        if (fs::exists(db_path)) {
            fs::remove(db_path);
            spdlog::info("Reindex: deleted existing index.db");
        }
        // Also remove WAL/SHM files if present.
        std::error_code rm_ec;
        fs::remove(locus_dir / "index.db-wal", rm_ec);
        fs::remove(locus_dir / "index.db-shm", rm_ec);
    }

    try {
        // Open workspace: config, DB, file watcher, index.
        locus::Workspace ws(workspace_path);

        if (!ws.locus_md().empty())
            spdlog::info("LOCUS.md loaded ({} bytes)", ws.locus_md().size());

        // Drain startup file events.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::vector<locus::FileEvent> events;
        size_t n = ws.file_watcher().drain(events);
        if (n > 0) {
            spdlog::info("File watcher: drained {} events, updating index", n);
            ws.indexer().process_events(events);
        }

        auto& st = ws.indexer().stats();
        spdlog::info("Index: {} files ({} text, {} binary), {} symbols, {} headings",
                     st.files_total, st.files_indexed, st.files_binary,
                     st.symbols_total, st.headings_total);

        // LLM client.
        locus::LLMConfig llm_cfg;
        llm_cfg.base_url      = args.endpoint;
        llm_cfg.model         = args.model;
        llm_cfg.context_limit = args.context;
        auto llm = locus::create_llm_client(llm_cfg);

        // Query server for model info (context length, model name).
        auto model_info = llm->query_model_info();
        if (!model_info.id.empty() && llm_cfg.model.empty()) {
            llm_cfg.model = model_info.id;
            spdlog::info("Model auto-detected: {}", llm_cfg.model);
        }
        if (args.context > 0) {
            // Explicit CLI override takes priority.
            llm_cfg.context_limit = args.context;
            spdlog::info("Context limit (from --context): {}", llm_cfg.context_limit);
        } else if (model_info.context_length > 0) {
            llm_cfg.context_limit = model_info.context_length;
            spdlog::info("Context limit (from server): {}", llm_cfg.context_limit);
        } else {
            llm_cfg.context_limit = 8192;  // fallback default
            spdlog::info("Context limit (default): {}", llm_cfg.context_limit);
        }

        // Tool system.
        locus::ToolRegistry tool_registry;
        locus::register_builtin_tools(tool_registry);
        spdlog::info("Tools: {} registered", tool_registry.all().size());

        // Agent core.
        locus::WorkspaceContext ws_ctx{workspace_path, &ws.query(), ws.embedding_worker()};
        locus::WorkspaceMetadata ws_meta;
        ws_meta.root          = workspace_path;
        ws_meta.file_count    = static_cast<int>(st.files_total);
        ws_meta.symbol_count  = static_cast<int>(st.symbols_total);
        ws_meta.heading_count = static_cast<int>(st.headings_total);

        fs::path sessions_dir = locus_dir / "sessions";
        locus::AgentCore agent(*llm, tool_registry, ws_ctx,
                               ws.locus_md(), ws_meta, llm_cfg, sessions_dir);

        // CLI frontend.
        locus::CliFrontend cli(agent);
        agent.register_frontend(&cli);

        // Start the agent thread.
        agent.start();

        // REPL.
        std::cout << "Locus ready. Type a message, or /quit to exit.\n\n";

        while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
            // Prompt with context meter.
            std::cout << "[ctx: " << cli.last_used() << "/"
                      << cli.last_limit() << "] > " << std::flush;

            std::string line;
            if (!std::getline(std::cin, line))
                break;  // EOF or closed stdin

            if (g_shutdown_requested.load(std::memory_order_relaxed))
                break;

            // Trim whitespace.
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                continue;  // empty input
            line = line.substr(start);
            auto end = line.find_last_not_of(" \t\r\n");
            if (end != std::string::npos)
                line.resize(end + 1);

            // Commands.
            if (line == "/quit" || line == "/exit" || line == "/q")
                break;

            if (line == "/reset") {
                agent.reset_conversation();
                std::cout << "Conversation reset.\n";
                continue;
            }

            if (line == "/compact") {
                agent.compact_context(locus::CompactionStrategy::drop_tool_results);
                std::cout << "Compacted: tool results dropped.\n";
                continue;
            }

            if (line == "/history") {
                auto& h = agent.history();
                std::cout << "Conversation: " << h.size() << " messages, ~"
                          << h.estimate_tokens() << " tokens\n";
                for (size_t i = 0; i < h.size(); ++i) {
                    auto& m = h.messages()[i];
                    std::cout << "  [" << i << "] "
                              << locus::to_string(m.role) << ": "
                              << m.content.substr(0, 80);
                    if (m.content.size() > 80) std::cout << "...";
                    std::cout << "\n";
                }
                continue;
            }

            if (line == "/save") {
                auto id = agent.save_session();
                std::cout << "Session saved: " << id << "\n";
                continue;
            }

            if (line == "/sessions") {
                auto sessions = agent.sessions().list();
                if (sessions.empty()) {
                    std::cout << "No saved sessions.\n";
                } else {
                    std::cout << "Saved sessions:\n";
                    for (auto& s : sessions) {
                        std::cout << "  " << s.id
                                  << "  (" << s.message_count << " msgs, ~"
                                  << s.estimated_tokens << " tk)";
                        if (!s.first_user_message.empty())
                            std::cout << "  \"" << s.first_user_message << "\"";
                        std::cout << "\n";
                    }
                }
                continue;
            }

            if (line.rfind("/load ", 0) == 0) {
                auto id = line.substr(6);
                // Trim whitespace.
                auto s = id.find_first_not_of(" \t");
                if (s != std::string::npos) id = id.substr(s);
                auto e = id.find_last_not_of(" \t");
                if (e != std::string::npos) id.resize(e + 1);

                try {
                    agent.load_session(id);
                    std::cout << "Session loaded: " << id << "\n";
                } catch (const std::exception& ex) {
                    std::cerr << "Error: " << ex.what() << "\n";
                }
                continue;
            }

            if (line == "/help") {
                std::cout << "Commands:\n"
                          << "  /quit       Exit Locus\n"
                          << "  /reset      Start a fresh conversation\n"
                          << "  /compact    Drop tool results to free context\n"
                          << "  /history    Show conversation summary\n"
                          << "  /save       Save current session\n"
                          << "  /sessions   List saved sessions\n"
                          << "  /load <id>  Load a saved session\n"
                          << "  /help       Show this help\n"
                          << "Anything else is sent as a message to the agent.\n";
                continue;
            }

            // Drain incremental file events before each turn.
            events.clear();
            n = ws.file_watcher().drain(events);
            if (n > 0) {
                spdlog::trace("Pre-turn: {} file events, updating index", n);
                ws.indexer().process_events(events);
            }

            // Send message to agent (blocks until turn completes).
            std::cout << "\n";
            agent.send_message_sync(line);
            std::cout << "\n";
        }

        agent.stop();
        agent.unregister_frontend(&cli);

    } catch (const std::exception& ex) {
        spdlog::error("Fatal: {}", ex.what());
        spdlog::shutdown();
        return 1;
    }

    spdlog::info("Locus shutting down");
    spdlog::shutdown();
    return 0;
}
