#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// CLI arguments
// ---------------------------------------------------------------------------

struct CliArgs {
    std::string workspace_path;
    bool        verbose = false;
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
        }
        // --endpoint and --model are parsed in later stages (S0.5)
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
        // file: always trace — gives full picture when reading locus.log after a run
        file_sink->set_level(spdlog::level::trace);

        auto logger = std::make_shared<spdlog::logger>(
            "locus",
            spdlog::sinks_init_list{stderr_sink, file_sink}
        );
        logger->set_level(spdlog::level::trace);  // logger itself never filters
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
    fs::path workspace = fs::absolute(args.workspace_path, ec);
    if (ec || !fs::is_directory(workspace)) {
        std::cerr << "Error: not a directory: " << args.workspace_path << "\n";
        return 1;
    }

    // Ensure .locus/ exists — must happen before log init
    fs::path locus_dir = workspace / ".locus";
    fs::create_directories(locus_dir, ec);
    if (ec) {
        std::cerr << "Error: cannot create .locus/ directory: " << ec.message() << "\n";
        return 1;
    }

    init_logging(locus_dir, args.verbose);

    spdlog::info("Locus starting");
    spdlog::info("Workspace : {}", workspace.string());
    spdlog::info("Log dir   : {}", locus_dir.string());
    if (args.verbose) {
        spdlog::trace("Verbose logging active — spdlog trace on stderr");
    }

    // TODO(S0.2): open Workspace, load config, init SQLite
    // TODO(S0.3): build FTS5 index
    // TODO(S0.4): IndexQuery API
    // TODO(S0.5): LLM client
    // TODO(S0.6): Tool system
    // TODO(S0.7): Agent core
    // TODO(S0.8): full CLI frontend loop

    spdlog::info("S0.1 complete — project skeleton operational");
    spdlog::info("Locus shutting down");
    spdlog::shutdown();
    return 0;
}
