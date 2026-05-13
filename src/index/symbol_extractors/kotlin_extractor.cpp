#include "../symbol_extractor.h"

namespace locus {

// fwcd/tree-sitter-kotlin uses a single `class_declaration` rule for both
// `class Foo` and `interface Foo` (distinguished by a keyword child, not a
// field). The shared rule-based extractor relies on the helper's child-walk
// fallback to find the name -- this grammar exposes no "name" field on any
// of these declarations.
std::unique_ptr<ISymbolExtractor> make_kotlin_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_declaration", "function"},
        {"class_declaration",    "class"},
        {"object_declaration",   "object"},
    });
}

} // namespace locus
