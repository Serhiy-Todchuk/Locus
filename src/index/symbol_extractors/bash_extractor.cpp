#include "../symbol_extractor.h"

namespace locus {

// tree-sitter-bash collapses both `function foo() { ... }` and `foo() { ... }`
// into a single `function_definition` node, so one rule covers both forms.
std::unique_ptr<ISymbolExtractor> make_bash_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_definition", "function"},
    });
}

} // namespace locus
