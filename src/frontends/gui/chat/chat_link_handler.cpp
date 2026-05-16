#include "chat_link_handler.h"

#include "chat_util.h"

#include <spdlog/spdlog.h>

#include <charconv>

namespace locus {

ChatLinkHandler::ChatLinkHandler(RunJsFn run_js, DecisionFn on_plan_decision,
                                  DeleteFn on_delete_message)
    : run_js_(std::move(run_js))
    , on_plan_decision_(std::move(on_plan_decision))
    , on_delete_message_(std::move(on_delete_message))
{}

void ChatLinkHandler::register_plan(const std::string& plan_id, int msg_id)
{
    if (!plan_id.empty())
        plan_msg_ids_[plan_id] = msg_id;
}

int ChatLinkHandler::lookup_msg_id(const std::string& plan_id) const
{
    auto it = plan_msg_ids_.find(plan_id);
    return (it != plan_msg_ids_.end()) ? it->second : -1;
}

bool ChatLinkHandler::handle_url(const wxString& url,
                                  const std::string& current_plan_id)
{
    if (url.StartsWith("locus://plan-approve")) {
        if (!current_plan_id.empty()) {
            auto it = plan_msg_ids_.find(current_plan_id);
            if (it != plan_msg_ids_.end()) {
                run_js_(wxString::Format(
                    "setPlanDecided(%d, '%s');",
                    it->second, chat_js_escape("Approved -- executing...")));
            }
        }
        if (on_plan_decision_) on_plan_decision_("approve");
        return true;
    }

    if (url.StartsWith("locus://plan-reject")) {
        if (!current_plan_id.empty()) {
            auto it = plan_msg_ids_.find(current_plan_id);
            if (it != plan_msg_ids_.end()) {
                run_js_(wxString::Format(
                    "setPlanDecided(%d, '%s');",
                    it->second, chat_js_escape("Rejected.")));
            }
        }
        if (on_plan_decision_) on_plan_decision_("reject");
        return true;
    }

    // S5.G -- per-message delete. URL shape: locus://delete-message/<history_id>
    if (url.StartsWith("locus://delete-message/")) {
        wxString tail = url.Mid(wxString("locus://delete-message/").length());
        // Trim a trailing '/' that WebView2 sometimes appends.
        if (!tail.empty() && tail.Last() == '/')
            tail = tail.RemoveLast();
        std::string tail_s = tail.ToStdString();
        int history_id = 0;
        auto [ptr, ec] = std::from_chars(tail_s.data(),
                                          tail_s.data() + tail_s.size(),
                                          history_id);
        if (ec != std::errc() || history_id <= 0) {
            spdlog::warn("ChatLinkHandler: bad delete-message id '{}'", tail_s);
            return true;  // still consume the URL
        }
        if (on_delete_message_) on_delete_message_(history_id);
        return true;
    }

    return false;
}

void ChatLinkHandler::clear_plans()
{
    plan_msg_ids_.clear();
}

} // namespace locus
