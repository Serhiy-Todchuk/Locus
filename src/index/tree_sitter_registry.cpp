#include "tree_sitter_registry.h"

#include <spdlog/spdlog.h>
#include <tree_sitter/api.h>

extern "C" {
    const TSLanguage* tree_sitter_c();
    const TSLanguage* tree_sitter_cpp();
    const TSLanguage* tree_sitter_python();
    const TSLanguage* tree_sitter_javascript();
    const TSLanguage* tree_sitter_typescript();
    const TSLanguage* tree_sitter_go();
    const TSLanguage* tree_sitter_rust();
    const TSLanguage* tree_sitter_java();
    const TSLanguage* tree_sitter_c_sharp();
}

namespace locus {

TreeSitterRegistry::TreeSitterRegistry()
    : parser_(ts_parser_new())
{
    langs_["c"]          = tree_sitter_c();
    langs_["cpp"]        = tree_sitter_cpp();
    langs_["python"]     = tree_sitter_python();
    langs_["javascript"] = tree_sitter_javascript();
    langs_["typescript"] = tree_sitter_typescript();
    langs_["go"]         = tree_sitter_go();
    langs_["rust"]       = tree_sitter_rust();
    langs_["java"]       = tree_sitter_java();
    langs_["csharp"]     = tree_sitter_c_sharp();

    spdlog::trace("TreeSitterRegistry initialised with {} grammars", langs_.size());
}

TreeSitterRegistry::~TreeSitterRegistry()
{
    if (parser_) {
        ts_parser_delete(parser_);
        parser_ = nullptr;
    }
}

const TSLanguage* TreeSitterRegistry::language_for_name(const std::string& name) const
{
    auto it = langs_.find(name);
    return (it != langs_.end()) ? it->second : nullptr;
}

} // namespace locus
