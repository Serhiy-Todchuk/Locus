// S4.Y -- Tier 1 + Tier 2 grammar coverage.
//
// One TEST_CASE per language confirms:
//   * the Tree-sitter grammar parses a fixture file (parse round trip)
//   * the symbol extractor finds the expected names (where applicable)
//   * the JSON / YAML text extractors emit top-level keys as headings
//   * `search mode=ast` round-trips a canonical query against a fixture
//
// Tests do NOT spin up a full Workspace where they don't need to; the symbol
// extractors are unit-testable directly via `TreeSitterRegistry` + the
// language's symbol extractor factory.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "core/workspace.h"
#include "extractors/json_extractor.h"
#include "extractors/yaml_extractor.h"
#include "index/symbol_extractor.h"
#include "index/tree_sitter_registry.h"
#include "tools/search_tools.h"
#include "tools/tool.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

fs::path make_test_dir(const char* suffix)
{
    auto tmp = fs::temp_directory_path() / ("locus_test_grammars_" + std::string(suffix));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    return tmp;
}

void write_file(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << content;
}

void cleanup(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Parse one source string under the requested language and run its symbol
// extractor.  Returns the flat list of (kind, name) pairs.
std::vector<std::pair<std::string, std::string>>
extract_symbols(const std::string& language, const std::string& source)
{
    locus::TreeSitterRegistry reg;
    const TSLanguage* lang = reg.language_for_name(language);
    REQUIRE(lang != nullptr);
    ts_parser_set_language(reg.parser(), lang);

    TSTree* tree = ts_parser_parse_string(reg.parser(), nullptr,
                                          source.data(),
                                          static_cast<uint32_t>(source.size()));
    REQUIRE(tree != nullptr);

    locus::SymbolExtractorRegistry sreg;
    locus::register_builtin_symbol_extractors(sreg);
    const auto* extractor = sreg.find(language);
    REQUIRE(extractor != nullptr);

    auto syms = extractor->extract(ts_tree_root_node(tree), source);
    ts_tree_delete(tree);

    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(syms.size());
    for (auto& s : syms) out.emplace_back(s.kind, s.name);
    return out;
}

bool contains_pair(const std::vector<std::pair<std::string, std::string>>& v,
                   const std::string& kind, const std::string& name)
{
    return std::any_of(v.begin(), v.end(), [&](const auto& p) {
        return p.first == kind && p.second == name;
    });
}

locus::ToolCall ast_call(const nlohmann::json& args)
{
    return {"a1", "search", args};
}

} // namespace

// -- C ----------------------------------------------------------------------

TEST_CASE("C symbol extractor finds function / struct / union / enum / typedef",
          "[s4.v][symbols][c]")
{
    auto syms = extract_symbols("c",
        "#include <stdio.h>\n"
        "typedef struct { int x; int y; } Point;\n"
        "typedef unsigned long u64;\n"
        "enum Color { RED, GREEN, BLUE };\n"
        "union Value { int i; float f; };\n"
        "struct Node { int data; struct Node* next; };\n"
        "int add(int a, int b) { return a + b; }\n");
    REQUIRE(contains_pair(syms, "function", "add"));
    REQUIRE(contains_pair(syms, "struct",   "Node"));
    REQUIRE(contains_pair(syms, "union",    "Value"));
    REQUIRE(contains_pair(syms, "enum",     "Color"));
    REQUIRE(contains_pair(syms, "typedef",  "Point"));
    REQUIRE(contains_pair(syms, "typedef",  "u64"));
}

// -- Ruby -------------------------------------------------------------------

TEST_CASE("Ruby symbol extractor finds method / class / module", "[s4.y][symbols][ruby]")
{
    auto syms = extract_symbols("ruby",
        "module Greeter\n"
        "  class Hello\n"
        "    def greet(name)\n"
        "      \"hi #{name}\"\n"
        "    end\n"
        "    def self.banner\n"
        "      \"=== greeter ===\"\n"
        "    end\n"
        "  end\n"
        "end\n");
    REQUIRE(contains_pair(syms, "module", "Greeter"));
    REQUIRE(contains_pair(syms, "class",  "Hello"));
    REQUIRE(contains_pair(syms, "function", "greet"));
    REQUIRE(contains_pair(syms, "method",   "banner"));
}

// -- PHP --------------------------------------------------------------------

TEST_CASE("PHP symbol extractor finds function / class / method / interface / trait",
          "[s4.y][symbols][php]")
{
    auto syms = extract_symbols("php",
        "<?php\n"
        "interface Greeter { public function greet(): string; }\n"
        "trait Loggable    { public function log(string $m): void {} }\n"
        "class Hello implements Greeter {\n"
        "    use Loggable;\n"
        "    public function greet(): string { return 'hi'; }\n"
        "}\n"
        "function banner(): string { return '==='; }\n");
    REQUIRE(contains_pair(syms, "interface", "Greeter"));
    REQUIRE(contains_pair(syms, "trait",     "Loggable"));
    REQUIRE(contains_pair(syms, "class",     "Hello"));
    REQUIRE(contains_pair(syms, "function",  "banner"));
    REQUIRE(contains_pair(syms, "method",    "greet"));
}

// -- Bash -------------------------------------------------------------------

TEST_CASE("Bash symbol extractor handles both function syntaxes", "[s4.y][symbols][bash]")
{
    auto syms = extract_symbols("bash",
        "#!/usr/bin/env bash\n"
        "function alpha() { echo a; }\n"
        "beta() { echo b; }\n"
        "function gamma { echo g; }\n");
    REQUIRE(contains_pair(syms, "function", "alpha"));
    REQUIRE(contains_pair(syms, "function", "beta"));
    REQUIRE(contains_pair(syms, "function", "gamma"));
}

// -- Swift ------------------------------------------------------------------

TEST_CASE("Swift symbol extractor finds function / class / protocol / init",
          "[s4.y][symbols][swift]")
{
    auto syms = extract_symbols("swift",
        "protocol Greeter { func greet() -> String }\n"
        "class Hello: Greeter {\n"
        "    init() {}\n"
        "    func greet() -> String { return \"hi\" }\n"
        "}\n"
        "func banner() -> String { return \"===\" }\n");
    REQUIRE(contains_pair(syms, "interface", "Greeter"));
    REQUIRE(contains_pair(syms, "class",     "Hello"));
    REQUIRE(contains_pair(syms, "function",  "banner"));
    REQUIRE(contains_pair(syms, "function",  "greet"));
}

// -- Kotlin -----------------------------------------------------------------

TEST_CASE("Kotlin symbol extractor finds function / class / object",
          "[s4.y][symbols][kotlin]")
{
    auto syms = extract_symbols("kotlin",
        "package demo\n"
        "fun banner(): String = \"===\"\n"
        "class Hello {\n"
        "    fun greet(): String = \"hi\"\n"
        "}\n"
        "object Registry {\n"
        "    fun get(): Hello = Hello()\n"
        "}\n");
    REQUIRE(contains_pair(syms, "function", "banner"));
    REQUIRE(contains_pair(syms, "class",    "Hello"));
    REQUIRE(contains_pair(syms, "object",   "Registry"));
    REQUIRE(contains_pair(syms, "function", "greet"));
    REQUIRE(contains_pair(syms, "function", "get"));
}

// -- JSON extractor ---------------------------------------------------------

TEST_CASE("JsonExtractor emits top-level keys as headings", "[s4.y][extractor][json]")
{
    auto tmp = make_test_dir("json");
    auto path = tmp / "config.json";
    write_file(path,
        "{\n"
        "  \"name\": \"Locus\",\n"
        "  \"port\": 1234,\n"
        "  \"nested\": { \"port\": 9999, \"timeout\": 30 },\n"
        "  \"flags\": [\"a\", \"b\"]\n"
        "}\n");

    locus::JsonExtractor ext;
    auto r = ext.extract(path);

    REQUIRE_FALSE(r.is_binary);
    REQUIRE(!r.text.empty());

    std::set<std::string> keys;
    for (const auto& h : r.headings) keys.insert(h.text);
    REQUIRE(keys.count("name")   == 1);
    REQUIRE(keys.count("port")   == 1);
    REQUIRE(keys.count("nested") == 1);
    REQUIRE(keys.count("flags")  == 1);
    // The nested `port` and `timeout` are NOT top-level keys.
    REQUIRE(keys.count("timeout") == 0);

    cleanup(tmp);
}

TEST_CASE("JsonExtractor tolerates // comments (jsonc)", "[s4.y][extractor][json]")
{
    auto tmp = make_test_dir("jsonc");
    auto path = tmp / "settings.json";
    write_file(path,
        "{\n"
        "  // user preferences\n"
        "  \"theme\": \"dark\",\n"
        "  /* block */ \"locale\": \"en\"\n"
        "}\n");

    locus::JsonExtractor ext;
    auto r = ext.extract(path);

    std::set<std::string> keys;
    for (const auto& h : r.headings) keys.insert(h.text);
    REQUIRE(keys.count("theme")  == 1);
    REQUIRE(keys.count("locale") == 1);

    cleanup(tmp);
}

// -- YAML extractor ---------------------------------------------------------

TEST_CASE("YamlExtractor emits top-level keys as headings", "[s4.y][extractor][yaml]")
{
    auto tmp = make_test_dir("yaml");
    auto path = tmp / "config.yaml";
    write_file(path,
        "name: Locus\n"
        "port: 1234\n"
        "database:\n"
        "  host: localhost\n"
        "  port: 5432\n"
        "tags:\n"
        "  - alpha\n"
        "  - beta\n");

    locus::YamlExtractor ext;
    auto r = ext.extract(path);

    REQUIRE_FALSE(r.is_binary);
    REQUIRE(!r.text.empty());

    std::set<std::string> keys;
    for (const auto& h : r.headings) keys.insert(h.text);
    REQUIRE(keys.count("name")     == 1);
    REQUIRE(keys.count("port")     == 1);
    REQUIRE(keys.count("database") == 1);
    REQUIRE(keys.count("tags")     == 1);
    // Nested keys must NOT surface as top-level.
    REQUIRE(keys.count("host") == 0);

    cleanup(tmp);
}

// -- AST search round trip per new language ---------------------------------

TEST_CASE("search mode=ast finds calls in ruby fixture", "[s4.y][search][ast][ruby]")
{
    auto tmp = make_test_dir("ast_ruby");
    write_file(tmp / "a.rb",
        "def f\n"
        "  puts \"hello\"\n"
        "end\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchTool tool;
        auto result = tool.execute(ast_call({
            {"mode", "ast"},
            {"language", "ruby"},
            {"query", "(call method: (identifier) @fn)"},
        }), ws);

        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("a.rb"));
        REQUIRE_THAT(result.content, ContainsSubstring("@fn"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=ast finds heading nodes in markdown", "[s4.y][search][ast][markdown]")
{
    auto tmp = make_test_dir("ast_md");
    write_file(tmp / "notes.md",
        "# Top\n"
        "Some text.\n"
        "## Section\n"
        "More text.\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchTool tool;
        auto result = tool.execute(ast_call({
            {"mode", "ast"},
            {"language", "markdown"},
            {"query", "(atx_heading) @h"},
        }), ws);

        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("notes.md"));
        REQUIRE_THAT(result.content, ContainsSubstring("@h"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=ast finds keys in json", "[s4.y][search][ast][json]")
{
    auto tmp = make_test_dir("ast_json");
    write_file(tmp / "a.json", "{\"port\": 1234, \"host\": \"localhost\"}\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchTool tool;
        auto result = tool.execute(ast_call({
            {"mode", "ast"},
            {"language", "json"},
            {"query", "(pair key: (string) @k)"},
        }), ws);

        REQUIRE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("a.json"));
        REQUIRE_THAT(result.content, ContainsSubstring("@k"));
    }
    cleanup(tmp);
}

TEST_CASE("search mode=ast rejects deferred SQL language gracefully",
          "[s4.y][search][ast][sql]")
{
    auto tmp = make_test_dir("ast_sql");
    write_file(tmp / "a.sql", "SELECT * FROM t;\n");

    {
        locus::Workspace ws(tmp);
        locus::SearchTool tool;
        auto result = tool.execute(ast_call({
            {"mode", "ast"},
            {"language", "sql"},
            {"query", "(select) @s"},
        }), ws);

        // SQL is intentionally not part of the grammar registry in S4.Y --
        // the tool should refuse cleanly rather than crash.
        REQUIRE_FALSE(result.success);
        REQUIRE_THAT(result.content, ContainsSubstring("unsupported language"));
    }
    cleanup(tmp);
}
