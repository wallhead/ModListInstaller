# Static Runtime Deployment Fix (F05)

Date: 2026-11-06. This follow-up extends the completed four-P1 fix scope only
to the installer VC++ runtime dependency and its build/deployment checks.

## Cause And Fix

The normal release script disables libtorrent. The installer's xmake targets
previously selected static CRT only when that optional backend was enabled;
the default build therefore imported MSVCP140, VCRUNTIME140 and related DLLs.

All Windows xmake targets now inherit `MT`, matching the packer and the previous
optional release-built static-library policy. The core, executable, test and
diagnostics objects use one policy. The CMake path selects
`MultiThreaded$<$<CONFIG:Debug>:Debug>` before creating targets. Non-MSVC CMake
builds and non-Windows xmake builds are unchanged.

No redistributable installer, network download or extra runtime DLL is added.
The original embedded matching 7-Zip console/DLL pairs remain unchanged.

## Regression Gate

`InstallerApp/scripts/test-runtime-dependencies.ps1` inspects actual PE imports
with dumpbin. It rejects MSVC/UCRT dynamic-runtime imports and fails on missing
tools/files, dumpbin errors or inconclusive empty parsing. Optional JSON reports
record each binary hash, dependencies and rejected imports. Both release scripts
invoke it for the shipped executable/installer and embedded EXE/DLL source assets.
It is a build-machine check, not an end-user requirement or an arbitrary recursive
DLL dependency resolver.

The gate failed against the pre-fix installer, naming its MSVCP/VCRUNTIME/UCRT
imports, then passed the rebuilt outputs. The retained old installer remains a
negative regression fixture and was rejected again after the fix. The same new
gate also ran successfully under Windows PowerShell 5.1.

## CMake Verification Prerequisites

The first full CMake build exposed duplicate application manifests: app.rc embeds
ID 1 and the MSVC generator also produced ID 1. Targets using app.rc now disable
only linker-generated manifests, keeping the existing DPI-aware manifest. The
first CTest run also exposed the core fixture generator's source-directory
assumption. Its CTest working directory now matches xmake's existing source root.
These failures were reproduced before their fixes; Release CTest then passed.

## Evidence

Artifacts: `E:/Test builds/Modlist-StaticCRT-20261106`.
Disposable installed outputs: `D:/MI-StaticCRT-20261106`.

- Forced xmake Release all-target rebuild passed with libtorrent disabled.
- All 17 inspected xmake/product/tool binaries passed the runtime import gate.
- Core, scheduler, 31 cleanup, native UI and diagnostics unit cases passed.
- Diagnostics integration passed both orders, cancellation and failure reporting.
- All 43 packer cases passed; packer runtime policy itself was unchanged.
- Four real production-worker 7z/ZIP direct/staged installs passed independent
  full SHA-256 and exact directory-set checks. Original packages were unchanged.
- The full smoke-helper regression suite passed, including forced termination
  and fixture-copy failure without modifying original archives.
- CMake Release all-target build passed; all nine outputs passed the import gate;
  all four registered CTest suites passed.
- CMake Debug GUI/core-test builds also passed without dynamic runtime imports;
  the Debug core suite passed.

Raw test logs, import JSON and roundtrip content reports are retained under the
artifact root. Current ready-to-try EXEs are placed together in its `ready` folder.
They include the preceding P1 fixes and unchanged version text, not a new release.

## Limits

No clean Windows VM or affected-user PC was available. PE import checks and local
execution establish removal of these runtime DLL imports, not full clean-machine
qualification. Optional libtorrent builds were not executed. Other remaining
review findings are outside this follow-up. No commit, push, release, version bump
or tracked distribution EXE replacement is included.

Reference contracts: [MSVC static runtime](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library),
[CMake runtime selection](https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html),
and [dumpbin dependency inspection](https://learn.microsoft.com/en-us/cpp/build/reference/dependents).
