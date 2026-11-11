#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/scanner.h"
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
void DescribeScan(const char* stage, const lockpeek::ScanResult& r) {
    std::printf("SCAN DIAGNOSTIC %s: win32=%lu ntstatus=0x%08X timeout=%d canceled=%d partial=%d id=%d hits=%zu inaccessible=%u uninspectable=%u\n",
                stage, static_cast<unsigned long>(r.win32Error), r.ntstatus,
                r.timeout ? 1 : 0, r.canceled ? 1 : 0, r.partial ? 1 : 0,
                r.targetIdentityAvailable ? 1 : 0, r.hits.size(),
                r.inaccessible, r.uninspectable);
    std::fflush(stdout);
}
class Handle {
    HANDLE h_ = nullptr;
public:
    explicit Handle(HANDLE h = nullptr) : h_(h) {}
    ~Handle() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return h_; }
    bool good() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
};
std::wstring Self() {
    std::wstring s(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
    if (!n || n >= s.size()) return {};
    s.resize(n);
    return s;
}
struct Child {
    HANDLE p = nullptr;
    HANDLE t = nullptr;
    DWORD pid = 0;
    ~Child() {
        if (p) { WaitForSingleObject(p, 10000); CloseHandle(p); }
        if (t) CloseHandle(t);
    }
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
};
bool LaunchHold(const std::wstring& filename, const std::wstring& ready, const std::wstring& stop,
                Child& child, bool shareDelete = false) {
    const auto exe = Self();
    std::wstring command = lockpeek::QuoteArgument(exe) + L" --hold " +
                           lockpeek::QuoteArgument(filename) + L" " +
                           lockpeek::QuoteArgument(ready) + L" " + lockpeek::QuoteArgument(stop) +
                           (shareDelete ? L" --allow-delete" : L"");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) return false;
    child.p = pi.hProcess;
    child.t = pi.hThread;
    child.pid = pi.dwProcessId;
    return true;
}
int HoldFile(const wchar_t* filename, const wchar_t* ready, const wchar_t* stop, bool shareDelete) {
    const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE |
                          (shareDelete ? FILE_SHARE_DELETE : 0u);
    Handle file(CreateFileW(filename, GENERIC_READ, sharing,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.good()) return 3;
    Handle readyEvent(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready));
    Handle stopEvent(OpenEventW(SYNCHRONIZE, FALSE, stop));
    if (!readyEvent.good() || !stopEvent.good()) return 4;
    SetEvent(readyEvent.get());
    return WaitForSingleObject(stopEvent.get(), 30000) == WAIT_OBJECT_0 ? 0 : 5;
}
void UnitTests() {
    using namespace lockpeek;
    WireResult wire{};
    wire.size = sizeof(wire);
    Check(ValidWire(wire), "empty valid IPC payload");
    wire.size = 0;
    Check(!ValidWire(wire), "reject mismatched IPC payload size");
    wire.size = sizeof(wire);
    wire.count = kMaxHits + 1;
    Check(!ValidWire(wire), "reject excessive IPC count");
    wire.count = 1;
    wire.hits[0] = {120, 5, 0};
    Check(!ValidWire(wire), "reject invalid matching method");
    wire.hits[0].match = 1;
    Check(ValidWire(wire), "accept valid evidence");
    wire.hits[0].pid = 0;
    Check(!ValidWire(wire), "reject impossible zero PID");
    wire.hits[0].pid = 120;
    wire.hits[0].match = 2;
    Check(ValidWire(wire), "accept heuristic path evidence");
    wire.flags = 0x80000000;
    Check(!ValidWire(wire), "reject undocumented flags");
    Check(sizeof(WireResult) == 56 + 16 * kMaxHits, "versioned IPC carries target file identity");
    std::array<std::byte, 56> snap{};
    size_t count = 7;
    Check(!ValidateExtendedHandleBuffer(snap.data(), 15, count) && count == 0,
          "reject truncated extended-handle header");
    Check(!ValidateExtendedHandleBuffer(nullptr, 56, count) && count == 0,
          "reject null extended-handle data");
    uintptr_t handleCount = 2;
    std::memcpy(snap.data(), &handleCount, sizeof(handleCount));
    Check(!ValidateExtendedHandleBuffer(snap.data(), snap.size(), count), "reject bad native count");
    handleCount = 1;
    std::memcpy(snap.data(), &handleCount, sizeof(handleCount));
    Check(ValidateExtendedHandleBuffer(snap.data(), snap.size(), count) && count == 1,
          "accept bounded native table");
    Check(QuoteArgument(L"a b\\") == L"\"a b\\\\\"", "escape spaces and trailing backslash");
    Check(QuoteArgument(L"a\"b") == L"\"a\\\"b\"", "escape inner quote");
    Check(QuoteArgument(L"документ\\") == L"\"документ\\\\\"", "escape Cyrillic argument and trailing slash");
    Check(NormalizePath(L"") == L"", "reject empty path");
    Check(!NormalizePath(L"C:\\Тест файл.xlsx").empty(), "accept Unicode path");
    Check(NormalizePath(L"\\\\.\\PhysicalDrive0").empty(), "reject device namespace");
    Check(NormalizePath(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\x").empty(),
          "reject extended GLOBALROOT device namespace");
    Check(NormalizePath(L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\x").empty(),
          "reject extended volume namespace rather than misresolve it");
    ScanResult observed{};
    Check(!NoMatchingHandlesObserved(observed), "waiting does not infer release without file identity");
    observed.targetIdentityAvailable = true;
    Check(NoMatchingHandlesObserved(observed), "waiting can notice absence of observed holders");
    observed.partial = true;
    Check(NoMatchingHandlesObserved(observed), "waiting may report qualified absence with partial scan");
    observed.hits.push_back({321, 0, Match::Path, {}});
    Check(!NoMatchingHandlesObserved(observed), "waiting does not report absence with a matching holder");
    observed.hits.clear();
    observed.timeout = true;
    Check(!NoMatchingHandlesObserved(observed), "waiting ignores timed out scan");
    observed.timeout = false;
    observed.canceled = true;
    Check(!NoMatchingHandlesObserved(observed), "waiting ignores canceled scan");
    observed.canceled = false;
    observed.truncated = true;
    Check(!NoMatchingHandlesObserved(observed), "waiting ignores truncated scan");
    observed.truncated = false;
    observed.win32Error = ERROR_ACCESS_DENIED;
    Check(!NoMatchingHandlesObserved(observed), "waiting ignores error scans");
    ScanResult invalidMetadata{};
    invalidMetadata.hits.push_back({0, 0, Match::Path, {}});
    PopulateProcessNames(invalidMetadata);
    Check(invalidMetadata.partial && invalidMetadata.hits.empty(),
          "uninspectable process identity is marked incomplete and invalid PID removed");
}
void IntegrationTests() {
    wchar_t directory[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, directory)) { Check(false, "get temporary directory"); return; }
    wchar_t temporary[MAX_PATH + 1]{};
    if (!GetTempFileNameW(directory, L"lpk", 0, temporary)) { Check(false, "create temp file"); return; }
    const std::wstring unicodePath = std::wstring(temporary) + L"-тест (два процесса).txt";
    const bool renamed = MoveFileW(temporary, unicodePath.c_str()) != FALSE;
    Check(renamed, "create Unicode temp file path");
    const std::wstring path = renamed ? unicodePath : temporary;
    // Test data is created by this test, never an existing user document.
    const auto basename = std::wstring(L"Local\\LockPeek-") + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(GetTickCount64());
    const auto r1 = basename + L"-ready1", r2 = basename + L"-ready2",
               r3 = basename + L"-ready3", stop = basename + L"-stop";
    const std::wstring alias = path + L"-hardlink";
    const bool hasHardlink = CreateHardLinkW(alias.c_str(), path.c_str(), nullptr) != FALSE;
    if (!hasHardlink) std::printf("SKIP: hard link identity test (not supported on this volume)\n");
    Handle ready1(CreateEventW(nullptr, TRUE, FALSE, r1.c_str()));
    Handle ready2(CreateEventW(nullptr, TRUE, FALSE, r2.c_str()));
    Handle ready3(CreateEventW(nullptr, TRUE, FALSE, r3.c_str()));
    Handle quit(CreateEventW(nullptr, TRUE, FALSE, stop.c_str()));
    if (!ready1.good() || !ready2.good() || !ready3.good() || !quit.good()) {
        Check(false, "create synchronization events");
        if (hasHardlink) DeleteFileW(alias.c_str());
        DeleteFileW(path.c_str());
        return;
    }
    {
        Child a, b;
        const bool first = LaunchHold(path, r1, stop, a) &&
                           WaitForSingleObject(ready1.get(), 10000) == WAIT_OBJECT_0;
        Check(first, "start one deterministic file holder");
        if (first) {
            const auto single = lockpeek::RunIsolatedScan(path, nullptr);
            DescribeScan("single", single);
            bool found = false;
            for (const auto& hit : single.hits) if (hit.pid == a.pid) found = true;
            Check(!single.win32Error && !single.timeout && found, "detect one file holder");
            Check(single.targetIdentityAvailable, "test target identity captured across IPC");
        }
        const bool second = LaunchHold(path, r2, stop, b) &&
                            WaitForSingleObject(ready2.get(), 10000) == WAIT_OBJECT_0;
        const bool both = first && second;
        Check(both, "start two deterministic file holders");
        if (both) {
            const auto result = lockpeek::RunIsolatedScan(path, nullptr);
            DescribeScan("dual", result);
            Check(!result.win32Error && !result.timeout, "isolated native scan succeeded");
            bool foundA = false, foundB = false;
            for (const auto& hit : result.hits) {
                if (hit.pid == a.pid) foundA = true;
                if (hit.pid == b.pid) foundB = true;
            }
            Check(foundA && foundB, "detect both holding processes by PID");
            Check(!result.hits.empty(), "return structured matching evidence");
            if (hasHardlink) {
                const auto alternate = lockpeek::RunIsolatedScan(alias, nullptr);
                DescribeScan("hardlink", alternate);
                bool aliasA = false, aliasB = false;
                for (const auto& hit : alternate.hits) {
                    if (hit.pid == a.pid && hit.match == lockpeek::Match::FileId) aliasA = true;
                    if (hit.pid == b.pid && hit.match == lockpeek::Match::FileId) aliasB = true;
                }
                Check(!alternate.win32Error && !alternate.timeout && aliasA && aliasB,
                      "recognize holders through hard link by volume/file ID");
            }
        }
        SetEvent(quit.get()); // Child destructors join and close process handles.
    }
    const auto cleared = lockpeek::RunIsolatedScan(path, nullptr);
    DescribeScan("cleared", cleared);
    Check(!cleared.win32Error && !cleared.timeout, "recheck after holders exit");
    Check(cleared.hits.empty(), "no stale holders after processes exited");
    ResetEvent(quit.get());
    {
        Child permissive;
        const bool launched = LaunchHold(path, r3, stop, permissive, true) &&
                              WaitForSingleObject(ready3.get(), 10000) == WAIT_OBJECT_0;
        Check(launched, "start holder that explicitly allows delete sharing");
        if (launched) {
            const auto scan = lockpeek::RunIsolatedScan(path, nullptr);
            DescribeScan("sharing", scan);
            bool found = false;
            for (const auto& hit : scan.hits) if (hit.pid == permissive.pid) found = true;
            Check(!scan.win32Error && !scan.timeout && found,
                  "report open handles even when deletion sharing is allowed");
        }
        SetEvent(quit.get());
    }
    if (hasHardlink) Check(DeleteFileW(alias.c_str()) != FALSE, "clean up test hard link");
    Check(DeleteFileW(path.c_str()) != FALSE, "clean up temporary test file");
    const auto absent = lockpeek::RunIsolatedScan(path, nullptr);
    Check(absent.win32Error == ERROR_FILE_NOT_FOUND || absent.win32Error == ERROR_PATH_NOT_FOUND,
          "missing file reports precise error");
    volatile LONG alreadyCanceled = 1;
    const auto canceled = lockpeek::RunIsolatedScan(path, &alreadyCanceled);
    Check(canceled.canceled && canceled.partial && canceled.hits.empty(),
          "canceled scan does not start or publish stale results");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--gui-message-smoke") {
        wchar_t* end = nullptr;
        const unsigned long long raw = std::wcstoull(argv[2], &end, 10);
        if (!raw || !end || *end != L'\0') return 6;
        HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(raw));
        if (!IsWindow(window)) return 7;
        // Invalid pointer payloads must not be read, deleted, or trusted.
        return PostMessageW(window, WM_APP + 7, 0, 1) &&
               PostMessageW(window, WM_APP + 7, 42, 2) ? 0 : 8;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--scan-worker")
        return lockpeek::RunScanWorker(argv[2]);
    if ((argc == 5 || argc == 6) && std::wstring(argv[1]) == L"--hold") {
        if (argc == 6 && std::wstring(argv[5]) != L"--allow-delete") return 2;
        return HoldFile(argv[2], argv[3], argv[4], argc == 6);
    }
    UnitTests();
    IntegrationTests();
    std::printf("TOTAL FAILURES: %d\n", failures);
    return failures ? 1 : 0;
}
