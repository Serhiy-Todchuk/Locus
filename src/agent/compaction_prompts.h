#pragma once

#include <string>

namespace locus {

// S5.F Layer 6 (LLM summary) prompt template. Kept small (~400 tokens) so
// the summary call itself doesn't blow what little budget remains.
//
// Substitution slots:
//   {conversation}         -- the dropped span formatted as role: content lines
//   {custom_instructions}  -- workspace config / per-run /compact arg; "" if absent
//
// File-ops accumulation across compactions (a separate cumulative tracker) is
// out of scope for S5.F; the prompt nods at it via the Critical Context bullet.
inline constexpr const char* k_compaction_summary_template = R"PROMPT(You are compacting an earlier portion of a coding-assistant conversation so the agent can continue without losing the load-bearing context. Produce a structured summary; do not invent details that are not in the excerpt.

The summary will replace the original messages in the conversation, so include everything the assistant needs to continue the task coherently. Bias toward concrete facts (file paths, function names, command lines, error messages) over prose.

Output sections (use these exact headers, in this order):

## Goal
One or two sentences: what the user was trying to accomplish in the dropped span.

## Constraints
Bullet list. Any rules, preferences, or limitations the user or the assistant established. Empty list is OK.

## Progress
Bullet list. What has been done so far -- files touched, decisions taken, code written. Mention concrete file paths.

## Key Decisions
Bullet list. Architectural or implementation choices that were made and the reasoning behind them. Empty list is OK.

## Next Steps
Bullet list. What the assistant was about to do or should do next.

## Critical Context
Bullet list. Anything that doesn't fit above but the assistant must remember to keep working effectively (error messages, command outputs, gotchas, half-finished thoughts).

{custom_instructions}

--- BEGIN DROPPED SPAN ---
{conversation}
--- END DROPPED SPAN ---

Now produce the structured summary. Use the exact section headers above. Be concise but complete; an experienced assistant should be able to pick up the work from your summary alone.
)PROMPT";

} // namespace locus
