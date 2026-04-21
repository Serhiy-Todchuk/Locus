#include "zip_reader.h"

#include <spdlog/spdlog.h>

#include <miniz.h>

namespace locus {

bool read_zip_entry(const std::filesystem::path& zip_path,
                    const std::string& entry_name,
                    std::string& out)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) {
        spdlog::warn("zip: failed to open {}", zip_path.string());
        return false;
    }

    int index = mz_zip_reader_locate_file(&zip, entry_name.c_str(), nullptr, 0);
    if (index < 0) {
        mz_zip_reader_end(&zip);
        return false;
    }

    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &stat)) {
        mz_zip_reader_end(&zip);
        return false;
    }

    out.resize(static_cast<size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip, static_cast<mz_uint>(index),
                                      out.data(), out.size(), 0)) {
        spdlog::warn("zip: failed to extract {} from {}", entry_name, zip_path.string());
        out.clear();
        mz_zip_reader_end(&zip);
        return false;
    }

    mz_zip_reader_end(&zip);
    return true;
}

} // namespace locus
