#include "../symbol_extractor.h"

namespace locus {

// C-specific rules. Tree-sitter-c does not emit `class_specifier` or
// `namespace_definition`, so we drop those vs. cpp and add `type_definition`
// (typedefs) and `union_specifier` -- both first-class in C.
std::unique_ptr<ISymbolExtractor> make_c_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_definition", "function"},
        {"struct_specifier",    "struct"},
        {"union_specifier",     "union"},
        {"enum_specifier",      "enum"},
        {"type_definition",     "typedef"},
    });
}

} // namespace locus
