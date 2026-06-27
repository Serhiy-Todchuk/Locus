#include "zim/zim_archive.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <zstd.h>
#include <lzma.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace locus::zim {

namespace {

constexpr uint32_t k_zim_magic = 0x044d495a;  // "ZIM\x04" little-endian
constexpr uint64_t k_header_size = 80;
constexpr int      k_max_redirect_depth = 16;

// Compression codes from the cluster info byte (libzim enum Compression).
enum ClusterCompression : uint8_t {
    Comp_DefaultNone = 0,
    Comp_None        = 1,
    Comp_Zip         = 2,  // discontinued
    Comp_Bzip2       = 3,  // discontinued
    Comp_Lzma        = 4,  // xz / liblzma
    Comp_Zstd        = 5,
};

// One-shot xz/lzma decode of `src` into a growable buffer. ZIM clusters use the
// .xz container (lzma_stream_decoder with auto-detect handles it). We don't
// know the decompressed size up front, so we grow the output in chunks.
std::optional<std::string> xz_decompress(const uint8_t* src, size_t src_len)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    // 64 MB memlimit is comfortably above any single Wikipedia cluster.
    if (lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
        return std::nullopt;

    std::string out;
    out.resize(std::max<size_t>(src_len * 4, 64 * 1024));
    strm.next_in = src;
    strm.avail_in = src_len;
    strm.next_out = reinterpret_cast<uint8_t*>(out.data());
    strm.avail_out = out.size();

    for (;;) {
        lzma_ret ret = lzma_code(&strm, LZMA_FINISH);
        if (ret == LZMA_STREAM_END) break;
        if (ret == LZMA_OK) {
            if (strm.avail_out == 0) {
                size_t used = out.size();
                out.resize(out.size() * 2);
                strm.next_out = reinterpret_cast<uint8_t*>(out.data()) + used;
                strm.avail_out = out.size() - used;
            }
            // else: more input to consume on the next iteration.
            continue;
        }
        lzma_end(&strm);
        return std::nullopt;  // LZMA_DATA_ERROR / etc.
    }
    out.resize(out.size() - strm.avail_out);
    lzma_end(&strm);
    return out;
}

// One-shot zstd decode. ZIM zstd clusters carry the content-size in the frame
// header for the common case; when absent (streaming-framed) we fall back to a
// growing streaming decode.
std::optional<std::string> zstd_decompress(const uint8_t* src, size_t src_len)
{
    unsigned long long const sz = ZSTD_getFrameContentSize(src, src_len);
    if (sz != ZSTD_CONTENTSIZE_UNKNOWN && sz != ZSTD_CONTENTSIZE_ERROR) {
        std::string out;
        out.resize(static_cast<size_t>(sz));
        size_t got = ZSTD_decompress(out.data(), out.size(), src, src_len);
        if (ZSTD_isError(got)) return std::nullopt;
        out.resize(got);
        return out;
    }

    // Unknown content size: streaming decode into a growing buffer.
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) return std::nullopt;
    ZSTD_initDStream(ds);
    std::string out;
    out.resize(std::max<size_t>(src_len * 4, 64 * 1024));
    ZSTD_inBuffer in{ src, src_len, 0 };
    size_t written = 0;
    for (;;) {
        if (written == out.size()) out.resize(out.size() * 2);
        ZSTD_outBuffer ob{ out.data() + written, out.size() - written, 0 };
        size_t ret = ZSTD_decompressStream(ds, &ob, &in);
        written += ob.pos;
        if (ZSTD_isError(ret)) { ZSTD_freeDStream(ds); return std::nullopt; }
        if (ret == 0) break;                 // a full frame was flushed
        if (in.pos == in.size && ob.pos == 0) break;  // no more progress
    }
    ZSTD_freeDStream(ds);
    out.resize(written);
    return out;
}

} // namespace

// -- mmap lifecycle ----------------------------------------------------------

ZimArchive::ZimArchive(const fs::path& path) : path_(path)
{
#ifdef _WIN32
    HANDLE fh = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE)
        throw ZimError("ZIM: cannot open file: " + path.string());
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(fh, &sz)) { CloseHandle(fh); throw ZimError("ZIM: GetFileSizeEx failed"); }
    file_size_ = static_cast<uint64_t>(sz.QuadPart);
    HANDLE mh = CreateFileMappingW(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mh) { CloseHandle(fh); throw ZimError("ZIM: CreateFileMapping failed"); }
    void* view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (!view) { CloseHandle(mh); CloseHandle(fh); throw ZimError("ZIM: MapViewOfFile failed"); }
    file_handle_ = fh;
    mmap_handle_ = mh;
    data_ = static_cast<const uint8_t*>(view);
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw ZimError("ZIM: cannot open file: " + path.string());
    struct stat st{};
    if (fstat(fd, &st) != 0) { ::close(fd); throw ZimError("ZIM: fstat failed"); }
    file_size_ = static_cast<uint64_t>(st.st_size);
    void* view = ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) { ::close(fd); throw ZimError("ZIM: mmap failed"); }
    file_handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    data_ = static_cast<const uint8_t*>(view);
#endif

    if (file_size_ < k_header_size)
        throw ZimError("ZIM: file too small to hold a header");

    // -- Header -------------------------------------------------------------
    if (read_u32(0) != k_zim_magic)
        throw ZimError("ZIM: bad magic (not a ZIM file)");
    major_version_   = read_u16(4);
    minor_version_   = read_u16(6);
    // [8..23] uuid -- skip.
    entry_count_     = read_u32(24);
    cluster_count_   = read_u32(28);
    url_ptr_pos_     = read_u64(32);
    title_ptr_pos_   = read_u64(40);
    cluster_ptr_pos_ = read_u64(48);
    mime_list_pos_   = read_u64(56);
    main_page_       = read_u32(64);
    // [68..71] layoutPage -- skip.
    checksum_pos_    = read_u64(72);

    if (major_version_ != 6 && major_version_ != 5)
        spdlog::warn("ZIM: unexpected major version {} (continuing best-effort)",
                     major_version_);

    // Bounds-sanity the pointer lists against the file size.
    auto in_bounds = [&](uint64_t off, uint64_t need) {
        return off <= file_size_ && need <= file_size_ - off;
    };
    if (!in_bounds(url_ptr_pos_, uint64_t(entry_count_) * 8) ||
        !in_bounds(cluster_ptr_pos_, uint64_t(cluster_count_) * 8) ||
        !in_bounds(mime_list_pos_, 1))
        throw ZimError("ZIM: pointer lists out of bounds (corrupt header)");

    // -- MIME type list -----------------------------------------------------
    uint64_t p = mime_list_pos_;
    while (p < file_size_) {
        const char* s = reinterpret_cast<const char*>(data_ + p);
        size_t maxlen = static_cast<size_t>(file_size_ - p);
        size_t len = ::strnlen(s, maxlen);
        if (len == 0) break;            // empty string terminates the list
        std::string mime(s, len);
        if (html_mime_index_ < 0 &&
            mime.rfind("text/html", 0) == 0)  // "text/html" or "text/html;charset=..."
            html_mime_index_ = static_cast<int>(mime_types_.size());
        mime_types_.push_back(std::move(mime));
        p += len + 1;
    }

    spdlog::info("ZIM: opened {} (v{}.{}, {} entries, {} clusters, {} mime types, "
                 "html_mime_index={})",
                 path.filename().string(), major_version_, minor_version_,
                 entry_count_, cluster_count_, mime_types_.size(), html_mime_index_);
}

ZimArchive::~ZimArchive()
{
#ifdef _WIN32
    if (data_) UnmapViewOfFile(const_cast<uint8_t*>(data_));
    if (mmap_handle_) CloseHandle(static_cast<HANDLE>(mmap_handle_));
    if (file_handle_) CloseHandle(static_cast<HANDLE>(file_handle_));
#else
    if (data_) ::munmap(const_cast<uint8_t*>(data_), file_size_);
    if (file_handle_) ::close(static_cast<int>(reinterpret_cast<intptr_t>(file_handle_)));
#endif
}

// -- Bounds-checked little-endian reads --------------------------------------

uint64_t ZimArchive::read_u64(uint64_t off) const
{
    if (off + 8 > file_size_) throw ZimError("ZIM: read_u64 out of bounds");
    uint64_t v;
    std::memcpy(&v, data_ + off, 8);
    return v;  // x86/ARM little-endian; ZIM is LE, no swap needed on our targets
}
uint32_t ZimArchive::read_u32(uint64_t off) const
{
    if (off + 4 > file_size_) throw ZimError("ZIM: read_u32 out of bounds");
    uint32_t v;
    std::memcpy(&v, data_ + off, 4);
    return v;
}
uint16_t ZimArchive::read_u16(uint64_t off) const
{
    if (off + 2 > file_size_) throw ZimError("ZIM: read_u16 out of bounds");
    uint16_t v;
    std::memcpy(&v, data_ + off, 2);
    return v;
}
uint8_t ZimArchive::read_u8(uint64_t off) const
{
    if (off + 1 > file_size_) throw ZimError("ZIM: read_u8 out of bounds");
    return data_[off];
}

// -- Dirents -----------------------------------------------------------------

uint64_t ZimArchive::dirent_offset(uint32_t entry_index) const
{
    if (entry_index >= entry_count_) throw ZimError("ZIM: entry index out of range");
    return read_u64(url_ptr_pos_ + uint64_t(entry_index) * 8);
}

ZimArchive::Dirent ZimArchive::parse_dirent(uint64_t off) const
{
    Dirent d;
    d.mimetype = read_u16(off);
    uint8_t param_len = read_u8(off + 2);
    d.ns = static_cast<char>(read_u8(off + 3));
    // [4..7] version -- skip.
    uint64_t cur;
    if (d.is_redirect()) {
        d.redirect_index = read_u32(off + 8);
        cur = off + 12;
    } else if (d.mimetype == 0xfffe || d.mimetype == 0xfffd) {
        // linktarget / deleted: no cluster/blob/redirect payload.
        cur = off + 8;
    } else {
        d.cluster_number = read_u32(off + 8);
        d.blob_number    = read_u32(off + 12);
        cur = off + 16;
    }
    // url (NUL-terminated)
    {
        const char* s = reinterpret_cast<const char*>(data_ + cur);
        size_t maxlen = static_cast<size_t>(file_size_ - cur);
        size_t len = ::strnlen(s, maxlen);
        d.url.assign(s, len);
        cur += len + 1;
    }
    // title (NUL-terminated)
    {
        const char* s = reinterpret_cast<const char*>(data_ + cur);
        size_t maxlen = static_cast<size_t>(file_size_ - cur);
        size_t len = ::strnlen(s, maxlen);
        d.title.assign(s, len);
        cur += len + 1;
    }
    (void)param_len;  // parameter trailer not needed for reading
    return d;
}

// -- Namespace range (binary search over the sorted URL pointer list) --------

uint32_t ZimArchive::namespace_lower_bound(char ns) const
{
    // First entry index whose dirent namespace byte >= ns. The URL pointer
    // list is sorted by (namespace, path), so the namespace byte is monotone.
    uint32_t lo = 0, hi = entry_count_;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        char mid_ns;
        try {
            mid_ns = parse_dirent(dirent_offset(mid)).ns;
        } catch (const ZimError&) {
            // A corrupt dirent mid-search: treat as >= so we shrink the window.
            hi = mid;
            continue;
        }
        if (mid_ns < ns) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

std::pair<uint32_t, uint32_t> ZimArchive::namespace_range(char ns) const
{
    uint32_t begin = namespace_lower_bound(ns);
    uint32_t end   = namespace_lower_bound(static_cast<char>(ns + 1));
    return { begin, end };
}

std::optional<uint32_t> ZimArchive::find_in_namespace(char ns, const std::string& path) const
{
    auto [begin, end] = namespace_range(ns);
    // Binary search within [begin,end) on the path (sorted within a namespace).
    uint32_t lo = begin, hi = end;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        Dirent d;
        try { d = parse_dirent(dirent_offset(mid)); }
        catch (const ZimError&) { return std::nullopt; }
        int cmp = d.url.compare(path);
        if (cmp < 0) lo = mid + 1;
        else if (cmp > 0) hi = mid;
        else return mid;
    }
    return std::nullopt;
}

// -- Article enumeration -----------------------------------------------------

const std::vector<ArticleRef>& ZimArchive::articles() const
{
    if (articles_built_) return articles_;
    articles_built_ = true;

    // The new-namespace scheme (minor>=1) puts all content under 'C'. Old
    // archives use 'A' for articles. Pick the content namespace by version.
    char content_ns = (minor_version_ >= 1) ? 'C' : 'A';
    uint32_t begin, end;
    try {
        std::tie(begin, end) = namespace_range(content_ns);
    } catch (const ZimError& e) {
        spdlog::warn("ZIM: namespace range scan failed: {}", e.what());
        return articles_;
    }

    articles_.reserve(end - begin);
    for (uint32_t i = begin; i < end; ++i) {
        Dirent d;
        try { d = parse_dirent(dirent_offset(i)); }
        catch (const ZimError&) { continue; }
        if (!d.is_article()) continue;  // skip redirects / linktarget / deleted
        // Gate to text/html so embedded CSS/JS/image assets in 'C' are excluded.
        // If the archive has no text/html mime (unusual), fall back to taking
        // every content entry so we don't index nothing.
        if (html_mime_index_ >= 0 &&
            d.mimetype != static_cast<uint16_t>(html_mime_index_))
            continue;
        ArticleRef ref;
        ref.entry_index = i;
        ref.path = d.url;
        ref.title = d.title.empty() ? d.url : d.title;
        articles_.push_back(std::move(ref));
    }
    spdlog::info("ZIM: enumerated {} article(s) in namespace '{}'",
                 articles_.size(), content_ns);
    return articles_;
}

// -- Cluster / blob read -----------------------------------------------------

std::optional<std::string> ZimArchive::read_blob(uint32_t cluster, uint32_t blob) const
{
    if (cluster >= cluster_count_) return std::nullopt;
    uint64_t cluster_off;
    try { cluster_off = read_u64(cluster_ptr_pos_ + uint64_t(cluster) * 8); }
    catch (const ZimError&) { return std::nullopt; }
    if (cluster_off >= file_size_) return std::nullopt;

    uint8_t info = data_[cluster_off];
    uint8_t comp = info & 0x0F;
    bool extended = (info & 0x10) != 0;
    const size_t P = extended ? 8 : 4;

    // The cluster body is everything after the info byte, bounded by the next
    // cluster offset (or checksum/EOF for the last cluster).
    uint64_t body_off = cluster_off + 1;
    uint64_t body_end = file_size_;
    if (cluster + 1 < cluster_count_) {
        try {
            uint64_t next = read_u64(cluster_ptr_pos_ + uint64_t(cluster + 1) * 8);
            if (next > cluster_off && next <= file_size_) body_end = next;
        } catch (const ZimError&) {}
    } else if (checksum_pos_ != 0 && checksum_pos_ <= file_size_) {
        body_end = checksum_pos_;
    }
    if (body_end < body_off) return std::nullopt;
    size_t comp_len = static_cast<size_t>(body_end - body_off);

    // Materialise the (decompressed) body.
    std::string body;
    if (comp == Comp_None || comp == Comp_DefaultNone) {
        body.assign(reinterpret_cast<const char*>(data_ + body_off), comp_len);
    } else if (comp == Comp_Zstd) {
        auto d = zstd_decompress(data_ + body_off, comp_len);
        if (!d) return std::nullopt;
        body = std::move(*d);
    } else if (comp == Comp_Lzma) {
        auto d = xz_decompress(data_ + body_off, comp_len);
        if (!d) return std::nullopt;
        body = std::move(*d);
    } else {
        spdlog::warn("ZIM: unsupported cluster compression {} (cluster {})", comp, cluster);
        return std::nullopt;
    }

    // Offset table: first offset / P = number of offsets; blob count = that - 1.
    auto read_off = [&](size_t idx) -> uint64_t {
        size_t at = idx * P;
        if (at + P > body.size()) throw ZimError("ZIM: offset table overrun");
        uint64_t v = 0;
        std::memcpy(&v, body.data() + at, P);
        return v;  // LE
    };
    uint64_t first;
    try { first = read_off(0); } catch (const ZimError&) { return std::nullopt; }
    if (first < P || (first % P) != 0) return std::nullopt;
    uint64_t n_offsets = first / P;
    if (n_offsets == 0) return std::nullopt;
    uint64_t blob_count = n_offsets - 1;
    if (blob >= blob_count) return std::nullopt;

    uint64_t start, stop;
    try {
        start = read_off(blob);
        stop  = read_off(blob + 1);
    } catch (const ZimError&) { return std::nullopt; }
    if (stop < start || stop > body.size()) return std::nullopt;

    return body.substr(static_cast<size_t>(start), static_cast<size_t>(stop - start));
}

// -- Content read (with redirect resolution) ---------------------------------

std::optional<std::string> ZimArchive::read_content(uint32_t entry_index) const
{
    uint32_t idx = entry_index;
    for (int depth = 0; depth < k_max_redirect_depth; ++depth) {
        Dirent d;
        try { d = parse_dirent(dirent_offset(idx)); }
        catch (const ZimError&) { return std::nullopt; }
        if (d.is_redirect()) {
            if (d.redirect_index == idx) return std::nullopt;  // self-loop
            idx = d.redirect_index;
            continue;
        }
        if (!d.is_article()) return std::nullopt;  // linktarget/deleted
        return read_blob(d.cluster_number, d.blob_number);
    }
    return std::nullopt;  // redirect chain too deep
}

std::optional<std::string> ZimArchive::read_article(const std::string& path) const
{
    char content_ns = (minor_version_ >= 1) ? 'C' : 'A';
    auto idx = find_in_namespace(content_ns, path);
    if (!idx) return std::nullopt;
    return read_content(*idx);
}

// -- Metadata ----------------------------------------------------------------

std::string ZimArchive::metadata(const std::string& key) const
{
    auto idx = find_in_namespace('M', key);
    if (!idx) return "";
    auto content = read_content(*idx);
    return content ? *content : "";
}

} // namespace locus::zim
