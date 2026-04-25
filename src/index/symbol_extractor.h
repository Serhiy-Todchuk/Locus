#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

typedef struct TSNode TSNode;

namespace locus {

// Result of extracting one symbol declaration (function, class, ...) from
// a Tree-sitter parse tree. Lines are 1-based, inclusive.
struct ExtractedSymbol {
    int         line_start = 0;
    int         line_end   = 0;
    std::string kind;        // "function", "class", "struct", "method", ...
    std::string name;
    std::string signature;
    std::string parent_name; // empty for top-level declarations
};

// Mapping from a Tree-sitter node type to the symbol kind we record.
struct SymbolRule {
    const char* node_type;
    const char* kind;
};

// Per-language symbol extractor. Implementations walk the parse tree and
// produce a flat list of `ExtractedSymbol`s. The DB persistence side stays
// in `Indexer` — this interface is purely about turning AST nodes into
// language-agnostic structs.
class ISymbolExtractor {
public:
    virtual ~ISymbolExtractor() = default;

    virtual std::vector<ExtractedSymbol> extract(
        TSNode root, std::string_view source) const = 0;
};

// Maps language name (matching `TreeSitterRegistry::language_for_name`) to
// extractor instance. `Indexer` owns one and asks `register_builtin_symbol_extractors`
// to populate it at construction time.
class SymbolExtractorRegistry {
public:
    void register_extractor(std::string language,
                            std::unique_ptr<ISymbolExtractor> extractor);

    const ISymbolExtractor* find(const std::string& language) const;

private:
    std::unordered_map<std::string, std::unique_ptr<ISymbolExtractor>> map_;
};

// Concrete extractor that walks the tree two levels deep and records any node
// whose type matches one of the supplied rules. All seven built-in languages
// fit this shape — only the rule table differs.
std::unique_ptr<ISymbolExtractor> make_rule_based_symbol_extractor(
    std::vector<SymbolRule> rules);

// Per-language factories — implementation lives in
// `symbol_extractors/<lang>_extractor.cpp`. They only assemble a rule table
// and forward to `make_rule_based_symbol_extractor`.
std::unique_ptr<ISymbolExtractor> make_cpp_symbol_extractor();
std::unique_ptr<ISymbolExtractor> make_python_symbol_extractor();
std::unique_ptr<ISymbolExtractor> make_js_ts_symbol_extractor();
std::unique_ptr<ISymbolExtractor> make_go_symbol_extractor();
std::unique_ptr<ISymbolExtractor> make_rust_symbol_extractor();
std::unique_ptr<ISymbolExtractor> make_java_symbol_extractor();
std::unique_ptr<ISymbolExtractor> make_csharp_symbol_extractor();

// Wires every built-in language extractor into the supplied registry.
void register_builtin_symbol_extractors(SymbolExtractorRegistry& reg);

} // namespace locus
