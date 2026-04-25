#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_go_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_declaration", "function"},
        {"method_declaration",   "method"},
        {"type_declaration",     "struct"},
    });
}

} // namespace locus
