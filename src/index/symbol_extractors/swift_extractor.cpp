#include "../symbol_extractor.h"

namespace locus {

// alex-pinkus/tree-sitter-swift folds struct / class / enum / actor / extension
// into `class_declaration` with a `declaration_kind` field; we label them all
// "class" since rule-based extraction can't read that field. Protocols are a
// separate node. deinit has no name and is skipped here.
std::unique_ptr<ISymbolExtractor> make_swift_symbol_extractor()
{
    return make_rule_based_symbol_extractor({
        {"function_declaration", "function"},
        {"class_declaration",    "class"},
        {"protocol_declaration", "interface"},
        {"init_declaration",     "method"},
    });
}

} // namespace locus
