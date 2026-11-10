# Known limitations

- Windows 10 and Windows 11 x64 are intended targets; no actual run or clean-VM verification has yet occurred.
- Administrator elevation may expand visibility, but protected processes remain intentionally inaccessible.
- Windows `SystemExtendedHandleInformation` is not a documented ABI. Scans fail or are explicitly partial when it cannot be used safely. No Restart Manager fallback is provided.
- The tool shows open matching file handles, **not** which sharing flags deny deletion. A found process need not be a blocker, and an empty list is not proof of freedom.
- Maximum of 128 distinct matching process IDs per scan; truncated results are clearly marked. Snapshot allocation cap: 128 MiB.
- Some driver responses to duplicated handles may block. The worker is isolated and subject to a deadline, but a kernel-mode hang may outlive process termination. A terminated worker can temporarily remain as a process object until the OS releases it.
- Access-denied or concurrently closed handles make scanning incomplete; there can be false negatives. Object identity and process liveness can race with snapshots.
- File ID matching requires the target to be safely opened with read-attributes access. When exclusive sharing prevents this, path fallback is heuristic and cannot resolve all hardlinks, symbolic links or junctions. Case-sensitive directories are particularly problematic for the fallback.
- Waiting pins the original target's file identity where available and stops if the path is replaced; without reliable target identity, waiting does not declare success.
- The selected path itself must be an ordinary file: selected symlinks/reparse points fail closed with `ERROR_NOT_SUPPORTED`. Reparse points in parent directories, network filesystems, deletion-pending files, very long paths, remote locks, AV minifilters and process isolation can still limit results.
- Drag/drop from a non-elevated Explorer to an elevated application can be blocked by Windows UIPI; normal launch is preferable.
- No background service, Explorer context integration, shell extension, scheduled monitoring, forced unlock or process termination UI.
