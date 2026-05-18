#include "llm_context.h"

#include "index/index_query.h"
#include "core/workspace.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace locus {

namespace {

// Generate a timestamp-based session id like SessionManager. Lifted from
// AgentCore's local helper -- the checkpoint store lays out turn dirs as
// `<session_id>/<turn_id>/` and the readable timestamp shape makes manual
// inspection of `.locus/checkpoints/` painless.
std::string make_session_id()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d_%H%M%S");
    return os.str();
}

} // namespace

LLMContext::LLMContext(IWorkspaceServices&                      services,
                       const LLMConfig&                         llm_config,
                       SystemPromptAssembly                     prompt,
                       FrontendRegistry&                        frontends,
                       SessionManager&                          sessions,
                       const std::filesystem::path&             checkpoints_dir)
    : services_(services)
    , llm_config_(llm_config)
    , prompt_(std::move(prompt))
    , frontends_(frontends)
    , sessions_(sessions)
{
    budget_         = std::make_unique<ContextBudget>(llm_config_.context_limit, frontends_);
    change_tracker_ = std::make_unique<FileChangeTracker>();
    update_reserve();

    session_id_ = make_session_id();
    turn_id_    = 0;

    if (!checkpoints_dir.empty()) {
        checkpoints_ = std::make_unique<CheckpointStore>(checkpoints_dir);
        spdlog::info("LLMContext: checkpoints at {} (session '{}')",
                     checkpoints_dir.string(), session_id_);
    }

    // Seed the conversation with the system message. The broadcast notifies
    // any already-registered frontends -- usually none at this point (the
    // GUI frontend is wired later), so this is effectively informational.
    history_.add({MessageRole::system, prompt_.full_text()});
    if (!history_.messages().empty()) {
        const auto& sys = history_.messages().back();
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_history_message_added(sys.history_id, sys.role, /*deletable=*/false);
        });
    }

    // Take an initial file-change tracker snapshot so the first user turn
    // diffs against "the world as it was when we started", not an empty set.
    snapshot_change_tracker();
}

// -- Token math --------------------------------------------------------------

int LLMContext::current_tokens() const
{
    return budget_->current(history_.estimate_tokens());
}

int LLMContext::effective_limit() const
{
    return budget_->effective_limit();
}

bool LLMContext::would_breach_reserve(int incoming_estimate) const
{
    int limit   = llm_config_.context_limit;
    int reserve = budget_->reserve();
    if (limit <= 0 || reserve <= 0) return false;
    return (incoming_estimate + reserve > limit);
}

void LLMContext::update_reserve()
{
    auto* ws = services_.workspace();
    if (!ws) return;
    int limit   = llm_config_.context_limit;
    int cfg_val = ws->config().compaction_reserve_tokens;
    int reserve = 0;
    if (cfg_val >= 0) {
        reserve = cfg_val;
    } else if (limit > 0) {
        reserve = std::min(limit / 5, 4096);
    }
    budget_->set_reserve(reserve);
    if (reserve > 0)
        spdlog::info("LLMContext: response reserve set to {} tokens (limit {})",
                     reserve, limit);
}

// -- Session / turn identity -------------------------------------------------

void LLMContext::renew_session_id()
{
    if (checkpoints_)
        checkpoints_->drop_session(session_id_);
    session_id_ = make_session_id();
    turn_id_    = 0;
}

// -- Attached file context (S2.4 / S4.F) -------------------------------------

void LLMContext::set_attached_context(AttachedContext ctx)
{
    std::lock_guard lock(attached_mutex_);
    attached_context_ = std::move(ctx);
}

void LLMContext::clear_attached_context()
{
    std::lock_guard lock(attached_mutex_);
    attached_context_.reset();
}

std::optional<AttachedContext> LLMContext::attached_context() const
{
    std::lock_guard lock(attached_mutex_);
    return attached_context_;
}

std::string LLMContext::compose_attached_context_block() const
{
    std::optional<AttachedContext> ctx;
    {
        std::lock_guard lock(attached_mutex_);
        ctx = attached_context_;
    }
    if (!ctx) return {};

    std::ostringstream ss;
    ss << "[Attached file: " << ctx->file_path << "]\n";

    if (auto* idx = services_.index()) {
        try {
            auto entries = idx->get_file_outline(ctx->file_path);
            if (!entries.empty()) {
                ss << "Outline:\n";
                for (const auto& e : entries) {
                    ss << "- L" << e.line << " ";
                    if (e.type == OutlineEntry::Heading) {
                        ss << std::string(static_cast<size_t>(std::max(1, e.level)), '#')
                           << " " << e.text;
                    } else {
                        ss << "[" << (e.kind.empty() ? "symbol" : e.kind) << "] "
                           << e.text;
                        if (!e.signature.empty())
                            ss << " " << e.signature;
                    }
                    ss << "\n";
                }
            }
        } catch (const std::exception& ex) {
            spdlog::warn("Attached-context outline lookup failed for '{}': {}",
                         ctx->file_path, ex.what());
        }
    }
    if (!ctx->preview.empty())
        ss << "Preview: " << ctx->preview << "\n";
    ss << "\n";
    return ss.str();
}

// -- Mutations ---------------------------------------------------------------

void LLMContext::add_message(ChatMessage msg)
{
    history_.add(std::move(msg));
    // S5.G -- notify frontends so they can map the new history_id back to a
    // chat bubble (only deletable shapes get the hover-reveal X). The system
    // prompt + tool result messages are never user-deletable; assistant
    // messages with tool_calls are paired with their tool results in the
    // wire format -- deleting one without the other would invalidate the
    // conversation, so they get deletable=false too.
    if (history_.messages().empty()) return;
    const auto& added = history_.messages().back();
    // Deletable bubbles get the hover-reveal X. Assistant-with-tool_calls is
    // deletable too (the X triggers a pair-delete of the assistant + every
    // matching tool result, routed through delete_message below). Empty-content
    // assistants get no bubble in the UI so deletability there is moot.
    // Tool-result messages are also deletable: clicking their X walks back to
    // the parent assistant and pair-deletes the whole turn (deleting just the
    // tool side would orphan the assistant's tool_call -- delete_message does
    // the lookup). System messages are never deletable.
    bool deletable = (added.role == MessageRole::user) ||
                     (added.role == MessageRole::assistant && !added.content.empty()) ||
                     (added.role == MessageRole::tool);
    frontends_.broadcast([&](IFrontend& fe) {
        fe.on_history_message_added(added.history_id, added.role, deletable);
    });
}

void LLMContext::replace_system_prompt_in_history(std::string content)
{
    history_.replace_system_prompt(std::move(content));
}

bool LLMContext::delete_message(int history_id)
{
    // Pair-delete if the target is an assistant message with tool_calls --
    // deleting one side without the other leaves orphan tool_calls / tool
    // results that break the next LLM round. Clicking the X on a tool-result
    // bubble redirects to the same pair-delete by looking up the parent
    // assistant via tool_call_id.
    const auto& msgs = history_.messages();
    auto it = std::find_if(msgs.begin(), msgs.end(),
                           [history_id](const ChatMessage& m) {
                               return m.history_id == history_id;
                           });
    if (it == msgs.end()) return false;

    std::vector<int> deleted;
    if (it->role == MessageRole::assistant && !it->tool_calls.empty()) {
        deleted = history_.delete_tool_call_pair(history_id);
        if (deleted.empty()) return false;
    } else if (it->role == MessageRole::tool && !it->tool_call_id.empty()) {
        const std::string call_id = it->tool_call_id;
        int parent_history_id = 0;
        for (const auto& m : msgs) {
            if (m.role != MessageRole::assistant) continue;
            bool match = false;
            for (const auto& tc : m.tool_calls) {
                if (tc.id == call_id) { match = true; break; }
            }
            if (match) { parent_history_id = m.history_id; break; }
        }
        if (parent_history_id == 0) return false;
        deleted = history_.delete_tool_call_pair(parent_history_id);
        if (deleted.empty()) return false;
    } else {
        if (!history_.delete_by_id(history_id)) return false;
        deleted.push_back(history_id);
    }

    // Drop cached server token totals -- they reflect a count that includes
    // the just-deleted message(s). The next LLM round repopulates them.
    budget_->set_server_total(0);
    budget_->set_server_split(0, 0);
    for (int id : deleted) {
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_history_message_deleted(id);
        });
    }
    return true;
}

// -- Compaction --------------------------------------------------------------

void LLMContext::compact_drop_tool_results()
{
    history_.drop_tool_results();
    // Invalidate cached server total -- it reflects the pre-compaction count.
    budget_->set_server_total(0);
    budget_->set_server_split(0, 0);
}

void LLMContext::compact_drop_oldest_turns(int n)
{
    history_.drop_oldest_turns(n);
    budget_->set_server_total(0);
    budget_->set_server_split(0, 0);
}

// -- Reset -------------------------------------------------------------------

void LLMContext::reset_for_new_conversation()
{
    history_.clear();
    // S4.F: the system prompt is byte-stable across the session; re-seed
    // from the same assembly we already hold.
    history_.add({MessageRole::system, prompt_.full_text()});
    if (!history_.messages().empty()) {
        const auto& sys = history_.messages().back();
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_history_message_added(sys.history_id, sys.role, /*deletable=*/false);
        });
    }
    budget_->reset();

    // Drop checkpoint history with the conversation; start a fresh session
    // so the on-disk layout doesn't keep accumulating across resets.
    renew_session_id();

    // S4.T -- baseline a fresh snapshot so we don't replay diffs that
    // happened across the now-cleared session.
    if (change_tracker_) {
        change_tracker_->clear_agent_touched();
        snapshot_change_tracker();
    }
}

// -- Persistence -------------------------------------------------------------

std::string LLMContext::save_session(const nlohmann::json& extras)
{
    auto id = sessions_.save(history_, extras);
    spdlog::info("LLMContext: session saved as '{}'", id);
    return id;
}

void LLMContext::load_session(const std::string& id)
{
    history_ = sessions_.load(id);
    budget_->reset();
    spdlog::info("LLMContext: loaded session '{}' ({} messages)",
                 id, history_.size());
    // S5.G -- re-announce every loaded message so a frontend connected after
    // load builds its dom -> history map for hover-reveal delete. Order is
    // history order; the chat panel's own re-rendering of loaded messages
    // happens through a separate path (LocusFrame::on_agent_session_reset
    // + re-render), so the added events here are purely for the delete map.
    for (const auto& m : history_.messages()) {
        bool deletable = (m.role == MessageRole::user) ||
                         (m.role == MessageRole::assistant && !m.content.empty()) ||
                         (m.role == MessageRole::tool);
        frontends_.broadcast([&](IFrontend& fe) {
            fe.on_history_message_added(m.history_id, m.role, deletable);
        });
    }
}

// -- Helpers -----------------------------------------------------------------

void LLMContext::snapshot_change_tracker()
{
    if (!change_tracker_) return;
    if (auto* idx = services_.index()) {
        try {
            change_tracker_->snapshot(*idx);
        } catch (const std::exception& e) {
            spdlog::warn("FileChangeTracker: snapshot failed: {}", e.what());
        }
    }
}

std::optional<std::string> LLMContext::read_current_pre_mutation(
    const std::string& rel_path) const
{
    if (!checkpoints_ || session_id_.empty() || turn_id_ <= 0) return std::nullopt;

    auto turn = checkpoints_->read_turn(session_id_, turn_id_);
    if (!turn.has_value()) return std::nullopt;

    std::string needle = rel_path;
    for (auto& c : needle) if (c == '\\') c = '/';

    const CheckpointEntry* match = nullptr;
    for (const auto& e : turn->entries) {
        if (e.path == needle) { match = &e; break; }
    }
    if (!match) return std::nullopt;
    if (!match->existed) return std::nullopt;
    if (match->skipped)  return std::nullopt;

    std::filesystem::path body = checkpoints_->root()
                  / session_id_
                  / std::to_string(turn_id_)
                  / "files"
                  / std::filesystem::path(needle);
    std::error_code ec;
    if (!std::filesystem::exists(body, ec)) return std::nullopt;

    std::ifstream f(body, std::ios::binary);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace locus
