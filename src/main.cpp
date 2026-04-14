#include "workspace.h"
#include "file_watcher.h"
#include "indexer.h"
#include "llm_client.h"
#include "tool_registry.h"
#include "tools.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// CLI arguments
// ---------------------------------------------------------------------------

struct CliArgs {
    std::string workspace_path;
    std::string endpoint = "http://127.0.0.1:1234";
    std::string model;   // empty = server default
    bool        verbose  = false;
};

static CliArgs parse_args(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: locus <workspace_path> [--endpoint URL] [--model NAME] [-verbose]\n";
        std::exit(1);
    }

    CliArgs args;
    args.workspace_path = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-verbose") {
            args.verbose = true;
        } else if (a == "--endpoint" && i + 1 < argc) {
            args.endpoint = argv[++i];
        } else if (a == "--model" && i + 1 < argc) {
            args.model = argv[++i];
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

        // stderr: always at least info (cleaner console output)
        stderr_sink->set_level(verbose ? spdlog::level::trace : spdlog::level::info);
        // file: always trace
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
    CliArgs args = parse_args(argc, argv);

    // Resolve and validate workspace path
    std::error_code ec;
    fs::path workspace_path = fs::absolute(args.workspace_path, ec);
    if (ec || !fs::is_directory(workspace_path)) {
        std::cerr << "Error: not a directory: " << args.workspace_path << "\n";
        return 1;
    }

    // Ensure .locus/ exists before log init
    fs::path locus_dir = workspace_path / ".locus";
    fs::create_directories(locus_dir, ec);
    if (ec) {
        std::cerr << "Error: cannot create .locus/ directory: " << ec.message() << "\n";
        return 1;
    }

    init_logging(locus_dir, args.verbose);

    spdlog::info("Locus starting");
    spdlog::info("Workspace : {}", workspace_path.string());
    if (args.verbose) {
        spdlog::trace("Verbose logging active");
    }

    // S0.2: Open workspace (config, DB, file watcher)
    try {
        locus::Workspace ws(workspace_path);

        if (!ws.locus_md().empty()) {
            spdlog::info("LOCUS.md loaded ({} bytes)", ws.locus_md().size());
        }

        // Drain file watcher events and run incremental index updates
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::vector<locus::FileEvent> events;
        size_t n = ws.file_watcher().drain(events);
        if (n > 0) {
            spdlog::info("File watcher: drained {} events, updating index", n);
            ws.indexer().process_events(events);
        }

        auto& st = ws.indexer().stats();
        spdlog::info("Index stats: {} files ({} text, {} binary), {} symbols, {} headings",
                     st.files_total, st.files_indexed, st.files_binary,
                     st.symbols_total, st.headings_total);

        spdlog::info("S0.3 complete - FTS5 index operational");

        // S0.5: LLM client smoke test
        locus::LLMConfig llm_cfg;
        llm_cfg.base_url = args.endpoint;
        llm_cfg.model    = args.model;

        auto llm = locus::create_llm_client(llm_cfg);

        spdlog::info("LLM smoke test: sending test prompt...");
        std::vector<locus::ChatMessage> test_msgs = {
            {locus::MessageRole::system, "You are a helpful assistant. Reply in one short sentence."},
            {locus::MessageRole::user,   "What is 2 + 2?"}
        };

        int est_tokens = locus::ILLMClient::estimate_tokens(test_msgs);
        spdlog::info("Estimated prompt tokens: {}", est_tokens);

        std::string full_response;
        llm->stream_completion(test_msgs, {}, {
            /* on_token */
            [&](const std::string& token) {
                full_response += token;
                std::cout << token << std::flush;
            },
            /* on_tool_calls */
            [](const std::vector<locus::ToolCallRequest>&) {},
            /* on_complete */
            [&]() {
                std::cout << "\n";
                spdlog::info("LLM response complete ({} chars, ~{} tokens)",
                             full_response.size(),
                             locus::ILLMClient::estimate_tokens(full_response));
            },
            /* on_error */
            [](const std::string& err) {
                spdlog::error("LLM error: {}", err);
            }
        });

        spdlog::info("S0.5 complete - LLM client operational");

        // S0.6: Tool system
        locus::ToolRegistry tool_registry;
        locus::register_builtin_tools(tool_registry);

        auto tool_schema = tool_registry.build_schema_json();
        spdlog::info("Tool system: {} tools registered, schema {} bytes",
                     tool_registry.all().size(), tool_schema.dump().size());

        // Quick smoke test: read this project's own CLAUDE.md via ReadFileTool
        locus::WorkspaceContext tool_ws{workspace_path, &ws.query()};
        auto* read_tool = tool_registry.find("read_file");
        if (read_tool) {
            locus::ToolCall test_call{"smoke_1", "read_file",
                {{"path", "CLAUDE.md"}, {"offset", 1}, {"length", 5}}};
            auto result = read_tool->execute(test_call, tool_ws);
            spdlog::info("ReadFileTool smoke test: success={}, content_size={}",
                         result.success, result.content.size());
            spdlog::trace("ReadFileTool output:\n{}", result.content);
        }

        spdlog::info("S0.6 complete - tool system operational");

        // TODO(S0.7): Agent core
        // TODO(S0.8): full CLI frontend loop

    } catch (const std::exception& ex) {
        spdlog::error("Fatal: {}", ex.what());
        spdlog::shutdown();
        return 1;
    }

    spdlog::info("Locus shutting down");
    spdlog::shutdown();
    return 0;
}
