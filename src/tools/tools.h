#pragma once

// Aggregator header for all built-in tools. Re-exports the concrete ITool
// subclasses + the registration factory. Individual subsystems can include
// a narrower sub-header (e.g. "tools/file_tools.h") if they only need one
// tool, but most callers just want everything.

#include "tool.h"
#include "tools/file_tools.h"
#include "tools/search_tools.h"
#include "tools/index_tools.h"
#include "tools/process_tools.h"
#include "tools/interactive_tools.h"

namespace locus {

void register_builtin_tools(IToolRegistry& registry);

} // namespace locus
