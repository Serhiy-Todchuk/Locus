#pragma once

#include <filesystem>
#include <string>

namespace locus {

// Reads a single named entry from a .zip archive via miniz.
// Returns true on success and writes the (possibly binary) bytes into `out`.
// Returns false if the archive can't be opened or the entry is missing.
// Logs warnings via spdlog on failure.
bool read_zip_entry(const std::filesystem::path& zip_path,
                    const std::string& entry_name,
                    std::string& out);

} // namespace locus
