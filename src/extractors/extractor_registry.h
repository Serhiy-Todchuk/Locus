#pragma once

#include "text_extractor.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace locus {

// Maps file extensions (e.g. ".md", ".pdf") to ITextExtractor instances.
// Owned by Workspace, passed to Indexer by reference.
class ExtractorRegistry {
public:
    // ext should include the dot: ".md", ".html", ".pdf"
    void register_extractor(std::string ext, std::unique_ptr<ITextExtractor> extractor);

    // Returns nullptr if no extractor registered for this extension.
    ITextExtractor* find(const std::string& ext) const;

private:
    std::unordered_map<std::string, std::unique_ptr<ITextExtractor>> extractors_;
};

} // namespace locus
