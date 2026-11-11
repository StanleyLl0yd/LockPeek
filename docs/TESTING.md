# Test plan and verification status

## Automated tests included

`LockPeekTests.exe` performs deterministic unit checks for IPC validation, native-handle buffer bounds, path and command-line escaping, and Unicode acceptance. It then creates **its own** temporary file and launches two child file holders without delete sharing. Where the filesystem supports hard links, the test also checks volume/file-ID matching through an alternative path. A third file holder allows delete sharing to confirm that the scanner reports an open handle without assuming that it blocks deletion. `RunIsolatedScan` must discover both child PIDs; after cooperative release it must stop listing them. A missing file must report a Win32 error. All temporary files and handles are owned and cleaned up by the test program.

GitHub Actions is configured to build Win32 x64 with `/MT`, run these tests, validate imports, PE hardening and file size, and upload the unsigned binary and diagnostic report as CI artifacts. When the runner has stable MSVC 14.51 rather than 14.52+, CI uses an explicitly marked **provisional** `/std:c++23preview` development run. That run can validate behavior but cannot qualify a 1.0 release. Workflows have only `contents: read` permissions.

## Provisional Windows verification (11 November 2026)

- [Exact-head Windows CI #12](https://github.com/StanleyLl0yd/LockPeek/actions/runs/42968161722): successful build, 35/35 unit and integration tests PASS, PE/CRT/CFG/ASLR/DEP checks PASS, CI artifact uploaded.
- Windows Server 2025 hosted runner, Visual Studio 2026/MSVC 19.51.36260, development-only `/std:c++23preview`.
- Executable: 229,376 bytes (224 KiB). Imports: KERNEL32, USER32, GDI32, COMCTL32, COMDLG32, SHELL32, ADVAPI32. No dynamic VC++ CRT imports. Embedded application manifest independently inspected.
- Integration PASS includes one/two holders, hard-link alias, holders disappearing after release, delete-sharing semantics, missing-file error, cancellation, and Unicode filenames.
- Expected confidence limitation: the runner reported inaccessible/uninspectable third-party handles, so its results correctly carry the partial flag. Found test holders were still matched by volume/file ID.

## Remaining release blockers and manual QA

- Build with **confirmed stable** MSVC 14.52+ using `/std:c++23`. A 14.51 preview-language build is not release-qualified.
- Boot clean **Windows 10 x64** and **Windows 11 x64** systems without VC++ Redistributable or .NET installations; verify standalone startup and that only system DLLs are needed.
- Exercise GUI selection, Explorer drag/drop, keyboard access, copy-to-clipboard, recheck, scaling at 100%/150%/200%, Russian/English UI, Unicode and extended paths, and user-canceled UAC.
- Exercise waiting with an exclusive file holder, explicit cancellation, holder exit, and path replacement between scans; confirm messages never promise deletion is safe.
- Check actual memory/CPU at idle and during a scan, and failure behavior against inaccessible processes and a deliberately slow filter/network path.
- Before merge, attach clean-machine QA evidence, require successful exact-head CI in the main branch ruleset, and perform the final repository-wide verification. Do not create a GitHub Release without separate authorization.
