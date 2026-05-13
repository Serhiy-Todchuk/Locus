#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_php_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_definition",  "function"},
        {"method_declaration",   "method"},
        {"class_declaration",    "class"},
        {"trait_declaration",    "trait"},
        {"interface_declaration","interface"},
    });
}

} // namespace locus
