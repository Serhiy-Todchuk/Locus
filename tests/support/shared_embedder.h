#pragma once

#include <memory>
#include <string>

namespace locus { class Embedder; }

namespace locus::test {

// Lazily-loaded process-wide embedder for the unit test suite.
//
// Workspace's embedder_provider hook (set by the Catch2 listener in
// shared_embedder.cpp) routes every `enable_semantic_search()` call through
// here, so the bge-small-en-v1.5 GGUF is mmap'd + initialised once per process
// instead of once per `Workspace` ctor (38× in the suite). The shared instance
// is destroyed at TestRunEnded — explicit lifetime, no leaked llama state.
//
// The first GUI/CLI run in the suite to actually require an embedder pays the
// load cost (~600 ms for the small model on a 5800H); every subsequent
// Workspace borrows it.
class SharedTestEmbedder {
public:
    // Returns the cached embedder, loading on first call. Throws
    // std::runtime_error if the model GGUF is not under <repo>/models/.
    static std::shared_ptr<Embedder> get();

    // Drop the cached embedder. Idempotent. Called by the Catch2 listener at
    // testRunEnded so llama_backend cleanup runs before process exit.
    static void shutdown();

    // Filename actually used to satisfy `get()` — exposed for log clarity in
    // tests that want to assert on the model identity.
    static const std::string& model_filename();
};

} // namespace locus::test
