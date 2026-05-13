// S5.L -- locus_ui_tests entry point.
//
// Usage:
//   locus_ui_tests --script <path> [--script <path> ...]
//                  [--output-root <dir>]
//                  [--gui <path/to/locus_gui.exe>]
//                  [--list]
//
// Exit codes:
//   0   all scripts passed
//   1   one or more scripts failed
//   2   bad usage / configuration error
//   77  every script skipped (e.g. preconditions not met) -- matches the
//       Unix convention also used by tests/integration/.

#ifndef NOMINMAX
#define NOMINMAX
#endif

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
        "Usage: locus_ui_tests --script <path.json> [--script <path.json> ...]\n"
        "                     [--output-root <dir>]\n"
        "                     [--gui <path/to/locus_gui.exe>]\n"
        "                     [--list]\n"
        "                     [--all]    # run every .json in default scripts/ folder\n"
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

} // namespace

int main(int argc, char** argv)
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
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            print_usage();
            return 2;
        }
    }

    // Resolve relative paths against the exe location so the binary works
    // from any CWD. This matches the integration tests' behavior.
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
        std::cerr << "No scripts requested. Use --script <path> or --all.\n";
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
