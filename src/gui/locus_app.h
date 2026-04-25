#pragma once

#include "../core/locus_session.h"
#include "../llm/llm_client.h"

#include <wx/wx.h>

#include <memory>

namespace locus {

class LocusFrame;

// wxApp entry point. Owns one `LocusSession` (workspace + LLM + tools +
// agent, bundled — see src/core/locus_session.h) plus the main frame.
// Single-instance enforcement is per-workspace via the Workspace's
// internal `WorkspaceLock` (file lock inside `.locus/`), not a
// process-global wxSingleInstanceChecker — so two GUIs on different folders
// coexist, but two on the same (or nested) folder fail cleanly.
class LocusApp : public wxApp {
public:
    bool OnInit() override;
    int  OnExit() override;

    // Switch to a different workspace folder. Tears down the current
    // session/frame and reinitializes everything with the new path.
    void open_workspace(const std::string& path);

private:
    void init_logging(const std::filesystem::path& locus_dir);

    // Build a fresh session + frame for `ws_path`. seed_cfg is the LLM
    // override layer (CLI args at startup; empty on workspace switch).
    // On failure shows a wxMessageBox and leaves session_ null.
    bool spawn_session(const std::filesystem::path& ws_path,
                       const LLMConfig&             seed_cfg);

    // The whole core-subsystem bundle (owned).
    std::unique_ptr<LocusSession> session_;

    LocusFrame* frame_ = nullptr;  // owned by wxWidgets
};

wxDECLARE_APP(LocusApp);

} // namespace locus
