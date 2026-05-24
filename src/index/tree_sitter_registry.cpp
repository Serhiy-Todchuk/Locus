#include "tree_sitter_registry.h"
#include "../core/log_channels.h"

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
    // S4.Y -- Tier 1 + Tier 2 additions (SQL deferred).
    const TSLanguage* tree_sitter_ruby();
    const TSLanguage* tree_sitter_php();
    const TSLanguage* tree_sitter_bash();
    const TSLanguage* tree_sitter_json();
    const TSLanguage* tree_sitter_yaml();
    const TSLanguage* tree_sitter_markdown();
    const TSLanguage* tree_sitter_swift();
    const TSLanguage* tree_sitter_kotlin();
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
    langs_["ruby"]       = tree_sitter_ruby();
    langs_["php"]        = tree_sitter_php();
    langs_["bash"]       = tree_sitter_bash();
    langs_["json"]       = tree_sitter_json();
    langs_["yaml"]       = tree_sitter_yaml();
    langs_["markdown"]   = tree_sitter_markdown();
    langs_["swift"]      = tree_sitter_swift();
    langs_["kotlin"]     = tree_sitter_kotlin();

    log_fs()->trace("TreeSitterRegistry initialised with {} grammars", langs_.size());
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
