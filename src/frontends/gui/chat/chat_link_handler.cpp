#include "chat_link_handler.h"

#include <spdlog/spdlog.h>

namespace locus {

ChatLinkHandler::ChatLinkHandler(RunJsFn run_js, DecisionFn on_plan_decision)
    : run_js_(std::move(run_js))
    , on_plan_decision_(std::move(on_plan_decision))
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
                    it->second, js_escape("Approved -- executing...")));
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
                    it->second, js_escape("Rejected.")));
            }
        }
        if (on_plan_decision_) on_plan_decision_("reject");
        return true;
    }

    return false;
}

void ChatLinkHandler::clear_plans()
{
    plan_msg_ids_.clear();
}

wxString ChatLinkHandler::js_escape(const wxString& s)
{
    wxString out;
    out.reserve(s.length() + 16);
    for (auto ch : s) {
        switch (ch.GetValue()) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '<':  out += "\\x3C"; break;
        default:   out += ch;      break;
        }
    }
    return out;
}

} // namespace locus
