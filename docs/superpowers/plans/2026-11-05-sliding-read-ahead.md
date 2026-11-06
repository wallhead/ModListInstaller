# Sliding Read-Ahead Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans with test-driven development.

**Goal:** Remove avoidable verified-input stalls without weakening validation.

**Architecture:** Internal production scheduler owns queue, cache and in-flight
state; the existing verified stream owns Windows reads, hashing and progress.

**Tech Stack:** C++20, Win32 overlapped I/O, BCrypt, xmake, existing C++ tests.

**Spec:** ../specs/2026-11-05-sliding-read-ahead.md

## Global Constraints

- Preserve dirty working-tree changes and work in this existing checkout.
- Keep full SHA-256, four-block read-ahead and six chunk buffers.
- Do not change decoder/output/UI policies or commit, push or publish.

## Review Focus

- Header/tail/backward seeks must not publish a block under the wrong index.
- Retained buffers must not defeat the memory bound or demand progress.
- Cancellation and destruction must drain I/O before freeing buffers.
- Final verification must cover chunks not requested by the decoder.
- Corrupt or missing input must fail without exposing unverified data.

## Task 1: Reproduce And Correct Scheduling

- [x] Run existing core tests as baseline.
- [x] Add deterministic tests and extract the current production scheduling policy.
- [x] Observe the sequential cache-hit test failing against that policy.
- [x] Correct window advancement, eviction, promotion and in-flight joining.
- [x] Run scheduler tests plus existing real split-archive tests.

Files: src/extractor/VerifiedBlockScheduler.h, src/extractor/PipelinedSevenZipExtractor.cpp,
tests/test_pipeline_scheduler.cpp, tests/test_main.cpp, xmake.lua, tests/CMakeLists.txt.

## Task 2: Verify Integration And Deliver A Test Build

- [x] Exercise seeks, final verification, cancellation and corruption regressions.
- [x] Run core, UI, diagnostics unit/integration tests and Windows Release builds.
- [x] Review the patch independently and address material findings.
- [x] Place an installer test executable in a new folder under E:/Test builds.
- [x] Record results and explicitly leave full-package performance gain unmeasured.

## Execution Record

Design scope approved by the user's "lets fix" after the scheduler-first proposal.
No worktree/branch migration: existing uncommitted diagnostics and UI changes are
dependencies of this test build and must remain intact.

Baseline core tests passed. The extracted original policy still passed core tests
and failed the new test with: "Consuming a cache hit must prefetch chunk 5 before
a demand miss". Corrected policy passes all seven deterministic scheduler cases
and the original real split-archive suite. Windows Release build of all targets
passes. Additional real cancellation/output-failure/unused-tail cases also pass.

Final verification on 2026-11-06: all nine production-scheduler cases, core tests,
native UI tests, diagnostics unit tests, diagnostics integration tests (including
both extraction orders), and `xmake build -a` passed. Initial diagnostics
integration under the deep workspace parent hit its documented 240-character
guard; repeating with TEMP as destination parent passed without code changes.

Independent review found no actionable issues. Added live-buffer tracking and
in-flight final-verification tests from review suggestions. An additional failing
demand-under-borrowed-pressure test justified evicting the farthest unborrowed
speculative block when essential demand has no obsolete victim; it now passes.
No deterministic pending-Windows-read injection test was added; real active
cancellation and output failure exercise ordinary teardown but cannot guarantee
the read was pending at that instant. Full-package speedup remains unmeasured.

Test installer: E:/Test builds/Modlist-Installer-20261106-read-ahead/modlist-installer.exe

SHA-256: A69D2C5399CB6E078F5C6F240D8702C10DAFA2CB19ACC0455E4FAC0B1DCF8474

Diagnostics smoke report:
InstallerApp/build/windows/x64/release/logs/ModlistBench-2026-11-06_00-07-41-51972-13752526551576701472

No version bump, dist replacement, commit, push or release was performed.

Cleanup of the root-level `.xmake/` and `build/` created by one `xmake -P`
invocation was rejected by the execution policy. They remain untracked; no
alternative deletion method was attempted. Normal InstallerApp build outputs and
the delivered test executable were not targeted.
