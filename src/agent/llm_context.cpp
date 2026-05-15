#include "llm_context.h"

#include "index/index_query.h"

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

    session_id_ = make_session_id();
    turn_id_    = 0;

    if (!checkpoints_dir.empty()) {
        checkpoints_ = std::make_unique<CheckpointStore>(checkpoints_dir);
        spdlog::info("LLMContext: checkpoints at {} (session '{}')",
                     checkpoints_dir.string(), session_id_);
    }

    // Seed the conversation with the system message.
    history_.add({MessageRole::system, prompt_.full_text()});

    // Take an initial file-change tracker snapshot so the first user turn
    // diffs against "the world as it was when we started", not an empty set.
    snapshot_change_tracker();
}

// -- Token math --------------------------------------------------------------

int LLMContext::current_tokens() const
{
    return budget_->current(history_.estimate_tokens());
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
}

void LLMContext::replace_system_prompt_in_history(std::string content)
{
    history_.replace_system_prompt(std::move(content));
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
