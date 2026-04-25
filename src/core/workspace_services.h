#pragma once

#include <filesystem>

namespace locus {

class EmbeddingWorker;
class IndexQuery;
class ProcessRegistry;
class Reranker;
class Workspace;

// Services surface that tools and the agent loop see.
// Replaces the former `WorkspaceContext` raw-pointer struct.
//
// Only `root()` and `index()` are pure-virtual. Optional subsystems default
// to `nullptr` so adding a new one (LSP, checkpoints, process registry, ...)
// doesn't require every existing implementation to change.
class IWorkspaceServices {
public:
    virtual ~IWorkspaceServices() = default;

    virtual const std::filesystem::path& root() const = 0;
    virtual IndexQuery*                  index()      = 0;

    // Optional subsystems.
    virtual EmbeddingWorker* embedder()  { return nullptr; }
    virtual Reranker*        reranker()  { return nullptr; }
    virtual ProcessRegistry* processes() { return nullptr; }
    virtual Workspace*       workspace() { return nullptr; }
};

} // namespace locus
