#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// S6.2 -- minimal, MIT-licensed ZIM (Kiwix / Wikipedia) reader.
//
// We deliberately do NOT use libzim: it is GPL-2.0-or-later and would
// relicense the whole Locus binary. The ZIM v6 binary format is documented and
// stable, so a focused read-only reader is a few hundred lines. We support
// exactly what Locus needs: open an archive, enumerate the article ('C'
// namespace, text/html) entries, and pull one article's raw HTML on demand.
//
// The file is memory-mapped read-only -- an English-Wikipedia ZIM is ~12 GB,
// so we never read the whole thing into RAM; we mmap and index into it.
//
// Format reference (all little-endian) verified against the libzim reference
// implementation source:
//   - 80-byte header (magic "ZIM\x04", major/minor version, entry/cluster
//     counts, pointer-list offsets, main page, mime list offset).
//   - MIME type list: NUL-terminated strings ending in an empty string.
//   - URL pointer list: entryCount * 8-byte LE dirent offsets, sorted by
//     (namespace byte, path).
//   - Cluster pointer list: clusterCount * 8-byte LE cluster offsets.
//   - Dirent: mimetype(2) paramLen(1) namespace(1) version(4) then, for an
//     article, clusterNumber(4) blobNumber(4) url(NUL) title(NUL); for a
//     redirect (mimetype 0xffff) redirectIndex(4) url(NUL) title(NUL).
//   - Cluster: info byte (low nibble = compression 1/none 4/xz 5/zstd, bit 4 =
//     8-byte-offset extended flag); body (decompressed if needed) begins with
//     an offset table whose first entry / pointer-size gives the blob count.

namespace locus::zim {

namespace fs = std::filesystem;

// One enumerable article entry. `path` is the namespace-stripped URL path
// (e.g. "Albert_Einstein"); `title` is the human title (falls back to path
// when the dirent carries an empty title, as is common in the new scheme).
struct ArticleRef {
    uint32_t    entry_index = 0;   // index into the URL pointer list
    std::string path;              // namespace-stripped path (the C/ url)
    std::string title;             // display title, or path if empty
};

// Thrown on a structurally invalid archive (bad magic, truncated header,
// unsupported compression, corrupt offset table). Callers (ZimWorkspace ctor,
// CLI/GUI open path) surface this as a normal "couldn't open" error.
class ZimError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ZimArchive {
public:
    // Opens + memory-maps the file and parses the header / pointer lists.
    // Throws ZimError on any structural problem.
    explicit ZimArchive(const fs::path& path);
    ~ZimArchive();

    ZimArchive(const ZimArchive&) = delete;
    ZimArchive& operator=(const ZimArchive&) = delete;

    // -- Metadata -----------------------------------------------------------
    const fs::path& path() const { return path_; }
    uint32_t entry_count()   const { return entry_count_; }
    uint32_t cluster_count() const { return cluster_count_; }
    uint16_t major_version() const { return major_version_; }
    uint16_t minor_version() const { return minor_version_; }

    // Archive title / description / language from the 'M' metadata namespace
    // (empty if absent). Used to label the workspace.
    std::string metadata(const std::string& key) const;

    // -- Article enumeration ------------------------------------------------
    // Every entry in the 'C' (content) namespace whose MIME type is text/html
    // -- i.e. real articles, not embedded CSS/JS/image assets. Redirects and
    // linktarget/deleted entries are skipped. Computed lazily on first call.
    const std::vector<ArticleRef>& articles() const;

    // -- Content read -------------------------------------------------------
    // Raw HTML bytes of the article at the given URL pointer index. Resolves
    // redirects (capped depth). Returns nullopt if the entry is missing /
    // a dangling redirect / not a content entry.
    std::optional<std::string> read_content(uint32_t entry_index) const;

    // Convenience: read by namespace-stripped path within the 'C' namespace.
    std::optional<std::string> read_article(const std::string& path) const;

private:
    struct Dirent {
        uint16_t    mimetype = 0;     // 0xffff redirect, 0xfffe linktarget, 0xfffd deleted
        char        ns = 0;
        uint32_t    cluster_number = 0;
        uint32_t    blob_number = 0;
        uint32_t    redirect_index = 0;
        std::string url;
        std::string title;
        bool is_redirect() const { return mimetype == 0xffff; }
        bool is_article()  const { return mimetype < 0xfffd; }
    };

    // Raw pointer + bounds-checked little-endian reads into the mmap.
    const uint8_t* base() const { return data_; }
    uint64_t read_u64(uint64_t off) const;
    uint32_t read_u32(uint64_t off) const;
    uint16_t read_u16(uint64_t off) const;
    uint8_t  read_u8 (uint64_t off) const;

    uint64_t dirent_offset(uint32_t entry_index) const;  // via URL pointer list
    Dirent   parse_dirent(uint64_t off) const;
    // Decompress (if needed) + slice blob `blob` out of cluster `cluster`.
    std::optional<std::string> read_blob(uint32_t cluster, uint32_t blob) const;
    // Find the [begin,end) URL-pointer index range for a namespace byte.
    std::pair<uint32_t, uint32_t> namespace_range(char ns) const;
    // Binary-search the first entry index whose dirent namespace >= ns.
    uint32_t namespace_lower_bound(char ns) const;
    std::optional<uint32_t> find_in_namespace(char ns, const std::string& path) const;

    fs::path  path_;
    uint64_t  file_size_ = 0;
    const uint8_t* data_ = nullptr;   // mmap base

    // OS handles (PIMPL-free; platform branches in the .cpp).
    void* mmap_handle_ = nullptr;     // Win32: file mapping HANDLE; POSIX: unused
    void* file_handle_ = nullptr;     // Win32: file HANDLE; POSIX: fd as intptr

    // Header fields.
    uint16_t major_version_ = 0;
    uint16_t minor_version_ = 0;
    uint32_t entry_count_ = 0;
    uint32_t cluster_count_ = 0;
    uint64_t url_ptr_pos_ = 0;
    uint64_t title_ptr_pos_ = 0;
    uint64_t cluster_ptr_pos_ = 0;
    uint64_t mime_list_pos_ = 0;
    uint32_t main_page_ = 0;
    uint64_t checksum_pos_ = 0;

    std::vector<std::string> mime_types_;       // index -> mime string
    int html_mime_index_ = -1;                  // index of text/html (-1 if none)

    mutable std::vector<ArticleRef> articles_;  // lazily filled
    mutable bool articles_built_ = false;
};

} // namespace locus::zim
