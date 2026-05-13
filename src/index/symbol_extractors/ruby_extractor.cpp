#include "../symbol_extractor.h"

namespace locus {

std::unique_ptr<ISymbolExtractor> make_ruby_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"method",            "function"},
        {"singleton_method",  "method"},
        {"class",             "class"},
        {"module",            "module"},
    });
}

} // namespace locus
