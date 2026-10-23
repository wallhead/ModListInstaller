Direct extraction for short destination paths.

- The installer inspects archive file and folder names before extraction.
- If every complete destination path fits within 240 UTF-16 code units, files are extracted directly into `<selected install root>\<archive_name>` and the final move is skipped.
- Longer paths or unavailable archive metadata use `<drive>:\Unpacking`, followed by the existing move into the final folder.
- Path inspection runs in the background. The log records the entry count, longest destination path, and chosen extraction folder.
- Full SHA-256 validation remains enabled. Direct SDK extraction checks the paths again against verified archive metadata before writing files.
- Retrying a failed or cancelled direct install asks before clearing the final folder. Choosing No preserves the files and returns to the main window.
- Updated installer text explains the automatic extraction choice.
- Corrected the Windows application manifest resource so Windows can read the packaged version and DPI declarations.
- Installation roots must be fully qualified, so the path budget includes the full destination.

The setup ZIP contains the portable packer, matching installer, editable CSS and text catalogue, and 7-Zip license. Keep the installer beside the packer.

Verification: release builds and core tests, including real split-archive extraction, exact 240/241-character boundaries, Unicode paths, staging fallback, cancellation, and corrupt archive rejection. Both extraction routes were also tested through the native installer UI with matching output hashes.
