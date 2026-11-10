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

Requires **Visual Studio 2026 / MSVC v14.52 or newer, confirmed stable**, the Windows SDK, and `/std:c++23`. MSVC 14.52 was only publicly confirmed as a Release Candidate on 10 November 2026. The project and CI **fail closed** with older compilers; no silent `/std:c++latest` fallback. The project requests `/MT` for the release configuration, uses standard Windows DLLs and enables CFG/ASLR/DEP. Run from a VS 2026 x64 Developer Command Prompt:

```cmd
msbuild LockPeek.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
x64\Release\LockPeekTests.exe
powershell -ExecutionPolicy Bypass -File scripts\verify-release.ps1 -Exe x64\Release\LockPeek.exe -ReportDirectory .artifacts
```

The verifier inspects the PE imports, hardening flags, architecture and size. The binary is **unsigned**; no GitHub Release is created by CI.

## Status

**Source implementation / initial review, not yet a verified Release Candidate.** In the preparation environment, no Windows/MSVC build or clean Windows VMs are available. All Windows runtime tests, exact EXE size, and PE dependencies are **NOT VERIFIED** until the workflow runs successfully with a stable compiler and clean-machine tests are performed. Do not distribute as a verified 1.0 build yet.

## Source layout

`src/scanner.cpp`: contained native handle enumeration and subprocess scanner protocol. `src/app.cpp`: simple Win32 UI and 2-second waiting. `tests/test_main.cpp`: unit tests plus deterministic file-holder integration tests. `.github/workflows/windows.yml`: build, test and PE checks.

See [architecture](docs/ARCHITECTURE.md), [testing](docs/TESTING.md) and [known limitations](docs/LIMITATIONS.md).

No license has been selected; do not assume permission to redistribute or relicense the sources.
