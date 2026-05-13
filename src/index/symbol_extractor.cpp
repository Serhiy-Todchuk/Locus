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
bool is_identifier_type(const char* t)
{
    return std::strcmp(t, "identifier")        == 0
        || std::strcmp(t, "type_identifier")   == 0
        || std::strcmp(t, "field_identifier")  == 0
        || std::strcmp(t, "simple_identifier") == 0;
}

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

    if (is_identifier_type(ts_node_type(node))) {
        return node_text(node, source);
    }

    // Fallback for grammars (e.g. Kotlin) whose class/function/object
    // declarations do not expose a "name" field -- the identifier is just
    // the first identifier-shaped named child. Bounded to direct children.
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode child = ts_node_named_child(node, i);
        if (is_identifier_type(ts_node_type(child))) {
            return node_text(child, source);
        }
    }

    return {};
}

// -- Rule-based extractor -----------------------------------------------------

// Kinds that name an enclosing container; when a node matches one of these
// kinds, its descendants inherit its name as their `parent_name`.
bool is_container_kind(std::string_view kind)
{
    return kind == "class"
        || kind == "struct"
        || kind == "module"
        || kind == "namespace"
        || kind == "interface"
        || kind == "trait"
        || kind == "object";
}

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

        // Bounded depth covers the worst real-world nesting (Ruby
        // `module > body_statement > class > body_statement > method`,
        // PHP namespace + class + method, Swift extension + class + method)
        // without descending into expression bodies indefinitely.
        constexpr int k_max_depth = 8;

        walk(root, /*parent=*/{}, k_max_depth, source, out);
        return out;
    }

private:
    void walk(TSNode node, const std::string& parent_name,
              int depth_remaining, std::string_view source,
              std::vector<ExtractedSymbol>& out) const
    {
        std::string child_parent = parent_name;

        const char* type = ts_node_type(node);
        if (auto it = type_to_kind_.find(type); it != type_to_kind_.end()) {
            std::string name = extract_name_from_node(node, source);
            if (!name.empty()) {
                TSPoint start = ts_node_start_point(node);
                TSPoint end   = ts_node_end_point(node);

                std::string sig = first_line(node_text(node, source));
                if (sig.size() > 200) sig = sig.substr(0, 200) + "...";

                ExtractedSymbol sym;
                sym.line_start  = static_cast<int>(start.row) + 1;
                sym.line_end    = static_cast<int>(end.row) + 1;
                sym.kind        = it->second;
                sym.signature   = std::move(sig);
                sym.parent_name = parent_name;

                if (is_container_kind(it->second)) child_parent = name;
                sym.name = std::move(name);

                out.push_back(std::move(sym));
            }
        }

        if (depth_remaining <= 0) return;

        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            walk(ts_node_child(node, i), child_parent,
                 depth_remaining - 1, source, out);
        }
    }

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
    // C has its own rule set (no classes/namespaces, has typedefs + unions);
    // JS and TS share rules. Each registration owns a separate extractor
    // instance because the registry takes unique_ptr.
    reg.register_extractor("c",          make_c_symbol_extractor());
    reg.register_extractor("cpp",        make_cpp_symbol_extractor());
    reg.register_extractor("python",     make_python_symbol_extractor());
    reg.register_extractor("javascript", make_js_ts_symbol_extractor());
    reg.register_extractor("typescript", make_js_ts_symbol_extractor());
    reg.register_extractor("go",         make_go_symbol_extractor());
    reg.register_extractor("rust",       make_rust_symbol_extractor());
    reg.register_extractor("java",       make_java_symbol_extractor());
    reg.register_extractor("csharp",     make_csharp_symbol_extractor());
    // S4.Y additions
    reg.register_extractor("ruby",       make_ruby_symbol_extractor());
    reg.register_extractor("php",        make_php_symbol_extractor());
    reg.register_extractor("bash",       make_bash_symbol_extractor());
    reg.register_extractor("swift",      make_swift_symbol_extractor());
    reg.register_extractor("kotlin",     make_kotlin_symbol_extractor());
}

} // namespace locus
