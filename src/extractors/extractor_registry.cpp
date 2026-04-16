#include "extractor_registry.h"

namespace locus {

void ExtractorRegistry::register_extractor(std::string ext,
                                           std::unique_ptr<ITextExtractor> extractor)
{
    extractors_[std::move(ext)] = std::move(extractor);
}

ITextExtractor* ExtractorRegistry::find(const std::string& ext) const
{
    auto it = extractors_.find(ext);
    return it != extractors_.end() ? it->second.get() : nullptr;
}

} // namespace locus
