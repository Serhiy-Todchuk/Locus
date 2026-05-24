// Per-area spdlog loggers so noisy categories (SQL exec/prepare, file watcher
// events, list_directory churn) can be muted independently of the default
// 'locus' logger. Both child loggers share the default's sinks, so a single
// init_logging call is still the only writer; only the levels diverge.
//
// Used by agentic-mode launches to keep the trace stream focused on
// LLM / tool / agent activity instead of drowning in SQL and watcher noise.
#pragma once

#include <spdlog/logger.h>
#include <memory>

namespace locus {

// Returns the 'db' logger -- used by Database / IndexQuery / Indexer SQL paths.
// Lazy-initialised on first call; clones the default logger's sinks so output
// goes to the same file + (optional) stderr destinations.
std::shared_ptr<spdlog::logger> log_db();

// Returns the 'fs' logger -- used by FileWatcher, WatcherPump, and the
// list_directory hot path that fires once per visible tree refresh.
std::shared_ptr<spdlog::logger> log_fs();

// Demote both noise loggers to info level so trace-level SQL / file-watcher
// chatter stops while the rest of the 'locus' logger keeps trace output.
// Idempotent.
void mute_noise_loggers();

} // namespace locus
