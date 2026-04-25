#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_cpp_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_definition",  "function"},
        {"class_specifier",      "class"},
        {"struct_specifier",     "struct"},
        {"enum_specifier",       "enum"},
        {"namespace_definition", "namespace"},
    });
}

} // namespace locus
