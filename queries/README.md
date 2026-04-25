# Curated Tree-sitter Query Library (S4.M)

These are reference S-expression patterns for `search` `mode=ast`. Copy the
contents of any `.scm` file into the tool's `query` argument (and pass the
matching `language`).

The files are not loaded by Locus -- they're a starter kit for the LLM and
for human users. The LLM can read them via `read_file` when it needs an
example, and can compose its own queries the same way.

## Quick reference

| Language    | Files                                                       |
|-------------|-------------------------------------------------------------|
| C/C++       | `cpp/find-call.scm`, `cpp/find-class.scm`, `cpp/empty-if.scm` |
| Python      | `python/find-call.scm`, `python/find-import.scm`            |
| TypeScript  | `typescript/find-import.scm`, `typescript/find-call.scm`    |
| Rust        | `rust/find-call.scm`                                        |
| Go          | `go/find-call.scm`                                          |

## Tree-sitter query primer

- Patterns are S-expressions over the grammar's node tree.
- `(node_type)` matches any node of that kind.
- `(node_type field_name: child_pattern)` matches a parent whose named
  `field_name` child matches the inner pattern.
- `@name` captures a node so the tool can report it.
- `(#eq? @cap "literal")` constrains a capture to an exact text match.
- `(#match? @cap "regex")` constrains a capture to a regex.

See the [Tree-sitter query syntax docs](https://tree-sitter.github.io/tree-sitter/using-parsers/queries/index.html)
for the full grammar.
