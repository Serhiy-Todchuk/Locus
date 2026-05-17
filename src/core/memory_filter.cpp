#include "memory_filter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace locus {

namespace fs = std::filesystem;

namespace {

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return true;
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

bool entry_carries_tag(const MemoryStore::Entry& e, const std::string& tag)
{
    return std::find(e.tags.begin(), e.tags.end(), tag) != e.tags.end();
}

bool entry_matches_query(const MemoryStore::Entry& e, const std::string& q)
{
    if (q.empty()) return true;
    if (contains_ci(e.content, q)) return true;
    if (contains_ci(e.id, q))      return true;
    for (auto& t : e.tags) if (contains_ci(t, q)) return true;
    return false;
}

// Parse just enough of an entry .md file to populate a DeletedEntryStub.
// We don't validate the entire frontmatter -- a malformed entry is shown
// with whatever fields parsed.
bool parse_stub(const fs::path& p, DeletedEntryStub& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    // Normalise CRLF
    std::string norm;
    norm.reserve(raw.size());
    for (char c : raw) if (c != '\r') norm.push_back(c);

    if (norm.compare(0, 3, "---") != 0) return false;
    auto body_start = norm.find('\n', 3);
    if (body_start == std::string::npos) return false;
    ++body_start;
    auto second = norm.find("\n---", body_start);
    if (second == std::string::npos) return false;
    auto body_after = norm.find('\n', second + 4);
    std::string fm   = norm.substr(body_start, second - body_start);
    std::string body = (body_after == std::string::npos)
                          ? std::string{}
                          : norm.substr(body_after + 1);

    // First non-blank line of body, capped at 80 chars.
    std::string preview;
    {
        std::istringstream bi(body);
        std::string line;
        while (std::getline(bi, line)) {
            // Trim
            auto a = line.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            auto b = line.find_last_not_of(" \t");
            preview = line.substr(a, b - a + 1);
            break;
        }
        if (preview.size() > 80) preview.resize(80);
    }
    out.content_preview = std::move(preview);

    // Walk frontmatter for id / tags / updated_at.
    std::istringstream in(fm);
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto trim = [](std::string s) {
            auto a = s.find_first_not_of(" \t");
            if (a == std::string::npos) return std::string{};
            auto b = s.find_last_not_of(" \t");
            return s.substr(a, b - a + 1);
        };
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if (key == "id") out.id = val;
        else if (key == "tags") {
            // Strip optional brackets and split on comma.
            if (!val.empty() && val.front() == '[') val.erase(0, 1);
            if (!val.empty() && val.back()  == ']') val.pop_back();
            std::stringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                auto a = tok.find_first_not_of(" \t");
                if (a == std::string::npos) continue;
                auto b = tok.find_last_not_of(" \t");
                out.tags.push_back(tok.substr(a, b - a + 1));
            }
        } else if (key == "updated_at") {
            // ISO8601 -> unix seconds. Cheap parser; failed parse leaves 0.
            std::tm tm{};
            std::istringstream is(val);
            is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            if (!is.fail()) {
#ifdef _WIN32
                auto t = _mkgmtime(&tm);
#else
                auto t = timegm(&tm);
#endif
                out.deleted_at_proxy = t < 0 ? 0 : static_cast<std::int64_t>(t);
            }
        }
    }
    if (out.id.empty()) out.id = p.stem().string();
    return true;
}

} // namespace

std::vector<MemoryStore::Entry> apply_filter(
    const std::vector<MemoryStore::Entry>& entries,
    const MemoryFilter&                    filter)
{
    std::vector<MemoryStore::Entry> out;
    out.reserve(entries.size());
    const bool has_source = (filter.source == "user"
                          || filter.source == "agent"
                          || filter.source == "mined");
    for (auto& e : entries) {
        if (filter.pinned_only && !e.pinned)            continue;
        if (has_source && e.source != filter.source)    continue;
        if (filter.created_from > 0
            && e.created_at < filter.created_from)      continue;
        if (filter.created_to > 0
            && e.created_at > filter.created_to)        continue;
        bool tags_ok = true;
        for (auto& t : filter.tags) {
            if (!entry_carries_tag(e, t)) { tags_ok = false; break; }
        }
        if (!tags_ok) continue;
        if (!entry_matches_query(e, filter.query)) continue;
        out.push_back(e);
    }
    return out;
}

void sort_entries(std::vector<MemoryStore::Entry>& entries, MemorySort mode)
{
    auto cmp = [mode](const MemoryStore::Entry& a, const MemoryStore::Entry& b) {
        switch (mode) {
        case MemorySort::pinned_first_last_used:
            if (a.pinned != b.pinned) return a.pinned;
            return a.last_used_at > b.last_used_at;
        case MemorySort::pinned_first_created:
            if (a.pinned != b.pinned) return a.pinned;
            return a.created_at > b.created_at;
        case MemorySort::content_asc:    return a.content < b.content;
        case MemorySort::content_desc:   return a.content > b.content;
        case MemorySort::tags_asc: {
            std::string ta, tb;
            for (auto& t : a.tags) { ta += t; ta += ' '; }
            for (auto& t : b.tags) { tb += t; tb += ' '; }
            return ta < tb;
        }
        case MemorySort::source_asc:     return a.source < b.source;
        case MemorySort::created_asc:    return a.created_at < b.created_at;
        case MemorySort::created_desc:   return a.created_at > b.created_at;
        case MemorySort::last_used_asc:  return a.last_used_at < b.last_used_at;
        case MemorySort::last_used_desc: return a.last_used_at > b.last_used_at;
        }
        return false;
    };
    std::sort(entries.begin(), entries.end(), cmp);
}

std::vector<std::string> collect_unique_tags(
    const std::vector<MemoryStore::Entry>& entries)
{
    std::set<std::string> set;
    for (auto& e : entries) for (auto& t : e.tags) set.insert(t);
    return {set.begin(), set.end()};
}

std::vector<DeletedEntryStub> list_deleted_stubs(const MemoryStore& store)
{
    std::vector<DeletedEntryStub> out;
    auto deleted_dir = store.memory_dir() / ".deleted";
    std::error_code ec;
    if (!fs::exists(deleted_dir, ec)) return out;

    for (auto& de : fs::directory_iterator(deleted_dir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        auto p = de.path();
        if (p.extension() != ".md") continue;
        DeletedEntryStub stub;
        if (parse_stub(p, stub)) out.push_back(std::move(stub));
    }

    std::sort(out.begin(), out.end(),
              [](const DeletedEntryStub& a, const DeletedEntryStub& b) {
                  return a.deleted_at_proxy > b.deleted_at_proxy;
              });
    return out;
}

} // namespace locus
