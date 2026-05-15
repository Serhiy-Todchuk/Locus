// S5.P -- Source Encoding Audit & Lint
//
// Verifies that every entry in scripts/non_ascii_allowlist.txt references a
// file that actually exists.  Catches stale allow-list entries when files are
// renamed or deleted so the CMake lint stays trustworthy.
//
// The build-time check_no_em_dashes.cmake is the primary enforcement surface;
// this unit test is the "allow-list integrity" companion.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct AllowEntry {
    std::string file_path;   // workspace-relative, forward slashes
    std::string codepoint;   // "U+XXXX" or empty for bare entries
};

// Locate the workspace root by walking up from the test binary location
// until we find scripts/non_ascii_allowlist.txt.
fs::path find_workspace_root()
{
    fs::path candidate = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(candidate / "scripts" / "non_ascii_allowlist.txt"))
            return candidate;
        candidate = candidate.parent_path();
    }
    return {};
}

std::vector<AllowEntry> load_allowlist(const fs::path& allowlist_path)
{
    std::vector<AllowEntry> entries;
    std::ifstream f(allowlist_path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        // Normalise backslashes
        for (char& c : line) if (c == '\\') c = '/';

        AllowEntry e;
        auto colon = line.rfind(':');
        // Check for ":U+XXXX" suffix
        if (colon != std::string::npos &&
            line.size() > colon + 2 &&
            line[colon + 1] == 'U' && line[colon + 2] == '+')
        {
            e.file_path  = line.substr(0, colon);
            e.codepoint  = line.substr(colon + 1);
        } else {
            e.file_path = line;
        }
        entries.push_back(e);
    }
    return entries;
}

} // namespace

TEST_CASE("allow-list entries reference existing files", "[s5.p][source-hygiene]")
{
    fs::path root = find_workspace_root();
    REQUIRE_FALSE(root.empty());

    fs::path allowlist = root / "scripts" / "non_ascii_allowlist.txt";
    REQUIRE(fs::exists(allowlist));

    auto entries = load_allowlist(allowlist);
    REQUIRE_FALSE(entries.empty()); // allow-list should have at least one entry

    for (const auto& entry : entries) {
        fs::path target = root / entry.file_path;
        // Replace forward slashes so filesystem::exists works on Windows
        std::string native = entry.file_path;
        for (char& c : native) if (c == '/') c = fs::path::preferred_separator;
        target = root / native;

        INFO("allow-list entry: " << entry.file_path
             << (entry.codepoint.empty() ? "" : " " + entry.codepoint));
        CHECK(fs::exists(target));
    }
}

TEST_CASE("allow-list file path is normalised (forward slashes)", "[s5.p][source-hygiene]")
{
    fs::path root = find_workspace_root();
    REQUIRE_FALSE(root.empty());

    fs::path allowlist = root / "scripts" / "non_ascii_allowlist.txt";
    REQUIRE(fs::exists(allowlist));

    auto entries = load_allowlist(allowlist);
    for (const auto& entry : entries) {
        INFO("path: " << entry.file_path);
        CHECK(entry.file_path.find('\\') == std::string::npos);
    }
}
