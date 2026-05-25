#pragma once

#include <string>

namespace locus {

// build_chat_html -- assembles the chat WebView's static document (HTML +
// inlined CSS + JS). Pulled out of chat_panel.cpp so the 800-line template
// literal doesn't dominate the panel translation unit. Called once per
// ChatPanel construction (LoadURL/SetPage); the document never reloads after
// that, so this isn't a hot path.
//
// Inlines the local Prism.js bundle from <exe_dir>/resources/prism/ rather
// than reaching for a CDN. Missing assets log a warning and the function
// still returns valid HTML (just without syntax highlighting).
std::string build_chat_html();

} // namespace locus
