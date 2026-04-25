#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_rust_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_item", "function"},
        {"struct_item",   "struct"},
        {"enum_item",     "enum"},
        {"impl_item",     "class"},
        {"trait_item",    "interface"},
    });
}

} // namespace locus
