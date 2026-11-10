#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/scanner.h"
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
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
bool LaunchHold(const std::wstring& filename, const std::wstring& ready, const std::wstring& stop, Child& child) {
    const auto exe = Self();
    std::wstring command = lockpeek::QuoteArgument(exe) + L" --hold " +
                           lockpeek::QuoteArgument(filename) + L" " +
                           lockpeek::QuoteArgument(ready) + L" " + lockpeek::QuoteArgument(stop);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) return false;
    child.p = pi.hProcess;
    child.t = pi.hThread;
    child.pid = pi.dwProcessId;
    return true;
}
int HoldFile(const wchar_t* filename, const wchar_t* ready, const wchar_t* stop) {
    Handle file(CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
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
    wire.flags = 0x80000000;
    Check(!ValidWire(wire), "reject undocumented flags");
    Check(sizeof(WireResult) == 56 + 16 * kMaxHits, "versioned IPC carries target file identity");
    std::array<std::byte, 56> snap{};
    size_t count = 7;
    Check(!ValidateExtendedHandleBuffer(snap.data(), 15, count) && count == 0,
          "reject truncated extended-handle header");
    uintptr_t handleCount = 2;
    std::memcpy(snap.data(), &handleCount, sizeof(handleCount));
    Check(!ValidateExtendedHandleBuffer(snap.data(), snap.size(), count), "reject bad native count");
    handleCount = 1;
    std::memcpy(snap.data(), &handleCount, sizeof(handleCount));
    Check(ValidateExtendedHandleBuffer(snap.data(), snap.size(), count) && count == 1,
          "accept bounded native table");
    Check(QuoteArgument(L"a b\\") == L"\"a b\\\\\"", "escape spaces and trailing backslash");
    Check(QuoteArgument(L"a\"b") == L"\"a\\\"b\"", "escape inner quote");
    Check(NormalizePath(L"") == L"", "reject empty path");
    Check(!NormalizePath(L"C:\\Тест файл.xlsx").empty(), "accept Unicode path");
    Check(NormalizePath(L"\\\\.\\PhysicalDrive0").empty(), "reject device namespace");
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
    const auto r1 = basename + L"-ready1", r2 = basename + L"-ready2", stop = basename + L"-stop";
    Handle ready1(CreateEventW(nullptr, TRUE, FALSE, r1.c_str()));
    Handle ready2(CreateEventW(nullptr, TRUE, FALSE, r2.c_str()));
    Handle quit(CreateEventW(nullptr, TRUE, FALSE, stop.c_str()));
    if (!ready1.good() || !ready2.good() || !quit.good()) {
        Check(false, "create synchronization events"); DeleteFileW(path.c_str()); return;
    }
    {
        Child a, b;
        const bool first = LaunchHold(path, r1, stop, a) &&
                           WaitForSingleObject(ready1.get(), 10000) == WAIT_OBJECT_0;
        Check(first, "start one deterministic file holder");
        if (first) {
            const auto single = lockpeek::RunIsolatedScan(path, nullptr);
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
            Check(!result.win32Error && !result.timeout, "isolated native scan succeeded");
            bool foundA = false, foundB = false;
            for (const auto& hit : result.hits) {
                if (hit.pid == a.pid) foundA = true;
                if (hit.pid == b.pid) foundB = true;
            }
            Check(foundA && foundB, "detect both holding processes by PID");
            Check(!result.hits.empty(), "return structured matching evidence");
        }
        SetEvent(quit.get()); // Child destructors join and close process handles.
    }
    const auto cleared = lockpeek::RunIsolatedScan(path, nullptr);
    Check(!cleared.win32Error && !cleared.timeout, "recheck after holders exit");
    Check(cleared.hits.empty(), "no stale holders after processes exited");
    DeleteFileW(path.c_str());
    const auto absent = lockpeek::RunIsolatedScan(path, nullptr);
    Check(absent.win32Error == ERROR_FILE_NOT_FOUND || absent.win32Error == ERROR_PATH_NOT_FOUND,
          "missing file reports precise error");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--scan-worker")
        return lockpeek::RunScanWorker(argv[2]);
    if (argc == 5 && std::wstring(argv[1]) == L"--hold")
        return HoldFile(argv[2], argv[3], argv[4]);
    UnitTests();
    IntegrationTests();
    std::printf("TOTAL FAILURES: %d\n", failures);
    return failures ? 1 : 0;
}
