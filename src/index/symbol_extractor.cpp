#include "symbol_extractor.h"

#include <tree_sitter/api.h>

#include <cstring>
#include <unordered_map>
#include <utility>

namespace locus {

// -- Common helpers -----------------------------------------------------------

namespace {

std::string node_text(TSNode node, std::string_view source)
{
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    if (start >= source.size() || end > source.size() || start >= end) return {};
    return std::string(source.substr(start, end - start));
}

std::string first_line(const std::string& text)
{
    auto pos = text.find('\n');
    if (pos == std::string::npos) return text;
    if (pos > 0 && text[pos - 1] == '\r') --pos;
    return text.substr(0, pos);
}

// Walk declarator chains (function_declarator -> declarator -> identifier) to
// find the readable name of a declaration node. Tree-sitter exposes this via
// the "name"/"declarator" field hierarchy.
std::string extract_name_from_node(TSNode node, std::string_view source)
{
    TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
    if (!ts_node_is_null(name_node)) {
        return node_text(name_node, source);
    }

    TSNode decl = ts_node_child_by_field_name(node, "declarator", 10);
    if (!ts_node_is_null(decl)) {
        return extract_name_from_node(decl, source);
    }

    const char* type = ts_node_type(node);
    if (std::strcmp(type, "identifier") == 0 ||
        std::strcmp(type, "type_identifier") == 0 ||
        std::strcmp(type, "field_identifier") == 0) {
        return node_text(node, source);
    }

    return {};
}

// -- Rule-based extractor -----------------------------------------------------

class RuleBasedSymbolExtractor : public ISymbolExtractor {
public:
    explicit RuleBasedSymbolExtractor(std::vector<SymbolRule> rules)
        : rules_(std::move(rules))
    {
        for (const auto& r : rules_) type_to_kind_[r.node_type] = r.kind;
    }

    std::vector<ExtractedSymbol> extract(TSNode root, std::string_view source) const override
    {
        std::vector<ExtractedSymbol> out;
        if (rules_.empty()) return out;

        auto try_extract = [&](TSNode node, const std::string& parent_name) {
            const char* type = ts_node_type(node);
            auto it = type_to_kind_.find(type);
            if (it == type_to_kind_.end()) return;

            std::string name = extract_name_from_node(node, source);
            if (name.empty()) return;

            TSPoint start = ts_node_start_point(node);
            TSPoint end   = ts_node_end_point(node);

            std::string sig = first_line(node_text(node, source));
            if (sig.size() > 200) sig = sig.substr(0, 200) + "...";

            ExtractedSymbol sym;
            sym.line_start  = static_cast<int>(start.row) + 1;
            sym.line_end    = static_cast<int>(end.row) + 1;
            sym.kind        = it->second;
            sym.name        = std::move(name);
            sym.signature   = std::move(sig);
            sym.parent_name = parent_name;
            out.push_back(std::move(sym));
        };

        uint32_t child_count = ts_node_child_count(root);
        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_child(root, i);
            try_extract(child, "");

            std::string parent_name = extract_name_from_node(child, source);
            uint32_t grandchild_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < grandchild_count; ++j) {
                TSNode grandchild = ts_node_child(child, j);
                try_extract(grandchild, parent_name);

                uint32_t ggc = ts_node_child_count(grandchild);
                for (uint32_t k = 0; k < ggc; ++k) {
                    try_extract(ts_node_child(grandchild, k), parent_name);
                }
            }
        }

        return out;
    }

private:
    std::vector<SymbolRule> rules_;
    std::unordered_map<std::string, const char*> type_to_kind_;
};

} // namespace

// -- Public factory -----------------------------------------------------------

std::unique_ptr<ISymbolExtractor> make_rule_based_symbol_extractor(
    std::vector<SymbolRule> rules)
{
    return std::make_unique<RuleBasedSymbolExtractor>(std::move(rules));
}

// -- Registry -----------------------------------------------------------------

void SymbolExtractorRegistry::register_extractor(std::string language,
                                                 std::unique_ptr<ISymbolExtractor> extractor)
{
    map_[std::move(language)] = std::move(extractor);
}

const ISymbolExtractor* SymbolExtractorRegistry::find(const std::string& language) const
{
    auto it = map_.find(language);
    return it != map_.end() ? it->second.get() : nullptr;
}

void register_builtin_symbol_extractors(SymbolExtractorRegistry& reg)
{
    // C and C++ share rules; JS and TS share rules. Each registration owns a
    // separate extractor instance because the registry takes unique_ptr.
    reg.register_extractor("c",          make_cpp_symbol_extractor());
    reg.register_extractor("cpp",        make_cpp_symbol_extractor());
    reg.register_extractor("python",     make_python_symbol_extractor());
    reg.register_extractor("javascript", make_js_ts_symbol_extractor());
    reg.register_extractor("typescript", make_js_ts_symbol_extractor());
    reg.register_extractor("go",         make_go_symbol_extractor());
    reg.register_extractor("rust",       make_rust_symbol_extractor());
    reg.register_extractor("java",       make_java_symbol_extractor());
    reg.register_extractor("csharp",     make_csharp_symbol_extractor());
}

} // namespace locus
