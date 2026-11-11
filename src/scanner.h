#pragma once
#ifndef UNICODE
#error LockPeek requires Unicode Windows APIs
#endif
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lockpeek {
constexpr unsigned kMaxHits = 128;
constexpr DWORD kScanDeadlineMs = 18000;
constexpr DWORD kScanPollMs = 200;

// Only the worker process calls ScanHandles. The UI never calls potentially
// blocking filesystem/native-object operations on its own threads.
enum class Match : uint32_t { FileId = 1, Path = 2 };
struct Hit {
    DWORD pid = 0;
    uint64_t creation = 0; // 0 means identity is not verified
    Match match = Match::Path;
    std::wstring image;
    std::wstring service; // available Windows service display name(s)
};
struct ScanResult {
    DWORD win32Error = ERROR_SUCCESS;
    uint32_t ntstatus = 0;
    bool partial = false;
    bool truncated = false;
    bool targetIdentityAvailable = false;
    uint64_t targetVolume = 0;
    std::array<uint8_t, 16> targetFileId{};
    bool timeout = false;
    bool canceled = false;
    uint32_t inaccessible = 0;
    uint32_t uninspectable = 0;
    std::vector<Hit> hits;
};

// Fixed-size, versioned IPC payload. No C++ objects or pointers cross the pipe.
constexpr uint32_t kMagic = 0x324B504C; // LPK2, little endian
struct WireHit {
    uint32_t pid;
    uint32_t match;
    uint64_t creation;
};
struct WireResult {
    uint32_t magic = kMagic;
    uint32_t size = 0;
    uint32_t win32Error = ERROR_SUCCESS;
    uint32_t ntstatus = 0;
    uint32_t flags = 0; // bit0=partial, bit1=truncated, bit2=target-ID
    uint32_t inaccessible = 0;
    uint32_t uninspectable = 0;
    uint32_t count = 0;
    uint64_t targetVolume = 0;
    std::array<uint8_t, 16> targetFileId{};
    WireHit hits[kMaxHits]{};
};
static_assert(sizeof(WireHit) == 16);
static_assert(sizeof(WireResult) == 56 + 16 * kMaxHits);

// A weak observation only: an incomplete scan cannot prove release.
bool NoMatchingHandlesObserved(const ScanResult& result) noexcept;
bool ValidWire(const WireResult& w) noexcept;
bool ValidateExtendedHandleBuffer(const void* data, size_t bytes, size_t& count) noexcept;
std::wstring NormalizePath(const std::wstring& path);
std::wstring QuoteArgument(const std::wstring& s);
ScanResult ScanHandles(const std::wstring& filename); // INTERNAL SUBPROCESS ONLY
int RunScanWorker(const std::wstring& filename);
ScanResult RunIsolatedScan(const std::wstring& file, const volatile LONG* cancellation);
void PopulateProcessNames(ScanResult& result);
} // namespace lockpeek
