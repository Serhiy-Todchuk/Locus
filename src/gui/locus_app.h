#pragma once

#include "../workspace.h"
#include "../agent_core.h"
#include "../llm_client.h"
#include "../tool_registry.h"

#include <wx/wx.h>
#include <wx/snglinst.h>

#include <memory>

namespace locus {

class LocusFrame;

// wxApp entry point. Owns the core subsystems (workspace, LLM, agent) and the
// main frame. Enforces single-instance via wxSingleInstanceChecker.
class LocusApp : public wxApp {
public:
    bool OnInit() override;
    int  OnExit() override;

private:
    void init_logging(const std::filesystem::path& locus_dir);

    std::unique_ptr<wxSingleInstanceChecker> instance_checker_;

    // Core subsystems (owned, constructed in OnInit).
    std::unique_ptr<Workspace>    workspace_;
    std::unique_ptr<ILLMClient>   llm_;
    std::unique_ptr<ToolRegistry> tools_;
    std::unique_ptr<AgentCore>    agent_;

    LocusFrame* frame_ = nullptr;  // owned by wxWidgets
};

} // namespace locus
