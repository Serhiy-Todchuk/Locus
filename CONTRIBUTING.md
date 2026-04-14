# Contributing to Locus

## Code Style

C++20. MSVC. No exceptions to the style rules below.

- **Naming**: `snake_case` functions/variables, `PascalCase` classes/structs, `k_name` constants
- **Namespace**: everything in `namespace locus`
- **Files**: `src/name.h` + `src/name.cpp` side by side, no `include/` dir
- **RAII**: wrap external resources (SQLite, handles). Delete copy ops. `std::unique_ptr` for ownership.
- **Errors**: `std::runtime_error` for fatal. `spdlog::warn` for recoverable.
- **Headers**: `#pragma once`. Minimal includes — forward-declare where possible.
- **No**: exceptions in hot paths, `using namespace std`, raw `new/delete`, Boost

## Build

Prerequisites: CMake 4+, Visual Studio 2026, vcpkg (set `VCPKG_ROOT` env var).

```
# Configure (once)
cmake -B build/debug -G "Visual Studio 18 2026" ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets

# Build
cmake --build build/debug --config Debug
```

Output: `build/debug/Debug/locus.exe`

## Run

```
build\debug\Debug\locus.exe <workspace_path> [--endpoint URL] [--model NAME] [-verbose]
```

- `<workspace_path>` — folder to index (required)
- `--endpoint` — LLM server URL (default `http://127.0.0.1:1234`)
- `--model` — model name (default: server default)
- `-verbose` — trace-level logging to stderr + `.locus/locus.log`

Requires LM Studio (or any OpenAI-compatible server) running at the endpoint.

## Tests

```
# Run all tests
cd build/debug
ctest -C Debug --output-on-failure

# Run tests for a specific stage
build\debug\tests\Debug\locus_tests.exe "[s0.6]"

# Run a single test by name
build\debug\tests\Debug\locus_tests.exe "ReadFileTool: basic read returns line-numbered content"

# List all tests
build\debug\tests\Debug\locus_tests.exe --list-tests
```

Tests use Catch2. One `tests/test_<topic>.cpp` per subsystem. Tag with stage: `[s0.2]`, `[s0.6]`.
Test names: ASCII only (CTest mangles Unicode on Windows).
