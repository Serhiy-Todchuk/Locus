#pragma once

#include "../workspace.h"
#include "../agent/agent_core.h"
#include "../llm_client.h"
#include "../tool_registry.h"

#include <wx/wx.h>

#include <memory>

namespace locus {

class LocusFrame;

// wxApp entry point. Owns the core subsystems (workspace, LLM, agent) and
// the main frame. Single-instance enforcement is per-workspace via the
// Workspace's internal `WorkspaceLock` (file lock inside `.locus/`), not a
// process-global wxSingleInstanceChecker — so two GUIs on different folders
// coexist, but two on the same (or nested) folder fail cleanly.
class LocusApp : public wxApp {
public:
    bool OnInit() override;
    int  OnExit() override;

    // Switch to a different workspace folder. Tears down the current
    // agent/frame and reinitializes everything with the new path.
    void open_workspace(const std::string& path);

private:
    void init_logging(const std::filesystem::path& locus_dir);

    // Core subsystems (owned, constructed in OnInit).
    std::unique_ptr<Workspace>    workspace_;
    std::unique_ptr<ILLMClient>   llm_;
    std::unique_ptr<ToolRegistry> tools_;
    std::unique_ptr<AgentCore>    agent_;

    LocusFrame* frame_ = nullptr;  // owned by wxWidgets
};

wxDECLARE_APP(LocusApp);

} // namespace locus
