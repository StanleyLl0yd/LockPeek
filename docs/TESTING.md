# Test plan and verification status

## Automated tests included

`LockPeekTests.exe` performs deterministic unit checks for IPC validation, native-handle buffer bounds, path and command-line escaping, and Unicode acceptance. It then creates **its own** temporary file and launches two child file holders without delete sharing. `RunIsolatedScan` must discover both child PIDs; after cooperative release it must stop listing them. A missing file must report a Win32 error. All temporary files and handles are owned and cleaned up by the test program.

GitHub Actions is configured to build Win32 x64 Release with C++23 and `/MT`, run these tests, validate imports, PE hardening and file size, and upload the unsigned binary and diagnostic report as CI artifacts. Workflows have only `contents: read` permissions.

## NOT VERIFIED (release-blocking)

- Clean Windows 10 and clean Windows 11 x64 execution, including no separately installed VC++ or .NET runtimes.
- Compilation with **stable** MSVC 14.52 or later. MSVC 14.52 was release candidate per public Microsoft documentation at the time this source package was prepared.
- SystemExtendedHandleInformation ABI and file type index on each target build.
- Live integration test outcomes (single holder, dual holder, release, wait/cancel, denied protected process, Unicode, network path, race/timeout).
- GUI UI behavior, DPI, keyboard and screen reader accessibility, UAC and drag/drop, resource leaks.
- Actual executable file size, PE section sizes and DLL import list, CFG flags, idle memory and CPU.

A successful CI job is **not equivalent** to clean Windows 10/11 testing. A manual QA pass on both clean VMs is required before 1.0 Release Candidate acceptance. No GitHub Release should be published without separate authorization.
