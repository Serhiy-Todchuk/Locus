#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_csharp_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"method_declaration",    "method"},
        {"class_declaration",     "class"},
        {"struct_declaration",    "struct"},
        {"interface_declaration", "interface"},
        {"enum_declaration",      "enum"},
        {"namespace_declaration", "namespace"},
    });
}

} // namespace locus
