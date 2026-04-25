#include "metrics.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace locus {

// -- Recording ---------------------------------------------------------------

void MetricsAggregator::begin_turn(int turn_id)
{
    std::lock_guard lock(mutex_);
    TurnSample s;
    s.turn_id = turn_id;
    s.started = std::chrono::system_clock::now();
    turns_.push_back(s);
    last_turn_id_ = turn_id;
}

void MetricsAggregator::end_turn(bool had_error)
{
    std::lock_guard lock(mutex_);
    if (turns_.empty()) return;
    auto& s = turns_.back();
    s.ended = std::chrono::system_clock::now();
    s.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        s.ended - s.started).count();
    s.had_error = had_error;
}

void MetricsAggregator::record_llm_step(const CompletionUsage& usage,
                                        long long stream_ms,
                                        int prev_turn_total)
{
    std::lock_guard lock(mutex_);
    if (turns_.empty()) return;
    auto& s = turns_.back();
    ++s.llm_rounds;
    s.stream_ms        += stream_ms;
    s.prompt_tokens     = usage.prompt_tokens;       // last round wins
    s.completion_tokens += usage.completion_tokens;  // sum
    s.reasoning_tokens  += usage.reasoning_tokens;
    if (usage.total_tokens > 0) {
        s.total_tokens = usage.total_tokens;
        s.tokens_delta = usage.total_tokens - prev_turn_total;
    }
}

void MetricsAggregator::record_tool(const std::string& name,
                                    bool ok,
                                    long long ms,
                                    size_t result_chars,
                                    std::optional<int> retrieval_count,
                                    std::optional<int> retrieval_cap)
{
    std::lock_guard lock(mutex_);

    auto& ts = tools_[name];
    ++ts.calls;
    if (!ok) ++ts.failures;
    ts.total_ms    += ms;
    ts.total_chars += static_cast<long long>(result_chars);

    if (!turns_.empty()) {
        ++turns_.back().tool_calls;
    }

    if (retrieval_count.has_value()) {
        ++retrieval_.queries;
        if (*retrieval_count > 0) ++retrieval_.hits;
        else                      ++retrieval_.empty;
        retrieval_.results_total += *retrieval_count;
        if (retrieval_cap.has_value() && *retrieval_cap > 0)
            retrieval_.max_results_cap_total += *retrieval_cap;
    }
}

// -- Reading -----------------------------------------------------------------

void MetricsAggregator::reset()
{
    std::lock_guard lock(mutex_);
    turns_.clear();
    tools_.clear();
    retrieval_ = {};
    last_turn_id_ = 0;
}

std::vector<TurnSample> MetricsAggregator::turns_snapshot() const
{
    std::lock_guard lock(mutex_);
    return turns_;
}

std::map<std::string, ToolStat> MetricsAggregator::tools_snapshot() const
{
    std::lock_guard lock(mutex_);
    return tools_;
}

RetrievalStat MetricsAggregator::retrieval_snapshot() const
{
    std::lock_guard lock(mutex_);
    return retrieval_;
}

MetricsAggregator::Aggregates MetricsAggregator::aggregates() const
{
    std::lock_guard lock(mutex_);

    Aggregates a;
    a.turn_count = static_cast<int>(turns_.size());

    long long completion_total = 0;
    for (const auto& s : turns_) {
        a.tokens_in_total  += std::max(0, s.tokens_delta);
        a.tokens_out_total += s.completion_tokens;
        a.reasoning_total  += s.reasoning_tokens;
        a.stream_ms_total  += s.stream_ms;
        completion_total   += s.completion_tokens;
    }

    if (a.stream_ms_total > 0)
        a.tokens_per_second = (1000.0 * static_cast<double>(completion_total))
                              / static_cast<double>(a.stream_ms_total);

    // Turn-time stats — only over turns that have ended.
    std::vector<long long> durations;
    durations.reserve(turns_.size());
    for (const auto& s : turns_) {
        if (s.duration_ms > 0) durations.push_back(s.duration_ms);
    }
    if (!durations.empty()) {
        long long sum = 0;
        for (auto d : durations) sum += d;
        a.avg_turn_ms = sum / static_cast<long long>(durations.size());

        std::vector<long long> sorted = durations;
        std::sort(sorted.begin(), sorted.end());
        a.max_turn_ms = sorted.back();
        // p95: linear-interpolation-free lower-bound rank — fine for small N.
        size_t idx = static_cast<size_t>(0.95 * (sorted.size() - 1) + 0.5);
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        a.p95_turn_ms = sorted[idx];
    }

    // Retrieval.
    a.retrieval_queries = retrieval_.queries;
    if (retrieval_.queries > 0)
        a.retrieval_hit_rate = static_cast<double>(retrieval_.hits)
                                / static_cast<double>(retrieval_.queries);

    // Tool histogram (descending by call count).
    a.tool_calls_by_name.reserve(tools_.size());
    for (const auto& [name, st] : tools_) {
        a.tool_calls_by_name.emplace_back(name, st.calls);
    }
    std::sort(a.tool_calls_by_name.begin(), a.tool_calls_by_name.end(),
              [](const auto& l, const auto& r) { return l.second > r.second; });

    // Spark-bar series.
    a.turn_durations_ms.reserve(turns_.size());
    a.turn_total_tokens.reserve(turns_.size());
    for (const auto& s : turns_) {
        a.turn_durations_ms.push_back(s.duration_ms);
        a.turn_total_tokens.push_back(s.total_tokens);
    }

    return a;
}

// -- Serialisation -----------------------------------------------------------

static long long iso_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch()).count();
}

nlohmann::json MetricsAggregator::to_json() const
{
    std::lock_guard lock(mutex_);

    nlohmann::json j;

    nlohmann::json turns = nlohmann::json::array();
    long long completion_total = 0;
    long long stream_total = 0;
    int prompt_total = 0, reasoning_total = 0, delta_total = 0;
    for (const auto& s : turns_) {
        nlohmann::json t;
        t["turn_id"]           = s.turn_id;
        t["started_ms"]        = iso_ms(s.started);
        if (s.ended.time_since_epoch().count() > 0)
            t["ended_ms"]      = iso_ms(s.ended);
        t["duration_ms"]       = s.duration_ms;
        t["stream_ms"]         = s.stream_ms;
        t["prompt_tokens"]     = s.prompt_tokens;
        t["completion_tokens"] = s.completion_tokens;
        t["reasoning_tokens"]  = s.reasoning_tokens;
        t["total_tokens"]      = s.total_tokens;
        t["tokens_delta"]      = s.tokens_delta;
        t["llm_rounds"]        = s.llm_rounds;
        t["tool_calls"]        = s.tool_calls;
        t["had_error"]         = s.had_error;
        turns.push_back(std::move(t));

        completion_total += s.completion_tokens;
        stream_total     += s.stream_ms;
        prompt_total     += s.prompt_tokens;
        reasoning_total  += s.reasoning_tokens;
        if (s.tokens_delta > 0) delta_total += s.tokens_delta;
    }
    j["turns"] = std::move(turns);

    nlohmann::json tools = nlohmann::json::object();
    for (const auto& [name, st] : tools_) {
        nlohmann::json t;
        t["calls"]       = st.calls;
        t["failures"]    = st.failures;
        t["total_ms"]    = st.total_ms;
        t["total_chars"] = st.total_chars;
        tools[name] = std::move(t);
    }
    j["tools"] = std::move(tools);

    nlohmann::json r;
    r["queries"]               = retrieval_.queries;
    r["hits"]                  = retrieval_.hits;
    r["empty"]                 = retrieval_.empty;
    r["results_total"]         = retrieval_.results_total;
    r["max_results_cap_total"] = retrieval_.max_results_cap_total;
    if (retrieval_.queries > 0)
        r["hit_rate"] = static_cast<double>(retrieval_.hits)
                          / static_cast<double>(retrieval_.queries);
    j["retrieval"] = std::move(r);

    nlohmann::json totals;
    totals["turn_count"]        = static_cast<int>(turns_.size());
    totals["tokens_in_total"]   = delta_total;
    totals["tokens_out_total"]  = completion_total;
    totals["reasoning_total"]   = reasoning_total;
    totals["stream_ms_total"]   = stream_total;
    if (stream_total > 0)
        totals["tokens_per_second"] =
            (1000.0 * static_cast<double>(completion_total))
            / static_cast<double>(stream_total);
    j["totals"] = std::move(totals);

    return j;
}

namespace {

// Strip embedded commas/newlines from a CSV cell — the rendered values here
// are tool names, error-free numbers, and short strings, so quoting suffices.
std::string csv_cell(const std::string& s)
{
    bool need = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!need) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else          out += c;
    }
    out += '"';
    return out;
}

} // namespace

std::string MetricsAggregator::to_csv() const
{
    std::lock_guard lock(mutex_);
    std::ostringstream o;

    o << "# turns\n";
    o << "turn_id,duration_ms,stream_ms,prompt_tokens,completion_tokens,"
         "reasoning_tokens,total_tokens,tokens_delta,llm_rounds,tool_calls,"
         "had_error\n";
    for (const auto& s : turns_) {
        o << s.turn_id           << ','
          << s.duration_ms       << ','
          << s.stream_ms         << ','
          << s.prompt_tokens     << ','
          << s.completion_tokens << ','
          << s.reasoning_tokens  << ','
          << s.total_tokens      << ','
          << s.tokens_delta      << ','
          << s.llm_rounds        << ','
          << s.tool_calls        << ','
          << (s.had_error ? 1 : 0) << '\n';
    }
    o << '\n';

    o << "# tools\n";
    o << "name,calls,failures,total_ms,total_chars\n";
    for (const auto& [name, st] : tools_) {
        o << csv_cell(name) << ','
          << st.calls       << ','
          << st.failures    << ','
          << st.total_ms    << ','
          << st.total_chars << '\n';
    }
    o << '\n';

    o << "# retrieval\n";
    o << "queries,hits,empty,results_total,max_results_cap_total,hit_rate\n";
    double hr = retrieval_.queries > 0
                  ? static_cast<double>(retrieval_.hits)
                      / static_cast<double>(retrieval_.queries)
                  : 0.0;
    o << retrieval_.queries               << ','
      << retrieval_.hits                  << ','
      << retrieval_.empty                 << ','
      << retrieval_.results_total         << ','
      << retrieval_.max_results_cap_total << ','
      << hr                                << '\n';

    return o.str();
}

// -- Search-result parser ----------------------------------------------------
//
// The headers we want to extract from look like:
//   "5 results for \"foo\""
//   "12 matches for /bar/"
//   "3 symbols matching \"baz\""
//   "8 semantic matches:"
//   "10 hybrid matches (BM25 + semantic):"
// Plus the "No semantic matches found." / "No matches found." strings. We
// only need the leading integer (or 0 for the "No ..." case).

std::optional<int> parse_search_result_count(const std::string& tool_name,
                                             const std::string& content)
{
    // Tool name gating: we want to track only retrieval calls. Both the
    // unified `search` face and the per-mode tools share the same headers.
    static const std::vector<std::string> retrieval_tools = {
        "search",
        "search_text", "search_regex", "search_symbols",
        "search_semantic", "search_hybrid",
    };
    bool match = false;
    for (const auto& n : retrieval_tools) if (n == tool_name) { match = true; break; }
    if (!match) return std::nullopt;

    // Empty result?
    auto starts_with = [](const std::string& s, const char* pfx) {
        size_t n = std::char_traits<char>::length(pfx);
        return s.size() >= n && std::char_traits<char>::compare(s.data(), pfx, n) == 0;
    };
    if (starts_with(content, "No matches found")
        || starts_with(content, "No semantic matches found"))
        return 0;

    // Skip a leading "Note: ... \n\n" preamble (semantic search prepends one
    // when the embedding queue is mid-flight).
    std::string body = content;
    if (starts_with(body, "Note:")) {
        auto p = body.find("\n\n");
        if (p != std::string::npos) body = body.substr(p + 2);
    }

    // Read the leading integer.
    size_t i = 0;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size() || !std::isdigit(static_cast<unsigned char>(body[i])))
        return std::nullopt;

    long long n = 0;
    while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) {
        n = n * 10 + (body[i] - '0');
        ++i;
    }
    if (n > 1'000'000) return std::nullopt;  // sanity
    return static_cast<int>(n);
}

} // namespace locus
