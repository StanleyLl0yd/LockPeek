#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "scanner.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <winsvc.h>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lockpeek {
namespace {
class Handle {
    HANDLE h_ = nullptr;
public:
    explicit Handle(HANDLE h = nullptr) noexcept : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept { if (this != &o) reset(o.release()); return *this; }
    HANDLE get() const noexcept { return h_; }
    bool valid() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { auto h = h_; h_ = nullptr; return h; }
    void reset(HANDLE next = nullptr) noexcept { if (valid()) CloseHandle(h_); h_ = next; }
};

class ServiceHandle {
    SC_HANDLE h_ = nullptr;
public:
    explicit ServiceHandle(SC_HANDLE h) noexcept : h_(h) {}
    ~ServiceHandle() { if (h_) CloseServiceHandle(h_); }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
};

// These layouts are intentionally private: SystemExtendedHandleInformation is
// not a documented stable Win32 contract. Never dereference Object.
struct NtHandle {
    void* object;
    ULONG_PTR pid;
    ULONG_PTR handle;
    ULONG access;
    USHORT creatorBackTraceIndex;
    USHORT typeIndex;
    ULONG attributes;
    ULONG reserved;
};
struct NtHandleHeader { ULONG_PTR count; ULONG_PTR reserved; };
static_assert(sizeof(NtHandle) == 40 && sizeof(NtHandleHeader) == 16,
              "This scanner supports 64-bit Windows only");
using QuerySystemInfo = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtStatusToDos = ULONG(NTAPI*)(LONG);
constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr LONG kLengthMismatch = static_cast<LONG>(0xC0000004u);
constexpr LONG kBufferTooSmall = static_cast<LONG>(0xC0000023u);
constexpr size_t kMaxSnapshot = 128u * 1024u * 1024u;
constexpr ULONGLONG kInternalDeadlineMs = 14500;

std::wstring ModuleName() {
    std::wstring s(32768, L'\0');
    DWORD count = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
    if (count == 0 || count >= s.size()) return {};
    s.resize(count);
    return s;
}

DWORD ToWin32(LONG status) {
    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* f = ntdll ? reinterpret_cast<NtStatusToDos>(GetProcAddress(ntdll, "RtlNtStatusToDosError")) : nullptr;
    return f ? f(status) : ERROR_INVALID_FUNCTION;
}

struct Snapshot {
    std::vector<std::byte> storage;
    const NtHandle* handles = nullptr;
    size_t count = 0;
    LONG status = 0;
};

bool TakeSnapshot(Snapshot& snapshot) {
    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* query = ntdll ? reinterpret_cast<QuerySystemInfo>(GetProcAddress(ntdll, "NtQuerySystemInformation")) : nullptr;
    if (!query) { snapshot.status = static_cast<LONG>(0xC0000002u); return false; }
    size_t capacity = 1u << 20;
    for (int retry = 0; retry < 12 && capacity <= kMaxSnapshot; ++retry) {
        snapshot.storage.resize(capacity);
        ULONG wanted = 0;
        snapshot.status = query(kSystemExtendedHandleInformation, snapshot.storage.data(),
                                static_cast<ULONG>(capacity), &wanted);
        if (snapshot.status == 0) {
            size_t count = 0;
            if (!ValidateExtendedHandleBuffer(snapshot.storage.data(), capacity, count)) {
                snapshot.status = static_cast<LONG>(0xC000000Du);
                return false;
            }
            snapshot.count = count;
            snapshot.handles = reinterpret_cast<const NtHandle*>(snapshot.storage.data() + sizeof(NtHandleHeader));
            return true;
        }
        // Treat even nonzero warning statuses as unsupported: do not parse a
        // potentially partial native table as a complete successful result.
        if (snapshot.status != kLengthMismatch && snapshot.status != kBufferTooSmall) return false;
        size_t required = wanted > capacity ? static_cast<size_t>(wanted) : capacity + 1;
        if (required > kMaxSnapshot) return false;
        // A changing system table may keep increasing: leave room for growth.
        capacity = std::max(capacity * 2, required + required / 4);
    }
    return false;
}

std::wstring FinalPath(HANDLE h) {
    // Called ONLY inside the disposable worker process. Filesystem filter
    // drivers can stall this call indefinitely; the parent owns a watchdog.
    DWORD size = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!size || size > 32767) return {};
    std::wstring value(size + 1, L'\0');
    DWORD read = GetFinalPathNameByHandleW(h, value.data(), static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED);
    if (!read || read >= value.size()) return {};
    value.resize(read);
    return value;
}

bool FileIdentity(HANDLE h, FILE_ID_INFO& info) {
    return GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info)) != FALSE;
}

bool SameIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
           std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    // Windows also permits case-sensitive directories; path-only matches are
    // therefore always explicitly marked as heuristic, not definitive.
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

uint64_t CreationTime(HANDLE process) {
    FILETIME c{}, x{}, k{}, u{};
    if (!GetProcessTimes(process, &c, &x, &k, &u)) return 0;
    ULARGE_INTEGER t{};
    t.LowPart = c.dwLowDateTime;
    t.HighPart = c.dwHighDateTime;
    return t.QuadPart;
}

struct Process {
    Handle duplicator;
    Handle identity;
    uint64_t creation = 0;
};
struct Evidence {
    ULONG_PTR pid = 0;
    ULONG_PTR handle = 0;
    void* object = nullptr;
    USHORT type = 0;
    Match match = Match::Path;
    uint64_t creation = 0;
};
struct EvidenceKey {
    ULONG_PTR pid;
    ULONG_PTR handle;
    void* object;
    USHORT type;
    bool operator==(const EvidenceKey&) const = default;
};
struct EvidenceHasher {
    size_t operator()(const EvidenceKey& k) const noexcept {
        size_t h = std::hash<ULONG_PTR>{}(k.pid);
        h ^= std::hash<ULONG_PTR>{}(k.handle) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<void*>{}(k.object) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h ^ k.type;
    }
};

bool ReadAll(HANDLE pipe, void* data, DWORD bytes) {
    auto* at = static_cast<std::byte*>(data);
    DWORD total = 0;
    while (total < bytes) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, at + total, bytes - total, &chunk, nullptr) || !chunk) return false;
        total += chunk;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* data, DWORD bytes) {
    const auto* at = static_cast<const std::byte*>(data);
    DWORD total = 0;
    while (total < bytes) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, at + total, bytes - total, &chunk, nullptr) || !chunk) return false;
        total += chunk;
    }
    return true;
}
} // namespace

bool ValidateExtendedHandleBuffer(const void* data, size_t bytes, size_t& count) noexcept {
    count = 0;
    if (!data || bytes < sizeof(NtHandleHeader)) return false;
    NtHandleHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    const size_t available = (bytes - sizeof(NtHandleHeader)) / sizeof(NtHandle);
    if (hdr.count > available) return false;
    count = static_cast<size_t>(hdr.count);
    return true;
}

bool ValidWire(const WireResult& w) noexcept {
    if (w.magic != kMagic || w.size != sizeof(WireResult) || w.count > kMaxHits) return false;
    if (w.flags & ~7u) return false;
    for (uint32_t i = 0; i < w.count; ++i) {
        if (!w.hits[i].pid || (w.hits[i].match != 1 && w.hits[i].match != 2)) return false;
    }
    return true;
}

std::wstring NormalizePath(const std::wstring& input) {
    if (input.empty() || input.size() >= 32767) return {};
    std::wstring path = input;
    // Do not feed NT namespace paths through GetFullPathNameW as DOS paths.
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
    if (path.starts_with(L"\\??\\") || path.starts_with(L"\\\\.\\")) return {};
    std::wstring absolute(32768, L'\0');
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(absolute.size()), absolute.data(), nullptr);
    if (!n || n >= absolute.size()) return {};
    absolute.resize(n);
    for (auto& ch : absolute) if (ch == L'/') ch = L'\\';
    return absolute;
}

std::wstring QuoteArgument(const std::wstring& in) {
    // Windows CommandLineToArgvW quoting, including trailing backslashes.
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : in) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'\"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(c);
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

ScanResult ScanHandles(const std::wstring& file) {
    ScanResult result{};
    const std::wstring targetPath = NormalizePath(file);
    if (targetPath.empty()) { result.win32Error = ERROR_INVALID_NAME; return result; }
    // Extended-length Win32 paths preserve Unicode names past MAX_PATH.
    std::wstring win32Target;
    if (targetPath.starts_with(L"\\\\")) win32Target = L"\\\\?\\UNC\\" + targetPath.substr(2);
    else win32Target = L"\\\\?\\" + targetPath;
    DWORD attributes = GetFileAttributesW(win32Target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME) {
            result.win32Error = error;
            return result;
        }
        result.partial = true;
    } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        result.win32Error = ERROR_DIRECTORY;
        return result;
    } else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        // Do not silently switch the inspected object to a symlink target.
        result.win32Error = ERROR_NOT_SUPPORTED;
        return result;
    }

    Handle target(CreateFileW(win32Target.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    FILE_ID_INFO targetId{};
    const bool hasId = target.valid() && FileIdentity(target.get(), targetId);
    result.targetIdentityAvailable = hasId;
    if (hasId) {
        result.targetVolume = targetId.VolumeSerialNumber;
        static_assert(sizeof(targetId.FileId.Identifier) == 16);
        std::memcpy(result.targetFileId.data(), targetId.FileId.Identifier, 16);
    }
    if (!hasId) result.partial = true;
    std::wstring physicalTarget = target.valid() ? NormalizePath(FinalPath(target.get())) : std::wstring{};
    if (physicalTarget.empty()) physicalTarget = targetPath;

    // Determine the current boot's FILE ObjectTypeIndex from a known file
    // handle in our own process. Never hardcode an index from another boot.
    Handle exemplar(CreateFileW(ModuleName().c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!exemplar.valid()) { result.win32Error = GetLastError(); return result; }
    Snapshot snapshot;
    if (!TakeSnapshot(snapshot)) {
        result.ntstatus = static_cast<uint32_t>(snapshot.status);
        result.win32Error = ToWin32(snapshot.status);
        return result;
    }
    USHORT fileType = 0;
    const auto pid = static_cast<ULONG_PTR>(GetCurrentProcessId());
    const auto refHandle = reinterpret_cast<ULONG_PTR>(exemplar.get());
    for (size_t i = 0; i < snapshot.count; ++i) {
        const auto& h = snapshot.handles[i];
        if (h.pid == pid && h.handle == refHandle) { fileType = h.typeIndex; break; }
    }
    if (!fileType) { result.win32Error = ERROR_NOT_SUPPORTED; return result; }

    std::unordered_map<ULONG_PTR, Process> processes;
    std::vector<Evidence> possible;
    std::unordered_set<ULONG_PTR> inaccessiblePids;
    std::unordered_map<void*, Match> positiveObjectMatches;
    const ULONGLONG started = GetTickCount64();
    for (size_t i = 0; i < snapshot.count; ++i) {
        if (GetTickCount64() - started >= kInternalDeadlineMs) { result.partial = true; result.timeout = true; break; }
        const auto& h = snapshot.handles[i];
        if (h.typeIndex != fileType || h.pid == pid || h.pid == 0 || h.pid > UINT32_MAX) continue;
        auto [it, inserted] = processes.try_emplace(h.pid);
        if (inserted) {
            it->second.duplicator.reset(OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(h.pid)));
            it->second.identity.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(h.pid)));
            if (it->second.identity.valid()) it->second.creation = CreationTime(it->second.identity.get());
            if (!it->second.duplicator.valid()) {
                inaccessiblePids.insert(h.pid);
                result.partial = true;
            }
        }
        if (!it->second.duplicator.valid()) continue;
        // Several process handles can refer to the same underlying FILE_OBJECT.
        // Cache only confirmed positive matches; failures are never cached
        // because another handle may have more query-capable access rights.
        if (h.object) {
            const auto known = positiveObjectMatches.find(h.object);
            if (known != positiveObjectMatches.end()) {
                possible.push_back({h.pid, h.handle, h.object, h.typeIndex, known->second, it->second.creation});
                continue;
            }
        }
        HANDLE raw = nullptr;
        if (!DuplicateHandle(it->second.duplicator.get(), reinterpret_cast<HANDLE>(h.handle),
                             GetCurrentProcess(), &raw, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            ++result.uninspectable;
            result.partial = true;
            continue;
        }
        Handle duplicated(raw);
        Match match = Match::Path;
        bool matched = false;
        FILE_ID_INFO candidate{};
        if (hasId && FileIdentity(duplicated.get(), candidate)) {
            matched = SameIdentity(targetId, candidate);
            match = Match::FileId;
        } else {
            const std::wstring actual = NormalizePath(FinalPath(duplicated.get()));
            if (!actual.empty()) matched = SamePath(actual, physicalTarget) || SamePath(actual, targetPath);
            else { ++result.uninspectable; result.partial = true; }
            match = Match::Path;
        }
        if (matched) {
            if (!h.object) result.partial = true; // kernel pointers may be hidden
            else positiveObjectMatches[h.object] = match;
            possible.push_back({h.pid, h.handle, h.object, h.typeIndex, match, it->second.creation});
        }
    }
    result.inaccessible = static_cast<uint32_t>(inaccessiblePids.size());
    // Re-check handle + kernel object identity against a fresh snapshot to
    // reject stale snapshot entries and reused handle values. A pinned process
    // handle additionally prevents its PID being silently recycled.
    Snapshot confirm;
    if (!possible.empty()) {
        if (TakeSnapshot(confirm)) {
            std::unordered_set<EvidenceKey, EvidenceHasher> fresh;
            for (size_t i = 0; i < confirm.count; ++i) {
                const auto& h = confirm.handles[i];
                if (h.typeIndex == fileType) fresh.insert({h.pid, h.handle, h.object, h.typeIndex});
            }
            std::erase_if(possible, [&fresh](const Evidence& e) {
                return !fresh.contains({e.pid, e.handle, e.object, e.type});
            });
        } else {
            result.partial = true;
            result.ntstatus = static_cast<uint32_t>(confirm.status);
        }
    }
    // Deduplicate multiple handles owned by one process. Prefer file ID
    // evidence over path heuristics and preserve the process creation time.
    for (const auto& e : possible) {
        if (e.pid > UINT32_MAX) continue;
        const auto p = static_cast<DWORD>(e.pid);
        auto it = std::find_if(result.hits.begin(), result.hits.end(),
                               [p, &e](const Hit& h) { return h.pid == p && h.creation == e.creation; });
        if (it != result.hits.end()) {
            if (e.match == Match::FileId) it->match = Match::FileId;
        } else if (result.hits.size() < kMaxHits) {
            result.hits.push_back({p, e.creation, e.match, {}});
        } else {
            result.truncated = true;
            result.partial = true;
        }
    }
    return result;
}

int RunScanWorker(const std::wstring& filename) {
    ScanResult result{};
    try {
        result = ScanHandles(filename);
    } catch (const std::bad_alloc&) {
        result.win32Error = ERROR_NOT_ENOUGH_MEMORY;
        result.partial = true;
    } catch (...) {
        result.win32Error = ERROR_GEN_FAILURE;
        result.partial = true;
    }
    WireResult out{};
    out.size = sizeof(out);
    out.win32Error = result.win32Error;
    out.ntstatus = result.ntstatus;
    out.flags = (result.partial ? 1u : 0u) | (result.truncated ? 2u : 0u) |
                (result.targetIdentityAvailable ? 4u : 0u);
    out.inaccessible = result.inaccessible;
    out.uninspectable = result.uninspectable;
    out.count = static_cast<uint32_t>(std::min<size_t>(result.hits.size(), kMaxHits));
    out.targetVolume = result.targetVolume;
    out.targetFileId = result.targetFileId;
    for (uint32_t i = 0; i < out.count; ++i) {
        out.hits[i] = {result.hits[i].pid, static_cast<uint32_t>(result.hits[i].match), result.hits[i].creation};
    }
    auto stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stdoutHandle || stdoutHandle == INVALID_HANDLE_VALUE) return 3;
    return WriteAll(stdoutHandle, &out, sizeof(out)) ? 0 : 4;
}

void PopulateProcessNames(ScanResult& result) {
    for (auto& hit : result.hits) {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, hit.pid));
        if (!process.valid()) continue;
        const uint64_t now = CreationTime(process.get());
        if (hit.creation && now && hit.creation != now) { // PID reused between scan and display
            hit.pid = 0; // drop evidence rather than attributing it to the new process
            result.partial = true;
            continue;
        }
        if (!hit.creation || !now) {
            // PID alone cannot safely identify a process across the worker
            // boundary. Keep scan evidence but avoid attributing metadata.
            result.partial = true;
            continue;
        }
        std::wstring buf(32768, L'\0');
        DWORD size = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(process.get(), 0, buf.data(), &size)) {
            buf.resize(size);
            hit.image = std::move(buf);
        }
    }
    std::erase_if(result.hits, [](const Hit& hit) { return hit.pid == 0; });
    if (result.hits.empty()) return;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return; // metadata is optional, not a scan error
    ServiceHandle closeScm(scm);
    DWORD bytes = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0, &bytes, &count, &resume, nullptr);
    if (GetLastError() == ERROR_MORE_DATA && bytes && bytes <= 4u * 1024u * 1024u) {
        std::vector<std::byte> storage(bytes);
        count = 0;
        resume = 0;
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                                  SERVICE_STATE_ALL, reinterpret_cast<LPBYTE>(storage.data()),
                                  bytes, &bytes, &count, &resume, nullptr)) {
            auto* entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(storage.data());
            for (DWORD i = 0; i < count; ++i) {
                const DWORD pid = entries[i].ServiceStatusProcess.dwProcessId;
                if (pid == 0) continue;
                for (auto& hit : result.hits) {
                    if (hit.pid == pid && !hit.image.empty() && entries[i].lpDisplayName) {
                        if (!hit.service.empty()) hit.service += L", ";
                        hit.service += entries[i].lpDisplayName;
                    }
                }
            }
        }
    }
}

ScanResult RunIsolatedScan(const std::wstring& filename, const volatile LONG* cancel) {
    ScanResult result{};
    const std::wstring exe = ModuleName();
    if (exe.empty()) { result.win32Error = ERROR_BAD_PATHNAME; return result; }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rawRead = nullptr, rawWrite = nullptr;
    if (!CreatePipe(&rawRead, &rawWrite, &sa, 8192)) { result.win32Error = GetLastError(); return result; }
    Handle read(rawRead), write(rawWrite);
    if (!SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0)) {
        result.win32Error = GetLastError(); return result;
    }
    // Standard input/error must also be valid inherited handles when
    // STARTF_USESTDHANDLES is set. Route both to the system NUL device;
    // only stdout carries the bounded binary reply.
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle nullError(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullInput.valid() || !nullError.valid()) {
        result.win32Error = GetLastError(); return result;
    }
    // Only these three standard stream handles can reach the worker.
    SIZE_T attrsSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrsSize);
    std::vector<std::byte> attrStorage(attrsSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrsSize)) {
        result.win32Error = GetLastError(); return result;
    }
    struct AttrGuard { LPPROC_THREAD_ATTRIBUTE_LIST p; ~AttrGuard() { DeleteProcThreadAttributeList(p); } } attrsGuard{attrs};
    HANDLE inherited[] = {write.get(), nullInput.get(), nullError.get()};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr)) {
        result.win32Error = GetLastError(); return result;
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = write.get();
    si.StartupInfo.hStdError = nullError.get();
    si.StartupInfo.hStdInput = nullInput.get();
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring command = QuoteArgument(exe) + L" --scan-worker " + QuoteArgument(filename);
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &si.StartupInfo, &pi)) {
        result.win32Error = GetLastError(); return result;
    }
    Handle process(pi.hProcess), thread(pi.hThread);
    write.reset(); // End-of-file when child closes its one output handle.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (job.valid()) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
            if (!AssignProcessToJobObject(job.get(), process.get())) {
                // Already assigned by a parent job on some Windows runners.
                // The explicit watchdog below still applies.
            }
        }
    }
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        const DWORD wait = WaitForSingleObject(process.get(), kScanPollMs);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) { result.win32Error = GetLastError(); break; }
        if ((cancel && InterlockedCompareExchange(const_cast<volatile LONG*>(cancel), 0, 0)) ||
            GetTickCount64() - started >= kScanDeadlineMs) {
            result.canceled = cancel && InterlockedCompareExchange(const_cast<volatile LONG*>(cancel), 0, 0);
            result.timeout = !result.canceled;
            result.partial = true;
            // ONLY our own diagnostic worker is terminated, never any process
            // found in the user's filesystem scan.
            TerminateProcess(process.get(), ERROR_TIMEOUT);
            break;
        }
    }
    // A worker stuck in a kernel driver may resist termination. Do not make
    // the GUI wait indefinitely for the process or pipe in that case.
    if (WaitForSingleObject(process.get(), 1000) != WAIT_OBJECT_0) {
        result.timeout = true; result.partial = true; result.win32Error = ERROR_TIMEOUT;
        return result;
    }
    if (result.canceled || result.timeout || result.win32Error) return result;
    DWORD code = 0;
    if (!GetExitCodeProcess(process.get(), &code) || code != 0) {
        result.win32Error = ERROR_GEN_FAILURE; result.partial = true; return result;
    }
    WireResult wire{};
    if (!ReadAll(read.get(), &wire, sizeof(wire)) || !ValidWire(wire)) {
        result.win32Error = ERROR_INVALID_DATA; result.partial = true; return result;
    }
    result.win32Error = wire.win32Error;
    result.ntstatus = wire.ntstatus;
    result.partial = (wire.flags & 1) != 0;
    result.truncated = (wire.flags & 2) != 0;
    result.targetIdentityAvailable = (wire.flags & 4) != 0;
    result.targetVolume = wire.targetVolume;
    result.targetFileId = wire.targetFileId;
    result.inaccessible = wire.inaccessible;
    result.uninspectable = wire.uninspectable;
    for (uint32_t i = 0; i < wire.count; ++i) {
        const auto& h = wire.hits[i];
        result.hits.push_back({h.pid, h.creation, static_cast<Match>(h.match), {}});
    }
    try {
        PopulateProcessNames(result);
    } catch (const std::bad_alloc&) {
        result.partial = true; // optional metadata may be incomplete
    }
    return result;
}
} // namespace lockpeek
