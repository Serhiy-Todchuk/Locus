// S5.L -- locus_ui_tests entry point.
// S5.Z task 9 -- added `--agentic` mode for interactive QA-LLM-driven testing.
//
// Usage (scripted, S5.L):
//   locus_ui_tests --script <path> [--script <path> ...]
//                  [--output-root <dir>]
//                  [--gui <path/to/locus_gui.exe>]
//                  [--list]
//
// Usage (agentic, S5.Z task 9):
//   locus_ui_tests --agentic --workspace <path|tmp> [--port N]
//                  [--allow-first-time-prompts] [--no-seed-config]
//                  [--output-root <dir>] [--gui <path>]
//
// Exit codes:
//   0   all scripts passed / agentic server clean shutdown
//   1   one or more scripts failed / agentic server error
//   2   bad usage / configuration error
//   77  every script skipped (e.g. preconditions not met) -- matches the
//       Unix convention also used by tests/integration/.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "agentic_server.h"
#include "script_runner.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

void print_usage()
{
    std::cerr <<
        "Usage:\n"
        "  scripted mode:\n"
        "    locus_ui_tests --script <path.json> [--script <path.json> ...]\n"
        "                   [--output-root <dir>] [--gui <path/to/locus_gui.exe>]\n"
        "                   [--list] [--all]\n"
        "  agentic mode (interactive QA-LLM driver):\n"
        "    locus_ui_tests --agentic --workspace <path|tmp> [--port N]\n"
        "                   [--allow-first-time-prompts] [--no-seed-config]\n"
        "                   [--output-root <dir>] [--gui <path>]\n"
        "Exit codes: 0=pass  1=fail  2=usage error  77=all skipped\n";
}

fs::path default_locus_gui()
{
#ifdef LOCUS_UI_TESTS_DEFAULT_GUI
    return fs::path{ LOCUS_UI_TESTS_DEFAULT_GUI };
#else
    return fs::path{ "locus_gui.exe" };
#endif
}

fs::path default_scripts_dir()
{
#ifdef LOCUS_UI_TESTS_SCRIPTS_DIR
    return fs::path{ LOCUS_UI_TESTS_SCRIPTS_DIR };
#else
    return fs::path{ "scripts" };
#endif
}

fs::path default_output_root()
{
#ifdef LOCUS_UI_TESTS_OUTPUT_DIR
    return fs::path{ LOCUS_UI_TESTS_OUTPUT_DIR };
#else
    return fs::path{ "output" };
#endif
}

fs::path default_temp_workspace_root()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    return fs::path{ buf } / "locus_ui_tests";
#else
    return fs::temp_directory_path() / "locus_ui_tests";
#endif
}

int run_scripted(int argc, char** argv)
{
    std::vector<fs::path> scripts;
    fs::path output_root  = default_output_root();
    fs::path gui_path     = default_locus_gui();
    fs::path scripts_root = default_scripts_dir();
    bool list_only = false;
    bool run_all   = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--script" && i + 1 < argc) {
            scripts.emplace_back(argv[++i]);
        } else if (a == "--output-root" && i + 1 < argc) {
            output_root = argv[++i];
        } else if (a == "--gui" && i + 1 < argc) {
            gui_path = argv[++i];
        } else if (a == "--scripts-dir" && i + 1 < argc) {
            scripts_root = argv[++i];
        } else if (a == "--all") {
            run_all = true;
        } else if (a == "--list") {
            list_only = true;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else if (a == "--agentic") {
            continue;  // handled by caller; should never reach here
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            print_usage();
            return 2;
        }
    }

    fs::path exe_dir = fs::path{ argv[0] }.parent_path();
    auto resolve = [&](fs::path p) {
        if (p.is_absolute()) return p;
        if (fs::exists(p))   return fs::absolute(p);
        fs::path with_exe = exe_dir / p;
        return fs::absolute(with_exe);
    };
    output_root  = resolve(output_root);
    gui_path     = resolve(gui_path);
    scripts_root = resolve(scripts_root);

    if (run_all && scripts.empty()) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(scripts_root, ec)) {
            if (e.path().extension() == ".json")
                scripts.push_back(e.path());
        }
        std::sort(scripts.begin(), scripts.end());
    }

    if (list_only) {
        std::cout << "Discovered scripts in " << scripts_root.string() << ":\n";
        std::error_code ec;
        for (auto& e : fs::directory_iterator(scripts_root, ec)) {
            if (e.path().extension() == ".json")
                std::cout << "  " << e.path().filename().string() << "\n";
        }
        return 0;
    }

    if (scripts.empty()) {
        std::cerr << "No scripts requested. Use --script <path>, --all, or --agentic.\n";
        print_usage();
        return 2;
    }

    if (!fs::exists(gui_path)) {
        std::cerr << "locus_gui.exe not found at " << gui_path.string()
                  << " (override with --gui)\n";
        return 2;
    }

    std::error_code ec;
    fs::create_directories(output_root, ec);
    fs::path temp_root = default_temp_workspace_root();
    fs::create_directories(temp_root, ec);

    int pass = 0;
    int fail = 0;

    for (const auto& s : scripts) {
        locus::uia::RunOptions opts;
        opts.script_path         = s;
        opts.locus_gui_path      = gui_path;
        opts.output_root         = output_root;
        opts.temp_workspace_root = temp_root;

        locus::uia::ScriptRunner runner(opts);
        auto r = runner.run();

        if (r.passed) {
            std::cout << "PASS  " << r.name
                      << "  (" << r.steps_executed << " steps, "
                      << r.duration_ms << " ms)\n";
            ++pass;
        } else {
            std::cout << "FAIL  " << r.name
                      << "  step=" << r.failure_op
                      << "  reason=" << r.failure_reason
                      << "  (artifacts: " << r.output_dir.string() << ")\n";
            ++fail;
        }
        fs::path xml = output_root / (r.name + ".xml");
        locus::uia::write_junit_xml(r, xml);
    }

    std::cout << "\nSummary: " << pass << " passed, " << fail << " failed\n";

    if (fail > 0) return 1;
    if (pass == 0) return 77;
    return 0;
}

int run_agentic(int argc, char** argv)
{
    std::string workspace_spec;
    fs::path output_root = default_output_root();
    fs::path gui_path    = default_locus_gui();
    int port             = 7878;
    bool allow_first_time_prompts = false;
    bool seed_config              = true;
    std::string bind_host = "127.0.0.1";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--agentic") continue;
        if (a == "--workspace" && i + 1 < argc) {
            workspace_spec = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--bind-host" && i + 1 < argc) {
            bind_host = argv[++i];
        } else if (a == "--allow-first-time-prompts") {
            allow_first_time_prompts = true;
        } else if (a == "--no-seed-config") {
            seed_config = false;
        } else if (a == "--output-root" && i + 1 < argc) {
            output_root = argv[++i];
        } else if (a == "--gui" && i + 1 < argc) {
            gui_path = argv[++i];
        } else if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown agentic argument: " << a << "\n";
            print_usage();
            return 2;
        }
    }

    if (workspace_spec.empty()) {
        std::cerr << "agentic: --workspace <path|tmp> is required\n";
        return 2;
    }

    fs::path exe_dir = fs::path{ argv[0] }.parent_path();
    auto resolve = [&](fs::path p) {
        if (p.is_absolute()) return p;
        if (fs::exists(p))   return fs::absolute(p);
        fs::path with_exe = exe_dir / p;
        return fs::absolute(with_exe);
    };
    output_root = resolve(output_root);
    gui_path    = resolve(gui_path);

    if (!fs::exists(gui_path)) {
        std::cerr << "locus_gui.exe not found at " << gui_path.string()
                  << " (override with --gui)\n";
        return 2;
    }

    fs::path temp_root = default_temp_workspace_root();
    auto prep = locus::uia::prepare_agentic_workspace(
        workspace_spec, temp_root, seed_config, allow_first_time_prompts);
    if (!prep.error.empty()) {
        std::cerr << "agentic: " << prep.error << "\n";
        return 2;
    }

    // Per-run output dir under output_root, named for the workspace dir.
    // Strip a redundant "agentic_" prefix on tmp dirs (they already carry it).
    std::string workspace_label = prep.resolved_dir.filename().string();
    std::string prefix = "agentic_";
    if (workspace_label.rfind(prefix, 0) == 0)
        workspace_label.erase(0, prefix.size());
    fs::path output_dir = output_root / ("agentic_" + workspace_label);
    std::error_code ec;
    fs::create_directories(output_dir, ec);

    locus::uia::AgenticOptions opts;
    opts.locus_gui_path           = gui_path;
    opts.workspace_dir            = prep.resolved_dir;
    opts.output_dir               = output_dir;
    opts.allow_first_time_prompts = allow_first_time_prompts;
    opts.seed_config              = seed_config;
    opts.port                     = port;
    opts.bind_host                = bind_host;

    return locus::uia::run_agentic_server(opts);
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--agentic") return run_agentic(argc, argv);
    }
    return run_scripted(argc, argv);
}
