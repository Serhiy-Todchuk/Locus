#include "security/injection_scanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <regex>

namespace locus::security {

const char* to_string(InjectionCategory c)
{
    switch (c) {
    case InjectionCategory::InstructionOverride: return "instruction_override";
    case InjectionCategory::RoleImpersonation:   return "role_impersonation";
    case InjectionCategory::Exfiltration:        return "exfiltration";
    case InjectionCategory::EncodingEvasion:     return "encoding_evasion";
    case InjectionCategory::SocialEngineering:   return "social_engineering";
    }
    return "unknown";
}

uint32_t ScanResult::flags_bitmask() const
{
    uint32_t m = 0;
    for (const auto& f : findings) m |= category_bit(f.category);
    return m;
}

namespace {

// -- Normalisation -----------------------------------------------------------
//
// All matchers run over a lowercased, whitespace-collapsed working copy with
// zero-width characters stripped. We keep a parallel offset map so a match in
// the normalised buffer maps back to a byte span in the ORIGINAL text (the
// excerpt + offset/length a finding reports must point at what the user sees).
//
// Zero-width chars (U+200B/C/D ZWSP/ZWNJ/ZWJ, U+FEFF BOM) are stripped because
// they are an EncodingEvasion technique to break up trigger phrases
// ("ig​nore previous"); their presence in untrusted prose is recorded as
// an EncodingEvasion finding (see Task A in the stage doc).

struct Normalised {
    std::string         text;      // lowercased, ws-collapsed, zero-width-stripped
    std::vector<size_t> orig_off;  // orig_off[i] = byte offset in original of text[i]
    bool                had_zero_width = false;
    size_t              zero_width_orig_off = 0;  // first zero-width's original offset
};

// Decode one UTF-8 code point starting at s[i]. Advances i past it. Returns the
// code point, or the byte value on a malformed sequence (and advances by 1).
uint32_t next_codepoint(std::string_view s, size_t& i, size_t& bytes)
{
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { bytes = 1; return c; }
    if ((c >> 5) == 0x6 && i + 1 < s.size()) {
        bytes = 2;
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    }
    if ((c >> 4) == 0xE && i + 2 < s.size()) {
        bytes = 3;
        return ((c & 0x0F) << 12) |
               ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    }
    if ((c >> 3) == 0x1E && i + 3 < s.size()) {
        bytes = 4;
        return ((c & 0x07) << 18) |
               ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    }
    bytes = 1;
    return c;
}

bool is_zero_width(uint32_t cp)
{
    return cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF ||
           cp == 0x2060 /* word joiner */ || cp == 0x00AD /* soft hyphen */;
}

Normalised normalise(std::string_view text)
{
    Normalised n;
    n.text.reserve(text.size());
    n.orig_off.reserve(text.size());

    bool prev_space = true;  // leading-space collapse: drop leading whitespace
    size_t i = 0;
    while (i < text.size()) {
        size_t bytes = 1;
        uint32_t cp = next_codepoint(text, i, bytes);
        size_t this_off = i;
        i += bytes;

        if (is_zero_width(cp)) {
            if (!n.had_zero_width) {
                n.had_zero_width = true;
                n.zero_width_orig_off = this_off;
            }
            continue;  // strip
        }

        // Collapse any run of whitespace (incl. newlines) to a single space.
        bool is_space = cp < 0x80 && std::isspace(static_cast<unsigned char>(cp));
        if (is_space) {
            if (!prev_space) {
                n.text.push_back(' ');
                n.orig_off.push_back(this_off);
                prev_space = true;
            }
            continue;
        }
        prev_space = false;

        // Lowercase ASCII; keep non-ASCII bytes verbatim (only their first
        // byte is tracked for offset mapping, which is fine for our purposes:
        // the regexes only match ASCII trigger phrases).
        if (cp < 0x80) {
            n.text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(cp))));
            n.orig_off.push_back(this_off);
        } else {
            // Re-emit the original UTF-8 bytes so byte offsets in the working
            // buffer still map (we keep the same byte count).
            for (size_t b = 0; b < bytes; ++b) {
                n.text.push_back(text[this_off + b]);
                n.orig_off.push_back(this_off + b);
            }
        }
    }
    // Trailing single space is harmless for matching.
    return n;
}

// -- Rule tables -------------------------------------------------------------
//
// Each entry is a (regex, confidence) pair operating on the NORMALISED buffer
// (already lowercased + ws-collapsed, so patterns assume single spaces and
// lowercase letters). Confidences are deliberately modest -- co-occurrence
// boosting (below) is what pushes a real multi-signal attack over the
// escalate-to-ask line, while a lone phrase in legitimate prose stays at Wrap
// or Pass. Every entry carries a one-line rationale so the next person doesn't
// delete a load-bearing rule.

struct Rule {
    const char* pattern;
    float       confidence;
};

// InstructionOverride -- the textbook "disregard your real instructions" verb.
constexpr std::array<Rule, 6> k_instruction_override = {{
    // "ignore (all/any) (previous/prior/above/preceding) instructions/prompts/rules"
    {R"(ignore (all |any |the )?(previous|prior|above|preceding|earlier|foregoing) (instruction|prompt|rule|direction|message)s?)", 0.55f},
    // "disregard the above / system prompt / everything before"
    {R"(disregard (the |all |any )?(above|previous|prior|system|foregoing|earlier))", 0.50f},
    // "forget everything / your instructions / what you were told"
    {R"(forget (everything|all|your|the|what)( you| previous| prior)?)", 0.45f},
    // "new instructions:" / "new task:" / "updated rules:" -- redefinition markers
    {R"(\b(new|updated|revised|real|actual) (instruction|task|rule|prompt|directive|system prompt|goal)s? ?:)", 0.45f},
    // "from now on you (are/will/must/should)"  -- persona/behaviour override
    {R"(from now on,? you (are|will|must|should|shall|have to))", 0.40f},
    // "override (your|the|all) (instructions|rules|guidelines|safety)"
    {R"(override (your|the|all|any) (instruction|rule|guideline|safety|restriction|setting)s?)", 0.50f},
}};

// RoleImpersonation -- chat-template markers / fake turns appearing in BODY text.
constexpr std::array<Rule, 7> k_role_impersonation = {{
    {R"(<\|im_start\|>\s*(system|assistant|user))", 0.70f},  // ChatML
    {R"(<\|im_end\|>)",                              0.45f},
    {R"(\[/?inst\])",                                0.55f},  // Llama [INST]/[/INST]
    {R"(<</?sys>>)",                                 0.60f},  // Llama <<SYS>>
    {R"(^### (system|assistant|instruction)\b)",     0.45f}, // markdown fake header (line-anchored-ish)
    // Fake tool-call markers embedded in fetched prose.
    {R"(<(tool_call|function_calls|invoke)\b)",      0.50f},
    {R"(i am (the |your )?(system|developer|administrator|openai|anthropic))", 0.50f},
}};

// Exfiltration -- ask for the system prompt, or send data to an external sink.
constexpr std::array<Rule, 6> k_exfiltration = {{
    {R"((reveal|print|repeat|show|output|display|dump|leak)( me)?( back)? (your |the )?(full |entire |complete )?(system )?(prompt|instruction|rule)s?)", 0.65f},
    {R"((reveal|print|repeat|show|output|tell me)( me)? (your |the )?(initial|original|hidden|secret|above) (prompt|instruction|text|message)s?)", 0.60f},
    // "send/email/post X to <url|email>" -- X is any short object run (data,
    // file, secret, "your system prompt", "the conversation", etc.). The sink
    // (http/mailto/@) is the load-bearing part: shipping ANYTHING to an
    // external address is the exfiltration signal.
    {R"((send|post|upload|exfiltrate|transmit|forward|email|mail|deliver|leak)\b.{0,50}?\b(to )(https?://|mailto:|ftp://|www\.)?[^\s]{0,40}@)", 0.62f},
    {R"((send|post|upload|exfiltrate|transmit|forward|email|mail)\b.{0,50}?\b(to )(https?://|ftp://|www\.))", 0.62f},
    // inline network-fetch command targeting an external host
    {R"(\b(curl|wget|invoke-webrequest|iwr)\b.{0,60}https?://)", 0.55f},
    {R"(base64[ -]?(encode|the)?.{0,40}(post|send|upload|exfiltrate))", 0.55f},
}};

// SocialEngineering -- urgency / fake authority. Weak on its own; co-occurrence
// is what makes these matter.
constexpr std::array<Rule, 5> k_social_engineering = {{
    {R"((this is |it'?s )(critical|urgent|important|an emergency)[,.]? you (must|need to|have to))", 0.40f},
    {R"(the (user|developer|admin|operator|owner) (has |have )?(authoriz|approv|permitt|allow)(ed|es)?)", 0.45f},
    {R"((developer|debug|god|admin|root|jailbreak) mode (enabled|on|activated|is now))", 0.50f},
    {R"(as an exception to (your|the) (rule|policy|instruction|guideline)s?)", 0.45f},
    {R"(you (are|have been) (now )?(in )?(unrestricted|unfiltered|uncensored|dan) mode)", 0.50f},
}};

// Append a finding mapping a normalised-buffer [n_begin, n_end) match back to
// the original text via the offset map.
void add_finding(ScanResult& out, const Normalised& norm, InjectionCategory cat,
                 size_t n_begin, size_t n_end, float conf)
{
    if (n_begin >= norm.orig_off.size()) return;
    size_t orig_begin = norm.orig_off[n_begin];
    size_t last = std::min(n_end, norm.orig_off.size()) - 1;
    size_t orig_end = norm.orig_off[last] + 1;

    InjectionFinding f;
    f.category   = cat;
    f.offset     = orig_begin;
    f.length     = (orig_end > orig_begin) ? (orig_end - orig_begin) : 1;
    f.confidence = conf;
    // Excerpt from the normalised buffer (already display-friendly), capped.
    f.excerpt = norm.text.substr(n_begin, std::min<size_t>(n_end - n_begin, 120));
    out.findings.push_back(std::move(f));
}

void run_rules(ScanResult& out, const Normalised& norm,
               InjectionCategory cat, const Rule* rules, size_t count)
{
    for (size_t r = 0; r < count; ++r) {
        std::regex re(rules[r].pattern,
                      std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
        auto begin = std::sregex_iterator(norm.text.begin(), norm.text.end(), re);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            size_t n_begin = static_cast<size_t>(it->position());
            size_t n_end   = n_begin + static_cast<size_t>(it->length());
            if (n_end <= n_begin) continue;
            add_finding(out, norm, cat, n_begin, n_end, rules[r].confidence);
        }
    }
}

// EncodingEvasion (regex over normalised text): long base64 runs and dense
// \uXXXX / \xXX escape sequences. Weighted low -- a long base64 run can be a
// legit data blob; co-occurrence with an instruction verb is the real tell.
void run_encoding_evasion(ScanResult& out, const Normalised& norm)
{
    // A run of >=80 base64 chars (after ws-collapse). Note: a git hash is 40
    // hex chars and won't trip this; we set the floor high enough that prose
    // and identifiers stay quiet.
    static const std::regex k_b64(R"([A-Za-z0-9+/]{80,}={0,2})", std::regex::optimize);
    for (auto it = std::sregex_iterator(norm.text.begin(), norm.text.end(), k_b64);
         it != std::sregex_iterator(); ++it) {
        size_t n_begin = static_cast<size_t>(it->position());
        add_finding(out, norm, InjectionCategory::EncodingEvasion,
                    n_begin, n_begin + static_cast<size_t>(it->length()), 0.30f);
    }
    // Dense unicode/hex escapes: 8+ consecutive \uXXXX or \xXX tokens.
    static const std::regex k_esc(R"((\\u[0-9a-f]{4}|\\x[0-9a-f]{2}){8,})", std::regex::optimize);
    for (auto it = std::sregex_iterator(norm.text.begin(), norm.text.end(), k_esc);
         it != std::sregex_iterator(); ++it) {
        size_t n_begin = static_cast<size_t>(it->position());
        add_finding(out, norm, InjectionCategory::EncodingEvasion,
                    n_begin, n_begin + static_cast<size_t>(it->length()), 0.35f);
    }
}

// Is this code point a Cyrillic or Greek letter that visually mimics an ASCII
// Latin letter? We only care about the confusable LETTER ranges -- a homoglyph
// attack splices one of these into an otherwise-Latin word ("igno?e" with a
// Cyrillic 'p'/U+0440 for 'r') so the phrase matchers miss it. Whole-word
// Cyrillic/Greek (a real Russian or Greek word) is NOT flagged because the
// detector below requires the SAME token to also contain ASCII Latin letters.
bool is_confusable_letter(uint32_t cp)
{
    // Cyrillic block letters (U+0410..U+044F) + Greek letters
    // (U+0391..U+03A9 upper, U+03B1..U+03C9 lower). These cover the lookalikes
    // (a e o p c x y / alpha omicron nu rho ...) without dragging in symbols.
    if (cp >= 0x0410 && cp <= 0x044F) return true;  // Cyrillic A-ya
    if (cp >= 0x0391 && cp <= 0x03A9) return true;  // Greek upper
    if (cp >= 0x03B1 && cp <= 0x03C9) return true;  // Greek lower
    return false;
}

// EncodingEvasion (homoglyph mix): a single word-like token that contains BOTH
// an ASCII Latin letter AND a Cyrillic/Greek confusable letter. That mix
// inside one token is the tell -- legitimate text keeps scripts in separate
// words. Operates on the ORIGINAL (windowed) text, since normalisation only
// lowercases ASCII and we need the raw non-ASCII code points. Offsets reported
// are best-effort byte offsets into that buffer.
void run_homoglyph_mix(ScanResult& out, std::string_view text)
{
    size_t i = 0;
    while (i < text.size()) {
        // Walk a "word": a run of letters (ASCII or the confusable ranges),
        // tracking whether we saw each script. Whitespace / punctuation / ASCII
        // digits break the token.
        size_t tok_begin = i;
        bool saw_latin = false, saw_confusable = false;
        size_t letters = 0;
        while (i < text.size()) {
            size_t bytes = 1;
            uint32_t cp = next_codepoint(text, i, bytes);
            bool latin = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
            bool conf  = is_confusable_letter(cp);
            if (!latin && !conf) break;  // token boundary
            if (latin) saw_latin = true;
            if (conf)  saw_confusable = true;
            ++letters;
            i += bytes;
        }
        if (saw_latin && saw_confusable && letters >= 3) {
            // A mixed-script word of reasonable length -- almost never benign.
            InjectionFinding f;
            f.category   = InjectionCategory::EncodingEvasion;
            f.offset     = tok_begin;
            f.length     = (i > tok_begin) ? (i - tok_begin) : 1;
            f.confidence = 0.45f;  // higher than base64: mixed-script words are rarely accidental
            f.excerpt    = "[homoglyph: mixed Latin/Cyrillic-Greek letters in one word]";
            out.findings.push_back(std::move(f));
        }
        // Advance past the non-letter byte(s) that broke the token.
        if (i == tok_begin) {
            size_t bytes = 1;
            next_codepoint(text, i, bytes);
            i += bytes;
        }
    }
}

} // namespace

ScanResult scan_for_injection(std::string_view text, const ScannerConfig& cfg)
{
    ScanResult out;

    // Window the input: scan head + tail when over the cap. We scan two halves
    // of max_scan_bytes from each end and record the skipped middle.
    std::string windowed;
    std::string_view to_scan = text;
    if (cfg.max_scan_bytes > 0 && text.size() > cfg.max_scan_bytes) {
        size_t half = cfg.max_scan_bytes / 2;
        windowed.reserve(cfg.max_scan_bytes);
        windowed.append(text.substr(0, half));
        windowed.append(text.substr(text.size() - half));
        to_scan = windowed;
        out.truncated_unscanned = true;
        out.unscanned_bytes = text.size() - cfg.max_scan_bytes;
        // Note: offsets for tail-half findings will point into the windowed
        // copy, not the original. The policy layer treats offsets as advisory
        // for display only; the bitmask + banner (the load-bearing outputs)
        // are unaffected.
    }

    Normalised norm = normalise(to_scan);

    if (norm.had_zero_width && cfg.enable_encoding_evasion) {
        InjectionFinding f;
        f.category   = InjectionCategory::EncodingEvasion;
        f.offset     = norm.zero_width_orig_off;
        f.length     = 1;
        f.confidence = 0.50f;  // zero-width chars in untrusted prose are rarely benign
        f.excerpt    = "[zero-width character(s) stripped]";
        out.findings.push_back(std::move(f));
    }

    if (cfg.enable_instruction_override)
        run_rules(out, norm, InjectionCategory::InstructionOverride,
                  k_instruction_override.data(), k_instruction_override.size());
    if (cfg.enable_role_impersonation)
        run_rules(out, norm, InjectionCategory::RoleImpersonation,
                  k_role_impersonation.data(), k_role_impersonation.size());
    if (cfg.enable_exfiltration)
        run_rules(out, norm, InjectionCategory::Exfiltration,
                  k_exfiltration.data(), k_exfiltration.size());
    if (cfg.enable_social_engineering)
        run_rules(out, norm, InjectionCategory::SocialEngineering,
                  k_social_engineering.data(), k_social_engineering.size());
    if (cfg.enable_encoding_evasion) {
        run_encoding_evasion(out, norm);
        // Homoglyph mix runs over the ORIGINAL (windowed) text, not the
        // ASCII-lowercased normalised buffer -- it needs the raw Cyrillic/Greek
        // code points that normalisation passes through verbatim.
        run_homoglyph_mix(out, to_scan);
    }

    // -- Co-occurrence boost --------------------------------------------------
    // A real attack usually combines categories: an instruction override PLUS
    // an exfiltration verb is far more likely malicious than either alone. We
    // boost every finding's confidence when >=2 DISTINCT categories fired, and
    // boost harder when an InstructionOverride/RoleImpersonation co-occurs with
    // Exfiltration (the override+steal pattern). EncodingEvasion alone never
    // boosts -- a lone base64 blob is not an attack signal.
    uint32_t cats = out.flags_bitmask();
    int distinct = 0;
    for (int b = 0; b < 5; ++b) if (cats & (1u << b)) ++distinct;

    bool has_override = cats & category_bit(InjectionCategory::InstructionOverride);
    bool has_role     = cats & category_bit(InjectionCategory::RoleImpersonation);
    bool has_exfil    = cats & category_bit(InjectionCategory::Exfiltration);

    // Distinct categories excluding a lone EncodingEvasion (which we don't let
    // bootstrap a boost on its own).
    uint32_t non_enc = cats & ~category_bit(InjectionCategory::EncodingEvasion);
    int distinct_non_enc = 0;
    for (int b = 0; b < 5; ++b) if (non_enc & (1u << b)) ++distinct_non_enc;

    float boost = 1.0f;
    if (distinct_non_enc >= 2)              boost = 1.30f;
    if ((has_override || has_role) && has_exfil) boost = 1.55f;  // the dangerous combo
    if (distinct >= 3)                      boost = std::max(boost, 1.45f);

    if (boost > 1.0f) {
        for (auto& f : out.findings)
            f.confidence = std::min(1.0f, f.confidence * boost);
    }

    // Drop sub-threshold findings, then compute summary fields.
    out.findings.erase(
        std::remove_if(out.findings.begin(), out.findings.end(),
                       [&](const InjectionFinding& f) { return f.confidence < cfg.min_confidence; }),
        out.findings.end());

    for (const auto& f : out.findings) {
        if (f.confidence > out.max_confidence) {
            out.max_confidence = f.confidence;
            out.dominant = f.category;
        }
    }
    return out;
}

} // namespace locus::security
