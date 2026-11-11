# Architecture and safety boundaries

1. The GUI process never enumerates handles directly. A separate invocation of the same EXE with `--scan-worker` reports a fixed-size, versioned binary record over a one-way inherited anonymous pipe. The parent only inherits the worker's file-identity/PID/match records after the child has exited. A handle inheritance allowlist restricts what is passed to the child. No user data is written to persistent logs or temp files.
2. The worker uses only query/duplicate operations. It opens a known ordinary file in its own process to learn the **boot-specific** File ObjectTypeIndex from the current system handle snapshot. It filters candidates by that index before calling `DuplicateHandle`, then excludes non-disk handles (pipes/devices) using `GetFileType` before file-identity/path queries. It does not use `DUPLICATE_CLOSE_SOURCE` or write to any selected file.
3. `NtQuerySystemInformation(64)` is an undocumented/unstable API. It is dynamically resolved from Windows `ntdll.dll`; its structures are kept in one component, architecture assumptions are asserted, lengths are validated before dereferencing, memory capped at 128 MiB, and retries bounded. Unexpected statuses are returned to the user rather than triggering a silent Restart Manager fallback.
4. A target is opened read-attributes only, with read/write/delete sharing when allowed. Matched volume + FILE_ID_INFO is stronger evidence than an apparent pathname match. If identity cannot be obtained (exclusive sharing, unsupported volume), final paths are compared cautiously, and each such hit is labelled `Path (heuristic)`.
5. Process handles are opened with only `PROCESS_DUP_HANDLE` and, separately, `PROCESS_QUERY_LIMITED_INFORMATION`. Keeping handles open reduces PID-reuse risk. A second snapshot checks `(PID, handle value, kernel object pointer, type index)` evidence, but races are still possible. Creation times are retained where available; unexpected PID reuse is explicitly marked as an uncertain result.
6. The worker has an internal 14.5-second scanning budget checked between potentially blocking API calls. The parent uses an 18-second watchdog and can terminate **only the child worker it created**. Even kernel-mode hangs may delay worker termination; the GUI does not wait indefinitely for its process. Job object `KILL_ON_JOB_CLOSE` is applied when supported.
7. The UI owns the selection, scan generation, one active worker thread and optional 2-second wait timer. Selecting a new file cancels or supersedes older work. Waiting pins the selected file's volume + file ID and stops without claiming release if the path is replaced. Incomplete/error/zero-result states never imply deletion is safe.

## Security notes

- UI defaults to `asInvoker`; elevation is explicit `ShellExecuteExW` with `runas` and Windows-correct argument quoting.
- No TCP/HTTP, shell extension, driver, service, installation, registry persistence or external runtime.
- All scan activity stops when the application closes. The worker subprocess is not an independent daemon.
- `TerminateProcess` is limited to a worker process created by LockPeek. It is **never** invoked on a PID discovered by scanning.

## Limitations of confidence

Finding a FILE_ID_INFO match means the process had an open handle to the same file identity at inspection time; it says nothing about that handle's `FILE_SHARE_DELETE` or whether an operation will succeed. A stale or unavailable handle, symlink, reparse point, case-sensitive directory, filesystem filter or remote access may lead to false negatives or heuristic false positives. Treat output as diagnostic evidence, not a security/locking oracle.
