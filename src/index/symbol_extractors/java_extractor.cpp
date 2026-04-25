#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_java_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"method_declaration",    "method"},
        {"class_declaration",     "class"},
        {"interface_declaration", "interface"},
        {"enum_declaration",      "enum"},
    });
}

} // namespace locus
