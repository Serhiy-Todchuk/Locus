#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include <wx/string.h>
#include <wx/webview.h>

namespace locus {

// Handles locus:// URL dispatch from the chat WebView and tracks the
// plan-id -> message-id mapping needed to lock plan bubbles on approve/reject.
//
// ChatPanel creates one and calls handle_url() from on_webview_navigating.
// The current_plan_id passed to handle_url comes from ChatFooterChips (the
// footer chip tracks which plan it's currently showing progress for).
class ChatLinkHandler {
public:
    using DecisionFn = std::function<void(const std::string&)>;
    using RunJsFn    = std::function<void(const wxString&)>;
    // S5.G -- per-message delete confirm-and-dispatch. The handler calls this
    // with the history_id from the locus://delete-message/<id> URL after the
    // user clicks the hover-reveal X. The frame implementation is responsible
    // for the confirm dialog + AgentCore::delete_message wiring.
    using DeleteFn   = std::function<void(int /*history_id*/)>;

    ChatLinkHandler(RunJsFn run_js, DecisionFn on_plan_decision,
                    DeleteFn on_delete_message = nullptr);

    void set_on_delete_message(DeleteFn fn) { on_delete_message_ = std::move(fn); }

    // Register a new plan bubble. Called from ChatPanel::on_plan_proposed.
    void register_plan(const std::string& plan_id, int msg_id);

    // Look up a plan bubble's message id. Returns -1 when not found.
    // Called from ChatPanel::on_plan_step_advanced and on_plan_completed.
    int lookup_msg_id(const std::string& plan_id) const;

    // Process a locus:// URL from on_webview_navigating.
    // Returns true when the URL was handled (caller must call evt.Veto()).
    // current_plan_id is the active plan tracked by ChatFooterChips.
    bool handle_url(const wxString& url, const std::string& current_plan_id);

    // Clear all plan state on session reset.
    void clear_plans();

private:
    RunJsFn    run_js_;
    DecisionFn on_plan_decision_;
    DeleteFn   on_delete_message_;

    std::unordered_map<std::string, int> plan_msg_ids_;
};

} // namespace locus
