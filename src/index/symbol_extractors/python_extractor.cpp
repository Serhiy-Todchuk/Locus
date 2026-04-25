#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_python_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_definition", "function"},
        {"class_definition",    "class"},
    });
}

} // namespace locus
