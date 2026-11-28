# Contributing to OpenEra

Thanks for helping. This guide covers what reviewers check: where information may come from, how to set up the toolchain and run the same checks as CI, the code style, and how to report how a title behaves.

Before starting on something large, say hello on [Discord](https://discord.gg/openera) or comment on the issue, so work isn't duplicated. Small, focused pull requests are much easier to review than large ones.

## Clean-room policy

OpenEra reimplements behaviour; it never copies it. Every structure, constant, flag value and API behaviour in this repository has to come from a source anyone could check. This is the most important rule in the project. A pull request that breaks it can't be merged, however good the code is.

### Allowed sources

- **Public documentation:** Microsoft Learn, the public GDK documentation pages, the PE Format specification, AMD's public ISA documents, and other published specifications.
- **Observable behaviour:**
  - the inputs, outputs and side effects of calls, observed by running **titles you own**;
  - OpenEra's own logs, such as import tables, the unimplemented-call summary and arguments logged at runtime.
- **Open-source projects under a compatible licence, with attribution.** OpenEra is GPL-3.0, so code may only be used from projects whose licence allows that (for example MIT, BSD, Apache-2.0, GPL-3.0). From projects with an incompatible licence, such as GPL-2.0-only, you may use **facts** (field offsets, constant values, what a flag means), never code. Don't port their code by translating or rewriting it either.

### Never

- Leaked SDKs or source code, or code derived from them.
- Disassembly or decompilation of Microsoft binaries, including console system modules and the Gaming Runtime.
- Headers, libraries or samples from the Microsoft GDK. The public GDK documentation may be *read* for reference, but its headers must not be copied or paraphrased line by line: write OpenEra's own declarations from the documented behaviour.
- Encryption keys, certificates, console firmware, or any proprietary blob.
- Game files, dumps or assets, in the repository, in issues, or in pull requests.
- Loading Xbox system DLLs from a console dump. OpenEra only maps title modules, and uses its own shims and the host's Windows DLLs.

### Attribution

When a fact comes from an open-source project, say so where it's used, normally at the top of the header that declares it:

```cpp
// XVD/XVC container layout. Field offsets, sizes and constants are facts
// documented by xvdtool (https://github.com/emoose/xvdtool, GPL-2.0);
// no code was taken from it.
```

Name the project, link it, give its licence, and say whether code was used or only facts. When the source is documentation, name the page (for example "Microsoft Learn: `VirtualAlloc`"). Put the same references in the issue or pull request under **Public references**.

### When you're unsure

Ask before writing the code: in the issue, or on Discord. If you can't point to an allowed source for something, leave it out and record it as unknown instead, for example as a stub, which the unimplemented-call summary will report. A missing feature can be added later; tainted code has to be removed from the history.

## Setting up

You need Windows 10 (22H2 or later) or Windows 11, x64, and:

| Requirement | Notes |
| :--- | :--- |
| **Visual Studio 2022** or newer | Workload **Desktop development with C++**, plus the components **C++ Clang compiler for Windows** and **C++ CMake tools for Windows** (which bring CMake and Ninja). Any edition works. |
| **Windows 11 SDK** | 10.0.22621.0 or newer. |
| **clang-format and clang-tidy 22.1** | CI pins 22.1. Visual Studio's bundled LLVM may be a different version and format slightly differently. To match CI, install them with `pip install "clang-format~=22.1.0" "clang-tidy~=22.1.0"`. |

Open PowerShell in the repository and load the Visual Studio developer environment. This is the same script CI uses:

```powershell
. .github/scripts/Enter-VsDevShell.ps1
```

It finds the newest Visual Studio with the x64 C++ tools. It puts `cl`, CMake and Ninja on `PATH`, adds Visual Studio's LLVM tools after them, and sets `$VsClangCl` to Visual Studio's own `clang-cl`.

## Verification gates

Run these before pushing. CI runs the same checks on every pull request, and they must all pass.

**1. Format** every changed `.h` / `.cpp` file:

```powershell
clang-format -i <changed files>
```

**2. Build and test with MSVC and C++ Core Check** (`/W4 /WX /analyze`). The configure output must include `C++ Core Check: enabled`:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

**3. Build and test with Clang-CL:**

```powershell
cmake -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_COMPILER=$VsClangCl"
cmake --build build-clang
ctest --test-dir build-clang --output-on-failure
```

**4. clang-tidy** on every source file, using a Clang-CL compile database:

```powershell
cmake -B build-tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_COMPILER=$VsClangCl" -DOPENERA_ENABLE_CORE_CHECK=OFF
git ls-files 'src/*.cpp' 'tests/*.cpp' | ForEach-Object { clang-tidy -p build-tidy --quiet $_ }
```

**5. Compatibility docs.** If you change `docs/compatibility/api-status.json`, regenerate the coverage image and the API matrix in `COMPATIBILITY.md`, then check them:

```powershell
./tools/Update-Compatibility.ps1
./tools/Update-Compatibility.ps1 -Check
```

Console-only APIs live in `docs/compatibility/gdkx-status.json`. Regenerate them with `./tools/Update-GdkxCoverage.ps1`, or add new ones from a report's unimplemented-call summary with `./tools/Import-GdkxReport.ps1 -Log <file> -Source '#<issue>'`.

**6. Check the diff:** run `git status` and `git diff`. Make sure no build output, logs, dumps or other unintended files are included.

Warnings are errors with both compilers, and every enabled clang-tidy check is an error. Fix the cause rather than suppressing it. Where a suppression really is right, for example a Win32 structure that is a union by definition, keep it as narrow as possible (one function or one line) and say why next to it.

## Code style

The rules are enforced by [`.clang-format`](.clang-format) (layout) and [`.clang-tidy`](.clang-tidy) (naming and checks), and by C++ Core Check under MSVC.

- **Language:** C++20 for Windows x64, built with MSVC v143+ and Clang-CL.
- **Layout:** Microsoft style. Allman braces, 4-space indent, 120 columns, braces always.
- **Naming:**

  | Construct | Convention | Example |
  | :--- | :--- | :--- |
  | Namespaces, classes, structs, enums | `PascalCase` | `OpenEra::Loader`, `class ModuleManager` |
  | Functions and methods | `PascalCase` | `ParsePe()`, `RegisterLoaderShims()` |
  | Local variables and parameters | `camelCase` | `sectorOffset` |
  | Private / protected members | `m_` + `camelCase` | `m_contentSize` |
  | Public fields of POD / ABI structs | `PascalCase`, fixed-width types | `uint32_t SectorCount;` |
  | Constants and `constexpr` | `k` + `PascalCase` | `kPageSize` |
  | Macros (avoid them) | `UPPER_SNAKE_CASE`, `OPENERA_` prefix | `OPENERA_ASSERT` |
  | Files | `PascalCase.h` / `PascalCase.cpp` | `XvdReader.h` |

- **Not allowed:**
  - raw `new` / `delete`: use RAII (`std::unique_ptr`, `Microsoft::WRL::ComPtr`, scoped handle wrappers);
  - C-style casts: use `static_cast`, `std::bit_cast`, or a narrowly suppressed `reinterpret_cast` where unavoidable;
  - pointer-and-length pairs across internal APIs: use `std::span` / `std::string_view`;
  - `printf` and raw C string buffers: use `std::format` and the logger in `src/Core/`.
- **Headers:**
  - `#pragma once`;
  - includes in the order: the file's own header, the C++ standard library, Windows / DirectX headers, then project headers;
  - keep `<Windows.h>` out of public headers in `src/Core/` and `src/Loader/` where possible.
- **Binary layouts:** any struct that maps an on-disk or ABI layout uses explicit packing or alignment, and a `static_assert(sizeof(...) == ...)`.
- **Errors:**
  - Return `HRESULT` / `NTSTATUS` at Windows and COM boundaries. No exception may escape a shim: wrap shim bodies in `Kernel::GuardShim` / `GuardHResult` / `GuardNtStatus`.
  - Use `std::optional` or status values for expected failures, and mark non-throwing functions `noexcept`.
- **Comments:** sparse. Explain why something is done, and cite sources; don't narrate what the code says.
- **Where things go:**
  - `src/Core/`: logging, binary readers, XML, utilities.
  - `src/Loader/`: the PE loader, the import resolver and module start-up.
  - `src/Kernel/`: system shims (files, memory, threads, debug output) and the stub registry.
  - `src/Graphics/`: DXGI / Direct3D 12.
  - `src/App/`: the `openera` launcher.
  - `tests/`: Catch2 tests. Test images that OpenEra maps itself go in `tests/Fixtures/`.

New parsers and binary structures need unit tests. Shims should be tested the way titles reach them, through `ShimRegistry` or a test image under `tests/Fixtures`, as well as with unit tests for any pure logic.

## Commits and pull requests

- **Commit messages:** `subsystem: summary`, one concise line in the imperative. For example `loader: register unwind data for mapped images`, or `kernel: copy-on-write title file layer`. Subsystems in use: `core`, `loader`, `kernel`, `graphics`, `app`, `tests`, `tooling`, `docs`. Details belong in the pull request, not the commit message.
- **Branches:** work on a branch such as `feat/...`, `fix/...` or `docs/...`, never on `main`.
- **Pull requests:**
  - Fill in the [pull request template](.github/pull_request_template.md): summary, reviewer notes, testing and checklist.
  - Link the issue (`Closes #12`, or "Part of #12" for partial work).
  - List any public references you used.
- **Testing notes:** say which compilers you built with, and what you ran by hand.

## Releases

Releases come from the `release` branch. Merging a pull request into it runs [the release workflow](.github/workflows/release.yml):

1. It builds and tests a Release configuration with MSVC and Clang-CL.
2. It packages `openera.exe` and `OpenEraTlsSlot.dll` as `OpenEra-<version>-win-x64.zip`, with a SHA-256 checksum.
3. It publishes them as a GitHub release tagged `v<version>`, with generated notes. 0.x versions are marked as pre-releases.

The version is the `project(OpenEra VERSION ...)` in `CMakeLists.txt`, so bump it in the pull request. A version that's already released fails the workflow. Pushing directly to `release` doesn't publish anything.

## Compatibility reports

Use the **Compatibility report** issue template to report how a title behaves under OpenEra.

- **Titles:** only report titles you own, run from your own unpacked title folder.
- **Never attach, upload or link game files, dumps, keys or packages,** not even "just one file" for debugging.
- **Logs:** run with `--log-file` and paste the relevant part of the log, including the unimplemented-call summary, into the report. Remove anything personal first, such as your user name in paths.
- **Details:** give the OpenEra commit you tested, your Windows version, CPU and GPU, and the furthest point the title reached.
- **Problems in OpenEra itself:** open a separate issue for each, linked from the report, so each can be tracked and fixed on its own.

## Questions

Ask on [Discord](https://discord.gg/openera). Blank issues are disabled, so use the templates for bugs, features and compatibility reports.
