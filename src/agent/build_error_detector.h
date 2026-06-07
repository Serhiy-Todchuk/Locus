#pragma once

// S6.20 -- "stuck on the same build error" detection.
//
// An agentic session driving a hosted model through a large-file build
// (the DX12 + DXR app, 2026-06-06) thrashed for 38-72 rounds: the model
// rewrote the whole file each round and reintroduced the SAME compiler
// errors, with no signal that it was looping. The repeated-tool-call
// QualityMonitor (S6.10) only catches identical (name, args) calls -- here
// the calls differ each round (different rewrites) but the resulting build
// error is the same, so that detector never fires.
//
// `extract_error_signature` distills a tool result's body (typically a
// run_command output carrying compiler / linker diagnostics) into a stable
// signature that is invariant to the noise that changes round-to-round but
// is irrelevant to "is this the same failure": line/column numbers, absolute
// paths, hex addresses, and the order diagnostics appear in. Two builds that
// fail the same way produce the same signature; the caller counts consecutive
// identical signatures and nudges / aborts when the streak crosses a budget.
//
// Pure + dependency-free so it unit-tests without a live turn or LLM.

#include <string>

namespace locus {

// Returns a normalized signature for the error diagnostics in `tool_output`,
// or an empty string when the output contains no recognizable build error
// (a successful build, a non-build tool result, plain text). The signature
// is a sorted, de-duplicated, newline-joined list of normalized diagnostic
// keys -- e.g. "error C2102:'&' requires l-value" with the file/line prefix
// and any numbers stripped. Stable across reorderings and line-number drift.
//
// Recognized diagnostic shapes (covers the toolchains Locus drives on
// Windows + common cross-platform output):
//   - MSVC:   "...path(line,col): error C2102: '&' requires l-value"
//   - MSVC:   "... : fatal error LNK1120: 1 unresolved externals"
//   - GCC/Clang: "path:line:col: error: expected ';' before '}'"
//   - generic: a line containing " error " / "error:" with a code/message
std::string extract_error_signature(const std::string& tool_output);

} // namespace locus
