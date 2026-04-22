#pragma once

#include "core/workspace_services.h"

#include <filesystem>

namespace locus::test {

// Minimal IWorkspaceServices implementation for unit tests that don't need
// a real Workspace. Backing subsystems are all optional — any unset pointer
// just makes the corresponding accessor return nullptr, which the tools
// already handle.
class FakeWorkspaceServices : public IWorkspaceServices {
public:
    explicit FakeWorkspaceServices(std::filesystem::path root,
                                   IndexQuery* index = nullptr,
                                   EmbeddingWorker* embedder = nullptr)
        : root_(std::move(root)), index_(index), embedder_(embedder) {}

    const std::filesystem::path& root() const override { return root_; }
    IndexQuery*      index()     override              { return index_; }
    EmbeddingWorker* embedder()  override              { return embedder_; }
    // `workspace()` stays nullptr — the real Workspace is not available here.

private:
    std::filesystem::path root_;
    IndexQuery*           index_    = nullptr;
    EmbeddingWorker*      embedder_ = nullptr;
};

} // namespace locus::test
