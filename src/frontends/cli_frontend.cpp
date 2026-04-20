#include "cli_frontend.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <string>

namespace locus {

CliFrontend::CliFrontend(ILocusCore& core)
    : core_(core)
{
}

void CliFrontend::on_token(std::string_view token)
{
    std::cout << token << std::flush;
}

void CliFrontend::on_tool_call_pending(const ToolCall& call,
                                       const std::string& preview)
{
    // Special handling for ask_user: show the question and prompt for a response.
    if (call.tool_name == "ask_user") {
        std::string question = call.args.value("question", "");
        std::cout << "\n\n--- Agent asks: " << question << " ---\n";
        std::cout << "> " << std::flush;

        std::string response;
        if (std::getline(std::cin, response) && !response.empty()) {
            nlohmann::json modified = call.args;
            modified["response"] = response;
            core_.tool_decision(call.id, ToolDecision::modify, modified);
        } else {
            core_.tool_decision(call.id, ToolDecision::approve);
        }
        return;
    }

    std::cout << "\n\n--- Tool call: " << call.tool_name << " ---\n";

    // Print args in a readable format.
    if (!call.args.empty() && !call.args.is_null()) {
        std::cout << "Args: " << call.args.dump(2) << "\n";
    }

    if (!preview.empty()) {
        std::cout << "Preview: " << preview << "\n";
    }

    char decision = read_decision();

    switch (decision) {
    case 'y':
        core_.tool_decision(call.id, ToolDecision::approve);
        break;
    case 'e': {
        auto modified = read_modified_args(call.args);
        core_.tool_decision(call.id, ToolDecision::modify, modified);
        break;
    }
    case 'n':
    default:
        core_.tool_decision(call.id, ToolDecision::reject);
        break;
    }
}

void CliFrontend::on_tool_result(const std::string& /*call_id*/,
                                 const std::string& display)
{
    if (!display.empty()) {
        // Truncate very long results for terminal display.
        constexpr size_t k_max_display = 2000;
        if (display.size() > k_max_display) {
            std::cout << display.substr(0, k_max_display)
                      << "\n... (" << display.size() - k_max_display
                      << " chars truncated)\n";
        } else {
            std::cout << display << "\n";
        }
    }
}

void CliFrontend::on_turn_start()
{
    // Nothing to do in CLI — the prompt already indicates we're waiting.
}

void CliFrontend::on_turn_complete()
{
    std::cout << "\n---\n";
}

void CliFrontend::on_session_reset()
{
    // Handled by main.cpp which prints its own message.
}

void CliFrontend::on_context_meter(int used_tokens, int limit)
{
    last_used_ = used_tokens;
    last_limit_ = limit;
}

void CliFrontend::on_compaction_needed(int used_tokens, int limit)
{
    int pct = (limit > 0) ? (used_tokens * 100 / limit) : 0;
    std::cout << "\n[context: " << used_tokens << "/" << limit
              << " tokens (" << pct << "%)]\n";

    if (pct >= 100) {
        std::cout << "Context is full. Choose a compaction strategy:\n"
                  << "  [b] Drop tool results (keep conversation, strip tool output)\n"
                  << "  [c] Drop oldest turns (remove N oldest exchanges)\n"
                  << "  [s] Skip (try to continue anyway)\n"
                  << "Compact [b/c/s]: " << std::flush;

        std::string line;
        if (std::getline(std::cin, line) && !line.empty()) {
            char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
            if (ch == 'b') {
                core_.compact_context(CompactionStrategy::drop_tool_results);
                std::cout << "Compacted: tool results dropped.\n";
            } else if (ch == 'c') {
                std::cout << "How many turns to drop? [3]: " << std::flush;
                std::string n_str;
                int n = 3;
                if (std::getline(std::cin, n_str) && !n_str.empty()) {
                    try { n = std::stoi(n_str); } catch (...) { n = 3; }
                }
                core_.compact_context(CompactionStrategy::drop_oldest, n);
                std::cout << "Compacted: dropped " << n << " oldest turn(s).\n";
            }
        }
    }
}

void CliFrontend::on_error(const std::string& message)
{
    std::cerr << "[error] " << message << "\n";
}

// -- Private helpers ----------------------------------------------------------

char CliFrontend::read_decision()
{
    std::cout << "[y]es / [n]o / [e]dit: " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line) || line.empty())
        return 'y';  // default: approve

    char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
    if (ch == 'y' || ch == 'n' || ch == 'e')
        return ch;
    return 'y';
}

nlohmann::json CliFrontend::read_modified_args(const nlohmann::json& original)
{
    std::cout << "Enter modified args as JSON (single line):\n"
              << "Current: " << original.dump() << "\n"
              << "> " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line) || line.empty())
        return original;

    try {
        return nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Invalid JSON, using original args: " << e.what() << "\n";
        return original;
    }
}

void CliFrontend::on_embedding_progress(int done, int total)
{
    spdlog::trace("Embedding progress: {}/{}", done, total);
}

void CliFrontend::on_activity(const ActivityEvent& event)
{
    spdlog::trace("activity[{}] {}: {}", event.id, to_string(event.kind), event.summary);
}

} // namespace locus
