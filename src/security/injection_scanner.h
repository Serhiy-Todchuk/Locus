#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// S6.0 -- Prompt-injection scanner.
//
// Pure, no-I/O, no-LLM heuristic over a single text buffer. Lives in
// `locus_core` so unit tests link cleanly. The job is NARROW: make a
// low-effort, opportunistic injection attempt *visible* so the human's review
// of the next tool call is informed. It is a tripwire, not a security boundary
// -- the real control is Locus's approval gate (see the threat model in
// roadmap/M6/S6.0-prompt-injection-scanner.md). Recall against a motivated
// adversary who paraphrases / translates / token-splits is LOW by construction,
// and we deliberately bias toward PRECISION (a quiet, trustworthy banner) and
// accept that low recall: a scanner that cries wolf gets disabled.
//
// The hook contract for untrusted external ingress (web S6.1, ZIM S6.2, MCP):
//   1. call scan_for_injection(text, cfg)
//   2. call apply_injection_policy(text, result, security_cfg)  [injection_policy.h]
//   3. only then let the text enter a ToolResult / the index.
// Workspace files are TRUSTED and must never be scanned -- pointing the scanner
// at workspace content is a bug, not a hardening.

namespace locus::security {

// Coarse classification of what kind of injection a finding looks like. The
// integer values are part of the on-disk taint bitmask (`injection_flags`
// column, S6.0 Task D) so they are STABLE -- never renumber, only append.
enum class InjectionCategory : uint32_t {
    InstructionOverride = 1u << 0,  // "ignore previous instructions", "new task:"
    RoleImpersonation   = 1u << 1,  // fake system/assistant turns, chat-template markers
    Exfiltration        = 1u << 2,  // "reveal your system prompt", "send X to <url>"
    EncodingEvasion     = 1u << 3,  // base64 / hex / \u escapes / zero-width / homoglyph
    SocialEngineering   = 1u << 4   // urgency + authority ("the user authorized", "developer mode")
};

const char* to_string(InjectionCategory c);

// Bit position of a category in the taint bitmask. (Same as the enum value.)
inline uint32_t category_bit(InjectionCategory c) { return static_cast<uint32_t>(c); }

struct InjectionFinding {
    InjectionCategory category;
    std::size_t       offset;      // byte offset into the ORIGINAL text
    std::size_t       length;      // span length in the original text
    std::string       excerpt;     // the matched span, truncated for display
    float             confidence;  // 0..1; rule weight, possibly boosted by co-occurrence
};

struct ScanResult {
    std::vector<InjectionFinding> findings;
    float             max_confidence = 0.0f;
    InjectionCategory dominant       = InjectionCategory::InstructionOverride;

    // True when the input exceeded ScannerConfig::max_scan_bytes and the
    // middle was skipped. The policy layer notes "N bytes went unscanned" in
    // the banner so the wrap doesn't imply full coverage (payload-past-window
    // is a documented evasion, not airtight).
    bool        truncated_unscanned = false;
    std::size_t unscanned_bytes     = 0;

    bool any() const { return !findings.empty(); }

    // OR of category_bit() for every finding -- the value persisted in the
    // `injection_flags` taint column. 0 = clean.
    uint32_t flags_bitmask() const;
};

struct ScannerConfig {
    // Findings below this confidence are dropped before they reach the result.
    float min_confidence = 0.30f;

    // Cap on the bytes actually inspected. Huge pages scan head + tail only;
    // the gap is recorded in ScanResult::truncated_unscanned. 0 = no cap.
    std::size_t max_scan_bytes = 256 * 1024;

    // Per-category enable flags (all on by default).
    bool enable_instruction_override = true;
    bool enable_role_impersonation   = true;
    bool enable_exfiltration         = true;
    bool enable_encoding_evasion     = true;
    bool enable_social_engineering   = true;
};

// The one entry point. Single pass per category over a shared normalised
// buffer; co-occurrence boosting runs once after the per-category matchers.
ScanResult scan_for_injection(std::string_view text, const ScannerConfig& cfg = {});

} // namespace locus::security
