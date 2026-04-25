#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_js_ts_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_declaration", "function"},
        {"class_declaration",    "class"},
        {"method_definition",    "method"},
    });
}

} // namespace locus
