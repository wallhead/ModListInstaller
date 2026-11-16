# Production P1 Review Fixes

Date: 2026-11-06. Baseline: `0e6c612f1208a55d3fc878a432928fba6c872cd3`.
Scope approved by the user's "fix": F01-F04 from the full-project review.
Earlier local R1-R3 tooling fixes are preserved, not reverted or published.

## Archive Contract (F01)

Root cause: pipeline eligibility considered chunk metadata, not archive format.
Both path inspection and extraction created the 7z handler, even for ZIP, and
treated `S_FALSE` from Open as success. Later property/extraction success could
overwrite that failed-open status and lead to a zero-item installation.

The handler now follows the first archive part's 7z/ZIP format, including split
suffixes and case-insensitive extensions. Both Open sites require `S_OK`.
Extraction success also requires `S_OK`, successful callbacks and full final
input verification. No fallback without integrity checks was introduced. The
verified-block scheduler, four-thread default and SHA-before-use are unchanged.
Genuinely opened empty archives remain allowed.

The format regression ran before the fix and reproduced false success for
correctly hashed nonarchives, failed ZIP inventory inspection and false success
for ZIP mislabeled as 7z. Afterwards, the complete core suite passed, including
7z/ZIP split and unsplit, uppercase ZIP, malformed input, wrong handler, genuine
empty archives, Unicode filenames and empty files/directories.

SDK format identifiers were cross-checked against the upstream
[26.02 GUID contract](https://github.com/ip7z/7zip/blob/f9d78aff31a5f2521ae7ddbdc97c4a8855808959/CPP/7zip/Guid.txt).

## Packer Deployment And Replacement (F02/F03)

The matched 26.02 console, codec DLL and license are embedded/deployed together.
Each cache component is checked byte-for-byte before use, including same-size
tamper repair. Sources and checksums are documented in
`PackerApp/resources/7zip/README.md`. No new dependency download was required.

The Worker preflights before changing previous outputs. Replacement archives,
installer and manifest are built in a fresh owned workspace on the release
volume. Manifest close/write state is checked before publication (also addressing
the F13 prerequisite to safe publication). Scoped previous outputs are backed up;
the replacement manifest is published last. Failure/cancellation rolls back.
Unrelated files and directories are not recursively removed. Failed rollback
retains named recovery files and a promotion journal rather than deleting them.

Extra free space is conservatively checked against the source-size estimate,
installer size and max(16 MiB, source/16). This may reject a highly compressible
replacement that could physically fit; it prioritizes preserving the live package.
The sequence is recoverable, not a crash-atomic directory swap or protection
against concurrent external writers.

RED evidence: old code passed only 20/22 initial tests, exposing missing codec
deployment and deletion of the previous package when the installer was absent.
Additional regressions cover close/promotion/ownership failures. Independent
review also caught case-only publication mismatches, unsafe Manifest Only installer
refresh and stale completion status. These are fixed: Windows path comparison is
consistent; Manifest Only stages only the installer/manifest pair while hashing
unchanged live archives; successful promotion is the commit point, so a later
Stop cannot change Ready into Stopped. Pre-commit cancellation still rolls back.

The post-commit status regression failed 42/43 before its fix. The final main-agent
run passed all 43 packer tests, retaining fixtures at
`PackerApp/build/windows/x64/release/test-fixtures/flat-layout-40296-331773343-0`.
Raw final output is `E:/Test builds/Modlist-P1-20261106/reports/packer-tests-final.txt`.
The private console's `i` output explicitly names its matching cached 26.02 DLL;
it is recorded in `reports/packer-private-codecs.txt` under the same artifact root.

## Cleanup Safety (F04)

Preparation records resolved directory identity; clearing reopens and verifies it.
Directory components and children are opened relative to pinned handles with
no-follow semantics, enumeration is handle-based, and deletion targets opened
objects rather than recursive path strings. Package overlap, changed identity,
linked roots/ancestors, nested reparse entries and volume/share roots fail closed.
This also handles an attribute-only junction conversion after an earlier check;
merely retaining a directory handle does not freeze its reparse metadata.

Staging ownership is a locked `owner` file at
`UserRuntimeFolder(stageFolder)/staging-owner/owner`, outside the payload and
destination namespace. It supports drive-root ACLs that allow subdirectory creation
but not files. Ownership persists as PENDING before the final empty/identity check,
then becomes COMMITTED. Pending/truncated records never authorize cleanup; a
nonempty pending claim requires manual inspection. Active, foreign and unmarked staging
are not offered as this package's leftovers. Explicit confirmed final-folder
overwrite remains separate and does not require an installer ownership marker.
Completion removes only the pinned empty staging root and owned metadata; empty
per-stage runtime parents may remain. UI, worker and helpers reject runtime-cache
overlap, including ancestors, descendants and resolved aliases.

Cleanup preflights the tree before deleting entries. Entry/depth limits default
to 1,000,000 and 512, with entry budget checked before every child acquisition,
including flat directories. An unsupported/inaccessible/oversized tree is refused,
not partially traversed through a link. Very large preflight trees may still need
manual cleanup if the OS cannot provide all required handles.

RED evidence includes the original private-junction sentinel deletion, attribute
mutation/raced-claim regressions, an actual subprocess termination during claiming,
the drive-root-style ACL failure introduced by a regular sibling marker file, and
a valid final archive name colliding with sibling metadata. The final main-agent
run passed 31 cleanup cases, including root/child/ancestor and metadata mutation
barriers, stale/foreign ownership, pending recovery/refusal, runtime overlap,
flat-tree budgets and the create-directories-only parent ACL. Barriers
wrap real native calls only in the test translation unit; production classes have
no test hooks. Real volume roots were inspected read-only, never cleared.
Raw output: `reports/folder_cleanup_tests-final.txt` under the artifact root.

Independent F01, packer and final F04 security reviews are clear, with no remaining
actionable findings within the approved four-P1 scope.

## Integration Evidence

All test artifacts are under `E:/Test builds/Modlist-P1-20261106`; installation
outputs are under `D:/MI-P1-20261106`. No existing user archive was corrupted.

`installer_workflow_tests` embeds the current tools and includes the actual UI
worker implementation, following the existing UI test pattern. Copies run beside
isolated package inputs, capture production messages with a message-only HWND,
and invoke the real `RunInstallWorker` and finalizer. No desktop automation or
changes to real Documents/game folders are performed.

Before cleanup integration, all ten native-fixture combinations (7z, split 7z,
ZIP, split ZIP, uppercase ZIP; direct and staged) passed. Independent external
SHA-256 checks matched all ten payload trees with their source, including empty
directories. Malformed input and wrong-handler input were rejected by the real
worker. Both 7z and ZIP direct workers also rejected one changed byte in the
second copied volume and emitted the validation-failure message.

After the final forced rebuild, the actual production worker passed packer-built
split 7z, ZIP and a Cyrillic archive/package/path fixture through direct and staged
routes. Two further 7z/ZIP cases containing legitimate ownership-like payload
filenames also passed. All ten final trees matched independent full SHA-256 and
directory-set checks. Successful output parents contain only `installed`, without
leftover staging or metadata directories. Originals remained byte-for-byte unchanged.
Reports are `checked-packer-{7z,zip,unicode}/content-verification.json` and
`checked-marker-{7z,zip}/content-verification.json` under the artifact root.
The exact formerly colliding final name `.Unpacking.modlist-staging-owner` also
passed direct/staged production workers after failing the staged RED run. Isolated
runtime-cache destinations were refused by both worker routes without installation.

Fourteen final worker cases also passed: genuine empty 7z/ZIP, correctly hashed
nonarchives, ZIP mislabeled as 7z, and same-size corruption in the second 7z/ZIP
volume, each in direct and staged mode. Corruption required both SHA rejection
and the production validation-failure signal. These were rerun against the final
pending/cache build; reports use `checked-*` and `reports/checked-contract.json`.

All-target forced Release builds passed for both applications. Core, scheduler,
cleanup, native UI and diagnostics unit suites passed, as did diagnostics integration
(both orders, final counters, cancellation, report-write failure and invalid paths).
Its latest report is
`InstallerApp/build/windows/x64/release/logs/ModlistBench-2026-11-06_13-02-11-54152-10126489447067722828`.
Earlier tooling regressions also passed again (source safety, forced termination,
copy failure, reordered/whitespace/UTF-8 archive description). The external payload
verifier's self-test passed. Embedded GUI JSON/CSS and packer EXE/DLL/license were
extracted from the built resources and matched source hashes exactly; see
`reports/embedded-asset-verification.json`.

## Ready Test Build

`E:/Test builds/Modlist-P1-20261106/ready` contains the current GUI installer as
`modlist-installer.exe`, the portable `modlist-packer.exe`, embedded-tool license,
this report and `provenance.json`. Both EXEs were copied from the tested Release
outputs, with SHA-256 recorded in provenance. Their final embedded assets match
the source bytes. No tracked distribution EXE was overwritten. Version text is
unchanged; this is an uncommitted test build, not a published release.

## Remaining Qualification

- A clean Windows VM was not used; clean-machine launch is not qualified here.
- F05 and other remaining P2/P3 findings were not included in the four-P1 scope.
- The installer still imports VC++ runtime DLLs; this was inspected, not fixed.
- Real UNC-share cleanup, full GUI click-through and large-workload timing are
  not established by the small headless worker/owned-control tests.
- No affected-user PC or large-workload speedup is measured by these functional tests.
- No commit, push, release, version bump or distribution-binary replacement is included.
