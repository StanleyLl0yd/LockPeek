# LockPeek

**A lightweight, portable Windows utility to identify processes with open handles to a selected file.**

LockPeek is a diagnostic tool. An open file handle **does not prove** that a process is preventing deletion, moving or renaming. Inaccessible processes, driver handles, share flags, remote filesystems and permissions may make the result incomplete.

## Intended features

- Native C++23 and Win32 GUI, x64 Windows 10/11, Russian and English (OS UI language).
- Select a single file, drop one from Explorer, or use `LockPeek.exe "C:\path\report.xlsx"`.
- Enumerate open file handles using `NtQuerySystemInformation(SystemExtendedHandleInformation)`, duplicate accessible file handles and compare by file identity or, when necessary, a clearly labelled heuristic path match. **No Restart Manager fallback.**
- Show all detected processes (up to 128 distinct matching PIDs), PID, matching method, obtainable executable path and Windows service display name where available.
- Manually recheck, poll every 2 seconds on explicit request, cancel waiting, copy a text report.
- Relaunch with UAC elevation on demand when available process permissions limit diagnosis.
- A separate, disposable invocation of **the same EXE** performs low-level scanning. A parent watchdog limits long-running/hung native requests; it may terminate **only its own worker process**, never another program holding a file.
- No installer, service, driver, telemetry, network request, process kill or force-close-handle feature.

## Build

The final release requires **Visual Studio 2026 / MSVC v14.52 or newer, confirmed stable**, the Windows SDK, and `/std:c++23`. MSVC 14.52 was publicly confirmed only as a Release Candidate when the project was prepared. **Provisional development/testing** on stable MSVC 14.51 is explicitly supported with `/std:c++23preview`; this is *not* a release-qualified build. No silent `/std:c++latest` or C++20 fallback is permitted. The project requests `/MT` for the release configuration, uses standard Windows DLLs and enables CFG/ASLR/DEP. Run from a VS 2026 x64 Developer Command Prompt:

```cmd
msbuild LockPeek.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
x64\Release\LockPeekTests.exe
powershell -ExecutionPolicy Bypass -File scripts\verify-release.ps1 -Exe x64\Release\LockPeek.exe -ReportDirectory .artifacts
```

For a **provisional MSVC 14.51 development run only**, append `/p:UsePreviewCxx23=true` to the MSBuild command and `-AllowProvisionalToolchain` to the verifier invocation. GitHub Actions selects the mode explicitly and labels C++23 preview artifacts as not release-qualified. The verifier inspects the PE imports, hardening flags, architecture and size. The binary is **unsigned**; no GitHub Release is created by CI.

## Status

**Provisional CI PASS; release qualification NOT COMPLETE.** [Windows CI #20](https://github.com/StanleyLl0yd/LockPeek/actions/runs/42992621400), on PR head `18b3273598ccb51391fea5d5ee3e72e33e99ea60` (11 November 2026), built Release x64 on hosted Windows Server 2025 and passed **47/47** unit/integration checks, static CRT/import/CFG/ASLR/DEP verification, and GUI/forged-window-message smoke. Its compiler was **MSVC 19.51.36260** with provisional `/std:c++23preview`, **not a release-qualified build**. Each uploaded binary has its own SHA-256 reported in the artifact's `build-info.txt`; hashes can differ between builds.

An additional [Stable MSVC C++23 qualification workflow](.github/workflows/stable-cxx23.yml) attempts to update the runner's Visual Studio 2026 Stable installation and fails closed unless MSVC **19.52+** builds/tests under `/std:c++23`. Its result must be checked independently; the existing green provisional CI does not satisfy this gate.

**Remaining release gates:** (1) passing exact-head stable MSVC 14.52+ build/tests and PE verification, (2) real clean Windows 10 and Windows 11 x64 client QA with evidence, (3) required checks in the active `lockpeek-main` ruleset, and (4) final exact-head repository-wide verification. `scripts/qa-client.ps1` automates only the client startup/hash/idle preflight; the manual tasks in `docs/TESTING.md` remain mandatory. No GitHub Release without explicit authorization.

## Source layout

`src/scanner.cpp`: contained native handle enumeration and subprocess scanner protocol. `src/app.cpp`: simple Win32 UI and 2-second waiting. `tests/test_main.cpp`: unit tests plus deterministic file-holder integration tests. `.github/workflows/windows.yml`: build, test and PE checks.

See [architecture](docs/ARCHITECTURE.md), [testing](docs/TESTING.md) and [known limitations](docs/LIMITATIONS.md).

No license has been selected; do not assume permission to redistribute or relicense the sources.
