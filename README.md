# LockPeek

**Find out which Windows processes are using a file.**

LockPeek is a lightweight, portable Windows utility designed
to identify processes holding open handles to a selected file.

It helps diagnose situations where Windows reports that a file
is being used by another application.

## Project status

**Under development.**

LockPeek 1.0 has not been released yet.

## Planned features

- Find processes with open handles to a selected file.
- Show process names, PIDs and available process details.
- Detect multiple processes using the same file.
- Select files through a native Windows file dialog.
- Drag and drop files into the application.
- Accept a file path as a command-line argument.
- Repeat diagnostics on demand.
- Wait for previously detected handles to disappear.
- Report inaccessible processes and incomplete scans.
- Support Unicode and long Windows paths.
- Request administrator privileges only when needed.
- Provide a native Windows interface.

## Detection mechanism

The primary detection mechanism is Windows system handle
enumeration using NtQuerySystemInformation with
SystemExtendedHandleInformation, followed by safe handle
inspection and file identity comparison.

Windows Restart Manager is not used as the detection engine.

**Important:** An open file handle does not necessarily prevent
deletion, renaming or movement. A scan with no matching handles
does not guarantee that the file is unlocked.

## Design principles

- Native C++23 and Win32 API.
- Windows 10 and Windows 11, x64.
- One standalone executable.
- Static MSVC runtime (/MT).
- No .NET, external runtime or third-party framework.
- No installer, driver or Windows service.
- No internet connection required.
- No telemetry.
- No automatic background monitoring.
- No forced process termination.
- No forced closing of another process's handles.
- No modification or deletion of user files.
- Minimal executable size without sacrificing safety.

## Development

Preferred toolchain:

- Visual Studio 2026 Build Tools.
- Stable MSVC with C++23 support.
- Windows SDK.
- MSBuild.

The build configuration and CI checks will be added
alongside the implementation.

Release readiness requires verified Windows 10/11
compatibility, integration tests, PE dependency validation
and security checks.

## License

Licensing has not yet been decided.
No license has been granted by the repository owner.
