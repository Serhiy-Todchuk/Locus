#include "agent/build_error_detector.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace locus {

namespace {

// Lowercase a copy.
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Collapse numeric NOISE (line/column numbers, counts like "3 unresolved
// externals", hex addresses) to '#' so two otherwise-identical diagnostics
// match -- WITHOUT destroying error codes. An error code is a digit run glued
// to a preceding letter (C2102, LNK2019, E0144); those digits are kept. A
// digit run that stands alone (preceded by a non-letter) is the noise we want
// to collapse. Hex addresses (0x....) collapse to "0x#".
std::string strip_numbers(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        // hex address 0x....
        if (in[i] == '0' && i + 1 < in.size() &&
            (in[i+1] == 'x' || in[i+1] == 'X')) {
            size_t j = i + 2;
            while (j < in.size() && std::isxdigit((unsigned char)in[j])) ++j;
            if (j > i + 2) { out += "0x#"; i = j; continue; }
        }
        if (std::isdigit((unsigned char)in[i])) {
            // Glued to a letter immediately before -> part of an error code;
            // keep the digits verbatim.
            bool code_digits = !out.empty() &&
                               std::isalpha((unsigned char)out.back());
            if (code_digits) {
                while (i < in.size() && std::isdigit((unsigned char)in[i]))
                    out += in[i++];
            } else {
                out += '#';
                while (i < in.size() && std::isdigit((unsigned char)in[i])) ++i;
            }
            continue;
        }
        out += in[i++];
    }
    return out;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

// Extract the "error ..." portion of a diagnostic line, dropping the
// file/path/line prefix that precedes it. Returns empty if the line is not a
// diagnostic. Recognizes "error", "fatal error" (MSVC) and "error:" (gcc/clang).
std::string diagnostic_key(const std::string& raw_line)
{
    std::string line = trim(raw_line);
    if (line.empty()) return {};
    std::string low = lower(line);

    // Must look like an error diagnostic (not a warning, not a note, not the
    // build summary). We deliberately ignore warnings -- a warning that
    // repeats is not a "stuck" signal the way a hard compile error is.
    // Find the diagnostic verb.
    size_t pos = std::string::npos;
    // Prefer "fatal error" then "error". Use the lowercase copy to locate,
    // then slice the original to keep the message's original casing/symbols.
    for (const char* kw : { "fatal error", "error:" , "error " }) {
        size_t p = low.find(kw);
        if (p != std::string::npos) { pos = p; break; }
    }
    if (pos == std::string::npos) return {};

    // Exclude lines that are clearly not per-diagnostic (e.g. "0 error(s)",
    // "Build FAILED." with no code) -- those still carry signal but are noisy;
    // we keep them only if a message follows the verb.
    std::string key = line.substr(pos);
    key = strip_numbers(key);
    key = trim(key);
    // Drop a trailing project/file annotation MSVC appends in brackets:
    // "... [D:\proj\foo.vcxproj]".
    size_t br = key.find(" [");
    if (br != std::string::npos) key = trim(key.substr(0, br));
    return key;
}

} // namespace

std::string extract_error_signature(const std::string& tool_output)
{
    if (tool_output.empty()) return {};

    std::set<std::string> keys;  // sorted + de-duplicated
    std::istringstream ss(tool_output);
    std::string line;
    while (std::getline(ss, line)) {
        std::string key = diagnostic_key(line);
        if (!key.empty()) keys.insert(key);
    }
    if (keys.empty()) return {};

    std::string sig;
    for (const auto& k : keys) {
        if (!sig.empty()) sig += '\n';
        sig += k;
    }
    return sig;
}

} // namespace locus
