#pragma once

#include "security/injection_scanner.h"

#include <cstdint>
#include <string>

// S6.0 Task C + D -- ingress policy + taint-surface renderer.
//
// The SCANNER (injection_scanner.h) decides *what a span looks like*. The
// POLICY here decides *what to do about it*. Default is NOT hard-block -- a
// security blog that quotes "ignore previous instructions" legitimately trips
// the scanner, and a false-positive block would silently break a fetch. The
// default is wrap + annotate: fence the untrusted body in a clearly-labelled,
// per-call-nonce delimiter so both the user (banner) and the model (in-band)
// know the span is untrusted. High-confidence Exfiltration escalates the tool's
// approval policy to `ask` so the user sees it before the result lands.
//
// None of this is a guarantee -- a small local model honors in-band delimiters
// unreliably (see the threat model). The real control is the approval gate;
// this just makes a hijack attempt visible.

namespace locus::security {

// Mirrors the WorkspaceConfig::Security sub-struct, decoupled so this pure
// module doesn't depend on the config header. The wiring layer copies the
// relevant fields across.
struct SecurityConfig {
    bool  injection_scan   = true;   // scan web + MCP ingress (not ZIM by default)
    bool  scan_zim         = false;  // opt-in keyword scan over ZIM content
    float block_confidence = 0.85f;  // escalate-to-ask threshold
    int   max_scan_kb      = 256;    // -> ScannerConfig::max_scan_bytes
};

enum class IngressAction {
    Pass,     // clean (or all sub-threshold) -- content untouched
    Wrap,     // findings present -- content fenced + banner, approval unchanged
    Escalate  // high-confidence (esp. Exfiltration) -- fenced + banner + force `ask`
};

struct IngressDecision {
    IngressAction action = IngressAction::Pass;
    std::string   banner;  // one-line human-facing summary (empty for Pass)
};

// Apply the policy to a freshly-scanned piece of untrusted ingress text.
// Mutates `content` in place when wrapping (fences body with a per-call nonce
// delimiter and prepends the in-band untrusted note + detected-category line +
// truncation note). Returns the action + a short banner for the approval panel
// / activity log. A Pass leaves `content` untouched and returns an empty banner.
IngressDecision apply_injection_policy(std::string& content,
                                       const ScanResult& scan,
                                       const SecurityConfig& cfg);

// -- Taint-surface renderer (Task D) ----------------------------------------
//
// Pure helper that the search read path reuses. Given a snippet that came from
// a tainted-origin row, prepend an origin marker -- and, when the row carried
// injection flags at ingress, re-fence the snippet with the same nonce wrap so
// the untrusted framing reaches the model at the MOMENT it sees the text, not
// only at fetch time (the structural gap ingress-only scanning leaves open).
//
//   origin          e.g. "web", "zim", "mcp" (empty = trusted, returns snippet unchanged)
//   injection_flags the bitmask persisted at ingress (0 = clean)
//   display_origin  human label for the marker (e.g. "wikipedia" for zim); empty -> origin
//
// The origin marker is UNCONDITIONAL for any non-empty origin even when
// injection_flags == 0 -- "this came from the web/wikipedia" is itself
// information the model and user should always have. The wrap is conditional on
// flags. Nonce is regenerated per call (this helper is pure aside from RNG).
std::string render_tainted_snippet(const std::string& snippet,
                                   const std::string& origin,
                                   uint32_t injection_flags,
                                   const std::string& display_origin = "");

// Compact one-line origin marker, e.g. "[wikipedia, untrusted]". Exposed for
// callers that want the marker without re-fencing (e.g. when batching).
std::string origin_marker(const std::string& origin,
                          uint32_t injection_flags,
                          const std::string& display_origin = "");

// Generate a fresh hex nonce that does NOT appear in `body`. Exposed for tests.
std::string make_fence_nonce(const std::string& body);

} // namespace locus::security
