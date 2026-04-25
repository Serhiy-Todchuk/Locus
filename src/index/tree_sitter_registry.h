#pragma once

#include <string>
#include <unordered_map>

typedef struct TSParser TSParser;
typedef struct TSLanguage TSLanguage;

namespace locus {

// Owns a Tree-sitter parser plus the per-language `TSLanguage*` map shared
// between the indexer and (future) `ast_search`. The parser instance is not
// thread-safe; callers must serialise access.
class TreeSitterRegistry {
public:
    TreeSitterRegistry();
    ~TreeSitterRegistry();

    TreeSitterRegistry(const TreeSitterRegistry&) = delete;
    TreeSitterRegistry& operator=(const TreeSitterRegistry&) = delete;

    TSParser* parser() const { return parser_; }

    // Lookup TSLanguage by canonical language name ("cpp", "python", ...).
    // Returns nullptr when no Tree-sitter grammar is registered for that name
    // (e.g. "markdown", "json").
    const TSLanguage* language_for_name(const std::string& name) const;

private:
    TSParser* parser_ = nullptr;
    std::unordered_map<std::string, const TSLanguage*> langs_;
};

} // namespace locus
