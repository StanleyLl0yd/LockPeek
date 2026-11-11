# Test plan and verification status

## Automated tests included

`LockPeekTests.exe` performs deterministic unit checks for IPC validation, native-handle buffer bounds, path and command-line escaping, and Unicode acceptance. It then creates **its own** temporary file and launches two child file holders without delete sharing. Where the filesystem supports hard links, the test also checks volume/file-ID matching through an alternative path. A third file holder allows delete sharing to confirm that the scanner reports an open handle without assuming that it blocks deletion. `RunIsolatedScan` must discover both child PIDs; after cooperative release it must stop listing them. A missing file must report a Win32 error. All temporary files and handles are owned and cleaned up by the test program.

GitHub Actions is configured to build Win32 x64 with `/MT`, run these tests, validate imports, PE hardening and file size, and upload the unsigned binary and diagnostic report as CI artifacts. When the runner has stable MSVC 14.51 rather than 14.52+, CI uses an explicitly marked **provisional** `/std:c++23preview` development run. That run can validate behavior but cannot qualify a 1.0 release. Workflows have only `contents: read` permissions.

## Provisional Windows verification (11 November 2026)

- [Exact-head Windows CI #20](https://github.com/StanleyLl0yd/LockPeek/actions/runs/42992621400) passed **47/47** native unit/integration checks on PR head `18b3273598ccb51391fea5d5ee3e72e33e99ea60`, including the bounded IPC scanner, waiting uncertainty gates, Unicode, two holders, hard links, delete sharing, missing files and cancellation.
- Windows Server 2025 hosted runner; Visual Studio 2026/MSVC 19.51.36260, **provisional** `/std:c++23preview`.
- Executable: 230,400 bytes; only seven Windows system DLL imports. No dynamic VC++ CRT imports; manifest/CFG/ASLR/DEP checks passed. GUI idle startup and forged-message smoke passed on the server runner. The binary's SHA-256 must be read from the corresponding run's `build-info.txt`.
- **None** of these checks qualifies a clean Windows 10/11 client deployment or a stable 14.52+ C++23 build.

## Client Windows 10/11 evidence procedure

Use two fresh **x64 client** virtual machines, Windows 10 22H2 and Windows 11. Install neither a standalone VC++ Redistributable nor an additional .NET SDK/Desktop Runtime before the first LockPeek test. Use the **same** unsigned EXE from a single, exact-head, stable-MSVC qualification artifact. Keep its `build-info.txt` and SHA-256. A registry software listing alone cannot establish that an image is clean.

Copy `scripts/qa-client.ps1` to each VM and run from an ordinary, **non-elevated** PowerShell console (substitute the artifact's SHA-256):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\qa-client.ps1 -Exe .\LockPeek.exe -ExpectedSha256 YOUR_64_CHARACTER_SHA256 -ReportDirectory .\qa-evidence
```

The script rejects a hash mismatch, Windows Server and non-x64 machines, then starts the GUI, verifies a main window, samples idle CPU/memory and writes `Windows-10-preflight.json` or `Windows-11-preflight.json`. **An automated preflight PASS is never manual QA acceptance.** Record OS build numbers, screenshot/video evidence and the two JSON reports. Test the following manually on **both** machines:

1. Cold startup and ordinary user operation without separately installed runtimes; no installation or external DLL required.
2. File picker, Explorer drag/drop, a quoted CLI path, keyboard focus/navigation, clipboard report, manual recheck.
3. RU/EN UI, Cyrillic/non-ASCII names, spaces, very long paths, and 100%/150%/200% DPI.
4. Two file-holder processes, delete-sharing case, inaccessible/protected holders, missing/deleted/replaced file, path identity, and explicit partial/incomplete notices.
5. Wait and cancel, holder release, timed-out or partial scans, slow paths/filters, and error handling; never report that deletion is guaranteed safe.
6. Explicit UAC acceptance and user-cancelled UAC. Elevated drag/drop from unelevated Explorer may be blocked by UIPI; it must not be represented as supported.
7. Idle and scanning CPU/memory, GUI responsiveness, and absence of orphaned scanner workers after normal completion/cancellation.

For every test, record PASS/FAIL/NOT-TESTED with screenshots or a reproducible note. Attach both VM reports and the manual test matrix to PR #1. The release gate is **not** closed when any mandatory case is missing.

## Remaining release blockers and manual QA

- Build with **confirmed stable** MSVC 14.52+ using `/std:c++23`. A 14.51 preview-language build is not release-qualified.
- Boot clean **Windows 10 x64** and **Windows 11 x64** systems without VC++ Redistributable or .NET installations; verify standalone startup and that only system DLLs are needed.
- Exercise GUI selection, Explorer drag/drop, keyboard access, copy-to-clipboard, recheck, scaling at 100%/150%/200%, Russian/English UI, Unicode and extended paths, and user-canceled UAC.
- Exercise waiting with an exclusive file holder, explicit cancellation, holder exit, and path replacement between scans; confirm messages never promise deletion is safe.
- Check actual memory/CPU at idle and during a scan, and failure behavior against inaccessible processes and a deliberately slow filter/network path.
- Before merge, attach clean-machine QA evidence, require successful exact-head CI in the main branch ruleset, and perform the final repository-wide verification. Do not create a GitHub Release without separate authorization.
