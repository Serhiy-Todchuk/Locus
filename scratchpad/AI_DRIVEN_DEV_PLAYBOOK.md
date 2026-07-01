# AI-Assisted Development Playbook

Rules for a C++ codebase where some features are written by AI agents and others by hand.
The goal: keep the codebase lean, scalable, understandable, reliable, and maintainable
after many AI-driven changes - so an experienced C++/graphics developer can read the code
and architecture (algorithms, class structure, how classes interoperate) and edit any of it
later without archaeology.

Three parts: **design principles** (how code should be shaped), **working principles**
(how the agent should behave during a task), and **the shared setup** (how the team - human
and agent - stays in sync).

---

## PART 1a - DESIGN PRINCIPLES (code shape)

### D1. Simplicity first
Write the minimum code that solves the problem. No features beyond what was asked, no
"flexibility" or configurability nobody requested, no error handling for impossible cases.
If 200 lines could be 50, write the 50. The test: would a senior engineer call this
overcomplicated?

This principle governs the ones below - they are its concrete clauses:

- **OOP/OOD principles serve simplicity, not the other way around.** Where an OOP/OOD
  principle (single responsibility, favor abstraction, don't repeat yourself) and simplicity
  or performance disagree, simplicity and performance win. Such principles break ties between
  otherwise-equal designs; they are never a reason to add structure. And OOP isn't the only
  lens: where a data-driven design fits better - laying data out for cache-friendly batch
  processing, driving behavior from data tables rather than class hierarchies, as engines
  routinely do for entities, materials, and render passes - prefer it.
- **Interfaces are a deliberate cost, especially in C++.** A virtual abstract base is
  indirection, no inlining, and usually a heap allocation - you pay that only when you
  actually need runtime polymorphism over more than one implementation. Do not create an
  `IRenderer` for a single `Renderer`. Prefer concrete types, templates, or free functions;
  reach for a virtual interface when a real second backend (Vulkan + D3D12) exists or is
  imminent. Abstracting to a contract is the single most over-applied habit; treat every new
  interface as something you have to justify, not a default.
- **Delete, don't accumulate.** No dead code, no backwards-compat shims "just in case".
  Remove unused code rather than guarding it.

### D2. One clear ownership tree, unwound in reverse
Owned subsystems are unique-owner members (`unique_ptr`). Construction order = wiring order;
destruction is the strict reverse. Where one object borrows another (a `RenderPass` holding
a raw pointer into the `Renderer` that outlives it), say so at the declaration. Prefer single
ownership; when you must share, comment why. Diffuse ownership is where lifetime bugs breed.

### D3. RAII for every external resource
Wrap GPU buffers, textures, command pools, file handles, and locks in a type whose destructor
releases them. Delete copy on resource owners; add move only when needed. The leak and
double-free class of bug then can't compile.

### D4. Composition over inheritance
Build behavior by composing collaborators, not deep hierarchies. A `Material` *has* a shader,
a blend state, and a texture set; it doesn't inherit from six bases. Each collaborator stays
independently testable and replaceable.

### D5. One responsibility - at the class and the file level
A class does one thing; a file holds one subsystem. When either starts accumulating unrelated
responsibilities, split off a collaborator rather than adding more methods or lines. A
`Renderer` that also loads assets, owns the window, and caches shaders should compose a
`ShaderCache` + `AssetLoader` instead of doing all three. Watch the smell: a class whose name
needs "and" to describe it, or whose members cluster into groups that don't touch each other.
Keeping both small means a change touches the relevant part, not 3000 lines - which
matters most under AI edits, where "add one more method to what's here" is the default drift.

### D6. Pure functions for decisions; side effects at the edge
Factor hard logic into pure, testable functions with one source of truth; let the stateful
caller do only the side effect. `is_visible(bounds, frustum) -> bool` is pure and
exhaustively testable; the render loop just skips the draw. Same for `pick_lod(distance,
thresholds)`, `classify_collision(a, b)`. When behavior must change, there's one place to
change it, tested without mocks. Highest-leverage habit here.

### D7. Self-explanatory code over comments
Prefer clear names and obvious structure to comments. When you do comment, keep it short.
Good uses of a comment: flagging a deliberate deviation from convention or common sense
(a workaround, a constraint, a non-obvious perf trick), or explaining a complex algorithm
inside a function body. Do not write comments that restate what the code already says, and
do not add docstrings/block headers to obvious classes.

### D8. Reuse before writing
If an action looks common (ray/triangle intersection, linearizing depth, a hash, a string
split), search the codebase for an existing util before writing your own. If new code is
itself common, put it in the existing tools/utils files - or create one if none fits - rather
than inlining a private copy. Reuse is the inflow side of "delete, don't accumulate".

### D9. Fail closed, fail loud, never silently
Fatal errors throw (or, in no-exception projects, log an error and return a failure).
Recoverable issues degrade with a warning. A quietly wrong result - a shader that renders
garbage with no diagnostic - is worse than a hard failure.

---

## PART 1b - WORKING PRINCIPLES (agent behavior during a task)

### W1. Think before coding
If the prompt is ambiguous, ask - don't guess. State assumptions explicitly. If there are
several reasonable interpretations, surface them instead of silently picking one. If a
simpler approach exists than what was asked, say so. (This is about ambiguous *requirements*;
for an unambiguous task the obvious mechanical path needs no permission - see W2.)

### W2. Surgical changes
Touch only the files and lines the task requires. Don't "improve" adjacent code, comments, or
formatting; don't refactor what isn't broken; match the surrounding style even if you'd do it
differently. Remove imports/variables your change made unused - but leave pre-existing dead
code alone unless removing it is the task. This keeps diffs small and reviewable, and keeps
AI-authored changes from colliding with hand-written code.

### W3. Goal-driven execution
Turn "make it work" into a verifiable goal, then loop until it's met: define what success
looks like (builds, the test passes, the feature does X against a real workload), do the
change, check it, and iterate - rather than declaring done on a vague directive.

---

## PART 2 - THE SHARED SETUP

### The shared context file (`CLAUDE.md`)
One file at the repo root that the agent reads at the start of every session and the team
maintains together. A quick-scan operational reference, not a design doc:

- **What this is** - one sentence + the defining constraint (perf target, platform).
- **Conventions** - naming, file layout, error policy, the "never do this" list, build/test
  commands. So generated code matches hand-written code on sight.
- **Code Map** - a table of each source file: what it owns + key types. Find the right file
  without walking the tree.
- **Key Invariants** - see below.

Keep it laconic: link, don't restate what the code already says.

### Key Invariants (the crown jewel)
A standing section anyone - human or agent - appends to when they discover or create a
cross-cutting rule: something no single file owns, that a change in a *neighboring* file could
break silently. Each entry names the files and the *why*.

Example: "Vertex attribute layout in `mesh_format.h` must match the input-layout binding in
`pipeline_state.cpp` - two halves of one contract, nothing checks them at compile time." An
agent asked to touch either reads this first and doesn't desync them. Single-file rules go as
an inline comment instead; this list is only for tripwires that span files.

### Scoping a feature for the agent
So agent output lands as if a teammate wrote it: point it at the nearest existing example
("match how `ShadowPass` is structured") and the conventions file; keep the change
self-contained (one subsystem, one seam) so a human can review and later edit it in isolation;
require it to reuse existing helpers rather than add parallel machinery.

### Tests: unit + integration
- **Unit** - fast, hermetic, especially over the pure decision functions from D6.
- **Integration** - the real system end to end; where it depends on non-deterministic pieces,
  assert the robust facts (didn't hang / no error / right thing happened), not exact output.

Any user-observable behavior change ships its test in the same change.

### Record cross-boundary decisions (lightly)
When a change crosses a system boundary (swapping a library, reshaping a subsystem, changing a
data format or threading model) *and* you rejected a reasonable alternative for a non-obvious
reason, write a short note - Context / Decision / Rejected-alternatives-and-why - by the code
or in a decisions folder. Rare: interface refactors and file moves don't qualify. The
rejected-alternatives line is what pays off later.

---

## THE SHORT VERSION

Simplicity first: minimum code, no interface without a real second implementation (in C++ a
virtual base is a cost you pay deliberately), delete rather than accumulate, and prefer a
data-driven design where it fits better than a class hierarchy. One clear ownership tree, RAII
on every resource, composition over inheritance, one responsibility per class and file, hard
logic in pure testable functions, self-explanatory code over comments, reuse before writing,
fail loud never silently. Think before coding when the
requirement is ambiguous; make surgical changes that match house style; loop until a verifiable
goal is met. Keep a laconic shared `CLAUDE.md` with a Code Map and a standing "Key Invariants"
list of cross-file tripwires anyone can append to, and ship each behavior change with its test.
