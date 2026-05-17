#pragma once

#include "memory_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace locus {

// S5.K -- pure filter / sort helpers for the Memory Bank panel.
//
// Lives here (not in `MemoryStore`) so the UI panel can call into it without
// touching SQLite, and so the filter logic is unit-testable without a real
// store or workspace. The store stays the source of truth for entries;
// this layer is policy on top.

struct MemoryFilter {
    // Free-text search box content. Empty = no text constraint.
    std::string              query;
    // Restrict to entries carrying EVERY tag in this list. Empty = no
    // tag constraint.
    std::vector<std::string> tags;
    // Source dropdown: empty = all, otherwise one of `"user"`, `"agent"`,
    // `"mined"`. Anything else is ignored.
    std::string              source;
    // Pinned-only toggle. When true, drops every unpinned entry.
    bool                     pinned_only = false;
    // Creation-time bounds in unix-seconds. `0` on either side disables
    // that bound.
    std::int64_t             created_from = 0;
    std::int64_t             created_to   = 0;
};

// Sort modes for the list view.
enum class MemorySort {
    pinned_first_last_used,   // default: pinned first, then last_used desc
    pinned_first_created,     // pinned first, then created desc
    content_asc,
    content_desc,
    tags_asc,
    source_asc,
    created_asc,
    created_desc,
    last_used_asc,
    last_used_desc,
};

// Return the subset of `entries` matching `filter`. Pure -- does not touch
// `last_used_at`, does not bump any DB rows. Caller decides what to do with
// the result (sort, display, bulk-mutate).
std::vector<MemoryStore::Entry> apply_filter(
    const std::vector<MemoryStore::Entry>& entries,
    const MemoryFilter&                    filter);

// Sort `entries` in place according to `mode`. Pure.
void sort_entries(std::vector<MemoryStore::Entry>& entries, MemorySort mode);

// All distinct tags across `entries`, alphabetically sorted. Used to
// populate the tag-filter dropdown / chip bar.
std::vector<std::string> collect_unique_tags(
    const std::vector<MemoryStore::Entry>& entries);

// Lightweight metadata about a soft-deleted entry. We parse just enough of
// the frontmatter to render the "Recently Deleted" list -- restore /
// hard-delete operations still go through MemoryStore by id.
struct DeletedEntryStub {
    std::string  id;
    std::string  content_preview;   // first ~80 chars, single line
    std::vector<std::string> tags;
    std::int64_t deleted_at_proxy = 0;  // entry's updated_at, used as "moved at"
};

// Walk `<memory_dir>/.deleted/*.md` and return parsed stubs, newest first.
// Returns an empty list if the directory doesn't exist. Cheap on the v1
// scale (max_entries 200 + retention 30 days => O(1000) files worst case).
std::vector<DeletedEntryStub> list_deleted_stubs(const class MemoryStore& store);

} // namespace locus
