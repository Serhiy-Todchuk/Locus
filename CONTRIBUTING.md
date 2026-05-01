# Contributing to Locus

## Code Style

C++20. MSVC. No exceptions to the style rules below.

- **Naming**: `snake_case` functions/variables, `PascalCase` classes/structs, `k_name` constants
- **Namespace**: everything in `namespace locus`
- **Files**: `src/name.h` + `src/name.cpp` side by side, no `include/` dir
- **RAII**: wrap external resources (SQLite, handles). Delete copy ops. `std::unique_ptr` for ownership.
- **Errors**: `std::runtime_error` for fatal. `spdlog::warn` for recoverable.
- **Headers**: `#pragma once`. Minimal includes, forward-declare where possible.
- **No**: exceptions in hot paths, `using namespace std`, raw `new/delete`, Boost
- **No em-dashes anywhere** (--, U+2014). Use ASCII `-` or `--` in code, comments, commit
  messages, docs, test names, and log strings. Em-dashes break ctest test-name dispatch on
  Windows (no CP437 mapping) and complicate grep / diff.

## Prerequisites

- **CMake** 4.0+
- **Visual Studio 2026** (MSVC 19.50+) -- the generator string is `Visual Studio 18 2026`
- **vcpkg** -- typical install at `C:/vcpkg`. The commands below assume that path; substitute
  your own if it lives elsewhere, or set `VCPKG_ROOT` and use `%VCPKG_ROOT%`.

The first configure pulls every dependency through vcpkg in `x64-windows-static`
mode, plus fetches the Tree-sitter grammars via `FetchContent`. Plan for
**10-25 minutes** the first time. Subsequent builds reuse the cache and finish
in tens of seconds.

## Build layout

The repo uses **separate build dirs per configuration** -- not a single `build/` with a
`-DCMAKE_BUILD_TYPE` flag. This is required because vcpkg manifest-mode resolves
debug-vs-release dependencies at configure time, not at build time:

```
build/
  debug/      # configured with default Debug-flavoured deps
  release/    # configured the same way; multi-config generator picks Release at build time
```

Pick the build dir that matches your target configuration when running `cmake --build`.

## Configure (once per build dir)

```
cmake -B build/release -G "Visual Studio 18 2026" ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets

cmake -B build/debug -G "Visual Studio 18 2026" ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets
```

The custom triplet at `cmake/triplets/x64-windows-static.cmake` adds
`-D_USE_STD_VECTOR_ALGORITHMS=0` to work around missing SIMD intrinsics in
`libcpmt.lib` (static CRT) for the MSVC STL shipped with VS 2026.

## Build

The repo has five buildable targets:

| Target | What it is | Output (Release) |
|---|---|---|
| `locus`                    | CLI binary                                          | `build/release/Release/locus.exe` |
| `locus_gui`                | wxWidgets desktop app                               | `build/release/Release/locus_gui.exe` |
| `locus_tests`              | Catch2 unit-test suite                              | `build/release/tests/Release/locus_tests.exe` |
| `locus_integration_tests`  | Live-LLM end-to-end harness (manual only)           | `build/release/tests/integration/Release/locus_integration_tests.exe` |
| `locus_retrieval_eval`     | Recall@K / MRR / nDCG benchmark (manual only)       | `build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe` |

`pdfium.dll` is auto-copied next to `locus.exe` and `locus_gui.exe` by a
`POST_BUILD` step on each app target.

### Build everything (default target -- skips `_integration_tests` and `_retrieval_eval`)

```
cmake --build build/release --config Release
cmake --build build/debug   --config Debug
```

The default `ALL_BUILD` target builds `locus`, `locus_gui`, and `locus_tests`. The two
manual harnesses are excluded from default and have to be asked for by name.

### Build a single target

```
cmake --build build/release --config Release --target locus
cmake --build build/release --config Release --target locus_gui
cmake --build build/release --config Release --target locus_tests
cmake --build build/release --config Release --target locus_integration_tests
cmake --build build/release --config Release --target locus_retrieval_eval
```

For Debug, swap `build/release` -> `build/debug` and `--config Release` -> `--config Debug`.

### Common pitfalls

- **"not a CMake build directory (missing CMakeCache.txt)"** -- you used `build/` instead
  of `build/release/` or `build/debug/`. The build dir paths above are not optional.
- **Mid-build CRT mismatch on llama.cpp libs** -- the project's `CMakeLists.txt` patches
  llama-cpp / ggml imported targets to expose both Debug and Release `.lib` paths from the
  vcpkg layout. If you swap vcpkg versions and hit a `MTd/MT` linker error, re-configure
  the affected build dir from scratch.

## Run

### CLI

```
build\release\Release\locus.exe <workspace_path> [--endpoint URL] [--model NAME] [--context N] [-verbose]
```

### GUI

```
build\release\Release\locus_gui.exe [<workspace_path>]
```

If launched without an argument, the GUI shows a workspace picker. Once a workspace
is open, the path is remembered in the recent-workspaces list.

Flags accepted by both:

| Flag | Description |
|---|---|
| `--endpoint URL` | LLM server URL (default: `http://127.0.0.1:1234`) |
| `--model NAME`   | Model name (default: server default, auto-detected from LM Studio) |
| `--context N`    | Override context window size |
| `-verbose`       | Trace-level logging to stderr and `<workspace>/.locus/locus.log` |

LM Studio (or any OpenAI-compatible server) must be running at the endpoint.

## Tests

### Unit tests (Catch2, fast, hermetic)

```
# All tests, Release
build\release\tests\Release\locus_tests.exe

# All tests, Debug
build\debug\tests\Debug\locus_tests.exe

# Through ctest (slower wrapper, but registers per-case results)
cd build\release
ctest -C Release --output-on-failure

# Filter by Catch2 tag
build\release\tests\Release\locus_tests.exe "[s4.t]"
build\release\tests\Release\locus_tests.exe "[file-change-tracker]"
build\release\tests\Release\locus_tests.exe "[indexer]"

# Filter by test name (whole-name match, supports wildcards)
build\release\tests\Release\locus_tests.exe "ReadFileTool: basic read returns line-numbered content"
build\release\tests\Release\locus_tests.exe "WriteFileTool: *"

# List everything
build\release\tests\Release\locus_tests.exe --list-tests
build\release\tests\Release\locus_tests.exe --list-tags
```

The first Workspace-creating test loads `bge-small-en-v1.5-Q8_0.gguf` (~80 MB) once
per process via `tests/support/shared_embedder.h`; the same instance is shared across
all 38 Workspace tests. Without the shared embedder, the suite would re-load the
GGUF ~38x and run for several minutes; with it, the full suite runs in ~20 s.

### Integration tests (live LLM, manual only)

Not registered with `ctest`. Need [LM Studio](https://lmstudio.ai/) running at
`http://127.0.0.1:1234` with a tool-calling-capable model loaded (minimum verified:
Gemma 4 E4B @ 8k context).

```
# All scenarios with the live trace console window
build\release\tests\integration\Release\locus_integration_tests.exe -console

# One tag area at a time
build\release\tests\integration\Release\locus_integration_tests.exe "[smoke]"   -console
build\release\tests\integration\Release\locus_integration_tests.exe "[search]"  -console
build\release\tests\integration\Release\locus_integration_tests.exe "[outline]" -console
build\release\tests\integration\Release\locus_integration_tests.exe "[fs]"      -console
build\release\tests\integration\Release\locus_integration_tests.exe "[shell]"   -console
build\release\tests\integration\Release\locus_integration_tests.exe "[bg]"      -console
build\release\tests\integration\Release\locus_integration_tests.exe "[ask_user]" -console
build\release\tests\integration\Release\locus_integration_tests.exe "[slash]"   -console
build\release\tests\integration\Release\locus_integration_tests.exe "[file_change_awareness]" -console

# List registered cases
build\release\tests\integration\Release\locus_integration_tests.exe --list-tests
```

Without `-console`, stderr stays at warning-level and Catch2 prints summary only.
The full trace always lands in `<workspace>/.locus/integration_test.log`.

See [tests/integration/README.md](tests/integration/README.md) for the design,
prerequisites checklist, and per-tag coverage matrix.

### Retrieval eval (manual)

Measures recall@K, MRR, and nDCG@10 against the gold queries in
`tests/retrieval_eval/queries.json`. Heavy: opens a workspace, waits for the
embedding queue to drain, runs every query through three search modes.

```
build\release\tests\retrieval_eval\Release\locus_retrieval_eval.exe ^
    --workspace . ^
    --queries tests/retrieval_eval/queries.json ^
    --out tests/retrieval_eval/results.md
```

See [tests/retrieval_eval/README.md](tests/retrieval_eval/README.md) for baseline
management and the regression-check protocol.

## Adding tests

- Catch2. One `tests/test_<topic>.cpp` per subsystem. Tag with stage + topic:
  `[s4.t][file-change-tracker]`.
- Test names: ASCII only -- CTest mangles Unicode test names on Windows.
- New `.cpp` files go into the `locus_tests` source list inside `tests/CMakeLists.txt`;
  re-run the configure step on each existing build dir before the next build.

## Per-stage verification protocol

After every completed stage:

1. `cmake --build build/release --config Release` (full build).
2. `build\release\Release\locus.exe <workspace_path> -verbose` and exercise the
   stage's features against `d:\Projects\AICodeAss\` (WS1).
3. Quit, then read `<workspace>/.locus/locus.log` -- check for errors and confirm
   expected behaviour.
4. Update `roadmap.md` (mark `[x]`, add `OK` to the stage header) and the "Current Stage"
   line in `CLAUDE.md` before moving on.

The same CLI/GUI exe handles both prototyping and verification; there is no
separate "test build" step in this protocol.
