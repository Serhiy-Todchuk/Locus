#pragma once

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace locus {

// Manages a list of recently opened workspace paths.
// Stored as a JSON array in AppData/Roaming/Locus/recent_workspaces.json.
class RecentWorkspaces {
public:
    static constexpr int k_max_entries = 10;

    // Returns the global config directory (e.g. AppData/Roaming/Locus).
    static std::filesystem::path config_dir()
    {
        namespace fs = std::filesystem;
#ifdef _WIN32
        const char* appdata = std::getenv("APPDATA");
        if (appdata)
            return fs::path(appdata) / "Locus";
#else
        const char* home = std::getenv("HOME");
        if (home)
            return fs::path(home) / ".config" / "locus";
#endif
        return fs::current_path() / ".locus-global";
    }

    static std::filesystem::path file_path()
    {
        return config_dir() / "recent_workspaces.json";
    }

    // Load the recent list from disk. Returns empty vector on any error.
    static std::vector<std::string> load()
    {
        std::vector<std::string> result;
        auto path = file_path();
        std::ifstream in(path);
        if (!in.is_open())
            return result;

        try {
            auto j = nlohmann::json::parse(in);
            if (j.is_array()) {
                for (auto& item : j) {
                    if (item.is_string())
                        result.push_back(item.get<std::string>());
                }
            }
        } catch (...) {
            spdlog::warn("Failed to parse {}", path.string());
        }
        return result;
    }

    // Add a path to the front of the list (most recent first).
    // Removes duplicates and trims to k_max_entries.
    static void add(const std::string& workspace_path)
    {
        namespace fs = std::filesystem;

        // Normalize to canonical form for dedup.
        std::error_code ec;
        std::string normalized = fs::canonical(workspace_path, ec).string();
        if (ec)
            normalized = workspace_path;

        auto entries = load();

        // Remove existing occurrence (case-insensitive on Windows).
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                [&](const std::string& e) {
#ifdef _WIN32
                    // Case-insensitive path comparison.
                    auto a = fs::path(e).lexically_normal().wstring();
                    auto b = fs::path(normalized).lexically_normal().wstring();
                    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
                    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
                    return a == b;
#else
                    return fs::path(e).lexically_normal() == fs::path(normalized).lexically_normal();
#endif
                }),
            entries.end());

        // Insert at front.
        entries.insert(entries.begin(), normalized);

        // Trim.
        if (static_cast<int>(entries.size()) > k_max_entries)
            entries.resize(k_max_entries);

        save(entries);
    }

    // Returns the most recently used workspace path, or empty string.
    static std::string last()
    {
        auto entries = load();
        return entries.empty() ? std::string{} : entries.front();
    }

private:
    static void save(const std::vector<std::string>& entries)
    {
        namespace fs = std::filesystem;
        auto dir = config_dir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            spdlog::warn("Cannot create config dir: {}", dir.string());
            return;
        }

        std::ofstream out(file_path());
        if (!out.is_open()) {
            spdlog::warn("Cannot write {}", file_path().string());
            return;
        }
        nlohmann::json j = entries;
        out << j.dump(2);
    }
};

} // namespace locus
