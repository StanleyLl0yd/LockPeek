#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app.h"
#include "scanner.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace lockpeek {
namespace {
constexpr UINT kResultMessage = WM_APP + 7;
constexpr UINT_PTR kWaitTimer = 1;
constexpr int kChoose = 101;
constexpr int kRetry = 102;
constexpr int kWait = 103;
constexpr int kCopy = 104;
constexpr int kElevate = 105;
constexpr int kStatusSummary = 106;
constexpr int kStatusDetails = 107;
constexpr int kProcessList = 108;

class App {
    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND file_ = nullptr, list_ = nullptr, status_ = nullptr, hint_ = nullptr;
    HWND title_ = nullptr, fileLabel_ = nullptr, section_ = nullptr;
    HWND statusPanel_ = nullptr, statusDetails_ = nullptr;
    HWND choose_ = nullptr, retry_ = nullptr, wait_ = nullptr, copy_ = nullptr, elevate_ = nullptr;
    HFONT font_ = nullptr, headingFont_ = nullptr, emphasisFont_ = nullptr;
    HBRUSH whiteBrush_ = nullptr;
    std::wstring selected_;
    ScanResult result_{};
    std::thread thread_;
    // Cross-thread results stay in process-owned memory; no pointer travels in
    // a WM_APP message that another process could forge.
    std::mutex resultMutex_;
    std::unique_ptr<ScanResult> pendingResult_;
    uint64_t pendingGeneration_ = 0;
    volatile LONG stop_ = 0;
    bool busy_ = false;
    bool pending_ = false;
    bool waiting_ = false;
    bool hadMatches_ = false;
    bool waitIdentityPinned_ = false;
    uint64_t waitVolume_ = 0;
    std::array<uint8_t, 16> waitFileId_{};
    bool english_ = true;
    uint64_t generation_ = 0;

    const wchar_t* Text(const wchar_t* en, const wchar_t* ru) const { return english_ ? en : ru; }
    void SetFont(HWND control, HFONT font = nullptr) {
        HFONT chosen = font ? font : font_;
        if (control && chosen)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(chosen), TRUE);
    }
    HWND Make(const wchar_t* type, const wchar_t* label, DWORD style, int id = 0, DWORD ex = 0) {
        auto h = CreateWindowExW(ex, type, label, style | WS_CHILD | WS_VISIBLE,
                                 0, 0, 100, 24, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 instance_, nullptr);
        SetFont(h);
        return h;
    }
    static UINT WindowDpi(HWND h) {
        using Fn = UINT(WINAPI*)(HWND);
        auto* fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        return fn ? fn(h) : 96u;
    }
    void RebuildFont() {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return;
        const int dpi = static_cast<int>(WindowDpi(window_));
        LOGFONTW base = metrics.lfMessageFont;
        base.lfHeight = -MulDiv(10, dpi, 72);
        base.lfWeight = FW_NORMAL;
        wcscpy_s(base.lfFaceName, L"Segoe UI");
        HFONT next = CreateFontIndirectW(&base);
        LOGFONTW large = base;
        large.lfHeight = -MulDiv(17, dpi, 72);
        large.lfWeight = FW_SEMIBOLD;
        HFONT nextHeading = CreateFontIndirectW(&large);
        LOGFONTW emphasis = base;
        emphasis.lfWeight = FW_SEMIBOLD;
        HFONT nextEmphasis = CreateFontIndirectW(&emphasis);
        if (!next || !nextHeading || !nextEmphasis) {
            if (next) DeleteObject(next);
            if (nextHeading) DeleteObject(nextHeading);
            if (nextEmphasis) DeleteObject(nextEmphasis);
            return;
        }
        const HFONT previous = font_;
        const HFONT previousHeading = headingFont_;
        const HFONT previousEmphasis = emphasisFont_;
        font_ = next;
        headingFont_ = nextHeading;
        emphasisFont_ = nextEmphasis;
        for (HWND c : {file_, list_, hint_, fileLabel_, statusDetails_, choose_,
                       retry_, wait_, copy_, elevate_})
            SetFont(c);
        SetFont(title_, headingFont_);
        SetFont(section_, emphasisFont_);
        SetFont(status_, emphasisFont_);
        if (previous) DeleteObject(previous);
        if (previousHeading) DeleteObject(previousHeading);
        if (previousEmphasis) DeleteObject(previousEmphasis);
    }
    void Resize() {
        if (!window_) return;
        RECT rc{};
        GetClientRect(window_, &rc);
        const int scale = static_cast<int>(WindowDpi(window_));
        auto u = [scale](int v) { return MulDiv(v, scale, 96); };
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int gap = u(20);
        const int inner = std::max(u(300), width - gap * 2);

        // The visual hierarchy stays stable across window sizes and DPI.
        MoveWindow(title_, gap, u(13), inner, u(34), TRUE);
        MoveWindow(hint_, gap, u(49), inner, u(23), TRUE);
        MoveWindow(fileLabel_, gap, u(86), inner, u(22), TRUE);
        const int buttonChoose = u(148);
        MoveWindow(file_, gap, u(113), inner - buttonChoose - u(10), u(36), TRUE);
        MoveWindow(choose_, width - gap - buttonChoose, u(113), buttonChoose, u(36), TRUE);
        MoveWindow(section_, gap, u(166), inner, u(25), TRUE);

        const int actionsY = height - gap - u(36);
        const int statusHeight = u(89);
        const int statusTop = actionsY - u(13) - statusHeight;
        const int listTop = u(197);
        const int listHeight = std::max(u(90), statusTop - listTop - u(12));
        MoveWindow(list_, gap, listTop, inner, listHeight, TRUE);
        MoveWindow(statusPanel_, gap, statusTop, inner, statusHeight, TRUE);
        MoveWindow(status_, gap + u(14), statusTop + u(8), inner - u(28), u(26), TRUE);
        MoveWindow(statusDetails_, gap + u(14), statusTop + u(35),
                   inner - u(28), statusHeight - u(41), TRUE);

        const int step = u(10);
        const int first = u(158);
        const int second = u(174);
        const int third = u(169);
        const int admin = u(172);
        MoveWindow(retry_, gap, actionsY, first, u(36), TRUE);
        MoveWindow(wait_, gap + first + step, actionsY, second, u(36), TRUE);
        MoveWindow(copy_, gap + first + second + step * 2, actionsY, third, u(36), TRUE);
        MoveWindow(elevate_, width - gap - admin, actionsY, admin, u(36), TRUE);

        const int firstCol = std::max(u(150), inner / 4);
        const int pidCol = u(77);
        const int matchCol = u(140);
        ListView_SetColumnWidth(list_, 0, firstCol);
        ListView_SetColumnWidth(list_, 1, pidCol);
        ListView_SetColumnWidth(list_, 2, matchCol);
        ListView_SetColumnWidth(list_, 3, std::max(u(130), inner - firstCol - pidCol - matchCol - u(5)));
    }
    void UpdateButtons() {
        EnableWindow(retry_, !selected_.empty() && !busy_);
        EnableWindow(wait_, !selected_.empty());
        EnableWindow(copy_, !selected_.empty() && !busy_);
        EnableWindow(elevate_, !selected_.empty() && !busy_ &&
                     (result_.inaccessible || result_.win32Error == ERROR_ACCESS_DENIED) && !Elevated());
        SetWindowTextW(wait_, waiting_ ? Text(L"Stop waiting", L"Остановить ожидание") :
                                       Text(L"Wait for release", L"Ждать освобождения"));
    }
    static bool Elevated() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                              elevation.TokenIsElevated != 0;
        CloseHandle(token);
        return elevated;
    }
    void ShowStatus(const std::wstring& title, const std::wstring& detail = {}) {
        if (status_) SetWindowTextW(status_, title.c_str());
        if (statusDetails_) {
            const std::wstring fallback = Text(
                L"Open handles do not prove a deletion lock. Results may be incomplete.",
                L"Открытый дескриптор не доказывает блокировку удаления. Проверка может быть неполной.");
            SetWindowTextW(statusDetails_, (detail.empty() ? fallback : detail).c_str());
        }
    }
    static std::wstring WindowsMessage(DWORD code) {
        wchar_t buffer[512]{};
        DWORD count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, buffer,
                                     static_cast<DWORD>(std::size(buffer)), nullptr);
        if (!count) return {};
        std::wstring message(buffer, count);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                    message.back() == L' ')) message.pop_back();
        return message;
    }
    void RefreshList() {
        ListView_DeleteAllItems(list_);
        for (size_t i = 0; i < result_.hits.size(); ++i) {
            const auto& hit = result_.hits[i];
            const auto slash = hit.image.find_last_of(L"\\/");
            std::wstring base = hit.image.empty() ? Text(L"[unavailable]", L"[недоступно]") :
                                (slash == std::wstring::npos ? hit.image : hit.image.substr(slash + 1));
            if (!hit.service.empty()) base += L" [" + hit.service + L"]";
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = base.data();
            const int index = ListView_InsertItem(list_, &item);
            if (index < 0) continue;
            const auto pid = std::to_wstring(hit.pid);
            ListView_SetItemText(list_, index, 1, const_cast<wchar_t*>(pid.c_str()));
            const wchar_t* method = hit.match == Match::FileId ? L"File ID" :
                Text(L"Path (heuristic)", L"Путь (оценка)");
            ListView_SetItemText(list_, index, 2, const_cast<wchar_t*>(method));
            const std::wstring extra = hit.image.empty() ? L"" : hit.image;
            ListView_SetItemText(list_, index, 3, const_cast<wchar_t*>(extra.c_str()));
        }
        std::wstring summary;
        std::wstring details = Text(
            L"An open handle is not proof that deletion is blocked. An empty result is not proof the file is free.",
            L"Открытый дескриптор не доказывает блокировку удаления. Пустой результат не гарантирует, что файл свободен.");
        if (result_.canceled) summary = Text(L"Scan canceled", L"Проверка отменена");
        else if (result_.timeout) summary = Text(L"Scan timed out", L"Время проверки истекло");
        else if (result_.win32Error) {
            summary = Text(L"Windows error: ", L"Ошибка Windows: ") + std::to_wstring(result_.win32Error);
            const auto detail = WindowsMessage(result_.win32Error);
            if (!detail.empty()) details = detail + L". " + details;
            if (result_.ntstatus) {
                wchar_t number[24]{};
                swprintf_s(number, L" (NTSTATUS 0x%08X)", result_.ntstatus);
                summary += number;
            }
        } else if (result_.hits.empty() &&
                   (result_.partial || result_.truncated || result_.inaccessible || result_.uninspectable)) {
            summary = Text(L"Scan incomplete: no confirmed matches",
                           L"Проверка неполная: совпадения не подтверждены");
        } else if (result_.hits.empty()) {
            summary = Text(L"No matching file handles found", L"Совпадающие дескрипторы не найдены");
        } else {
            summary = Text(L"Processes with open handles: ", L"Процессов с открытыми дескрипторами: ") +
                      std::to_wstring(result_.hits.size());
        }
        if (result_.partial || result_.truncated || result_.inaccessible || result_.uninspectable) {
            details += Text(L" Scan incomplete.", L" Проверка неполная.");
        }
        if (result_.inaccessible) {
            details += Text(L" Inaccessible processes: ", L" Нет доступа к процессам: ") +
                       std::to_wstring(result_.inaccessible) + L".";
        }
        ShowStatus(summary, details);
    }
    void Finish() {
        if (thread_.joinable()) thread_.join();
        busy_ = false;
        if (pending_) {
            pending_ = false;
            Start();
            return;
        }
        RefreshList();
        if (waiting_ && waitIdentityPinned_ && result_.targetIdentityAvailable &&
            (waitVolume_ != result_.targetVolume || waitFileId_ != result_.targetFileId)) {
            waiting_ = false;
            KillTimer(window_, kWaitTimer);
            ShowStatus(Text(L"Waiting stopped: file changed", L"Ожидание остановлено: файл заменён"),
                       Text(L"The selected path now refers to a different file. No release is implied.",
                            L"По выбранному пути находится другой файл. Освобождение исходного файла не подтверждено."));
            UpdateButtons();
            return;
        }
        if (waiting_ && !waitIdentityPinned_ && result_.targetIdentityAvailable &&
            !result_.hits.empty()) {
            waitVolume_ = result_.targetVolume;
            waitFileId_ = result_.targetFileId;
            waitIdentityPinned_ = true;
        }
        // A previously unopenable file can become inspectable after its owner
        // closes an exclusive handle. Do not wait forever just because we could
        // not pin its ID in the initial scan. The weaker conclusion is explicitly
        // labelled as path-based and never asserts release of the original file.
        if (waiting_ && hadMatches_ && NoMatchingHandlesObserved(result_)) {
            waiting_ = false;
            KillTimer(window_, kWaitTimer);
            if (result_.partial || result_.inaccessible || result_.uninspectable) {
                ShowStatus(Text(L"Previously found handles are no longer visible",
                                L"Ранее найденные дескрипторы больше не видны"),
                           waitIdentityPinned_ ?
                           Text(L"Scan incomplete; the file may still be held. Deletion is not guaranteed.",
                                L"Проверка неполная. Файл может всё ещё удерживаться. Удаление не гарантировано.") :
                           Text(L"Scan incomplete and original file identity unknown. It may still be held or replaced.",
                                L"Проверка неполная, исходный файл не опознан. Он мог измениться или всё ещё удерживаться."));
            } else if (waitIdentityPinned_) {
                ShowStatus(Text(L"Previously found handles are no longer visible",
                                L"Ранее найденные дескрипторы больше не видны"),
                           Text(L"Release cannot be guaranteed; deletion may still be blocked.",
                                L"Освобождение не гарантировано, удаление всё ещё может быть заблокировано."));
            } else {
                ShowStatus(Text(L"No matching handles visible at this path",
                                L"По этому пути совпадающих дескрипторов не видно"),
                           Text(L"The original file identity could not be verified; it may have changed.",
                                L"Исходная идентичность файла не подтверждена. Он мог измениться."));
            }
        }
        if (!result_.hits.empty()) hadMatches_ = true;
        UpdateButtons();
    }
    void Start() {
        if (selected_.empty()) return;
        if (busy_) {
            pending_ = true;
            InterlockedExchange(&stop_, 1);
            return;
        }
        if (thread_.joinable()) thread_.join();
        InterlockedExchange(&stop_, 0);
        busy_ = true;
        ++generation_;
        const auto currentGeneration = generation_;
        const auto path = selected_;
        HWND destination = window_;
        ShowStatus(Text(L"Scanning handles...", L"Проверка дескрипторов..."));
        UpdateButtons();
        try {
            thread_ = std::thread([this, destination, path, currentGeneration]() {
                std::unique_ptr<ScanResult> scan;
                try {
                    scan = std::make_unique<ScanResult>(RunIsolatedScan(path, &stop_));
                } catch (const std::bad_alloc&) {
                    scan.reset(new (std::nothrow) ScanResult{});
                    if (scan) { scan->win32Error = ERROR_NOT_ENOUGH_MEMORY; scan->partial = true; }
                } catch (...) {
                    scan.reset(new (std::nothrow) ScanResult{});
                    if (scan) { scan->win32Error = ERROR_GEN_FAILURE; scan->partial = true; }
                }
                {
                    std::lock_guard<std::mutex> lock(resultMutex_);
                    pendingGeneration_ = currentGeneration;
                    pendingResult_ = std::move(scan);
                }
                // Only a wakeup token is posted, never a heap pointer.
                PostMessageW(destination, kResultMessage,
                             static_cast<WPARAM>(currentGeneration), 0);
            });
        } catch (...) {
            busy_ = false;
            result_.win32Error = ERROR_NOT_ENOUGH_MEMORY;
            UpdateButtons();
            RefreshList();
        }
    }
    void Select(const std::wstring& path) {
        if (path.empty()) return;
        waiting_ = false;
        KillTimer(window_, kWaitTimer);
        selected_ = path;
        result_ = {};
        hadMatches_ = false;
        waitIdentityPinned_ = false;
        SetWindowTextW(file_, selected_.c_str());
        ListView_DeleteAllItems(list_);
        Start();
    }
    void Choose() {
        std::wstring name(32768, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = window_;
        ofn.lpstrFile = name.data();
        ofn.nMaxFile = static_cast<DWORD>(name.size());
        ofn.lpstrFilter = Text(L"All files\0*.*\0\0", L"Все файлы\0*.*\0\0");
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) Select(name.c_str());
    }
    void ToggleWait() {
        if (waiting_) {
            waiting_ = false;
            KillTimer(window_, kWaitTimer);
            if (busy_) InterlockedExchange(&stop_, 1);
            ShowStatus(Text(L"Waiting stopped.", L"Ожидание остановлено."));
        } else if (!selected_.empty()) {
            waiting_ = true;
            hadMatches_ = !result_.hits.empty();
            waitIdentityPinned_ = result_.targetIdentityAvailable;
            if (waitIdentityPinned_) {
                waitVolume_ = result_.targetVolume;
                waitFileId_ = result_.targetFileId;
            }
            SetTimer(window_, kWaitTimer, 2000, nullptr);
            if (!busy_) Start();
        }
        UpdateButtons();
    }
    std::wstring Report() const {
        std::wstring report = L"LockPeek\r\n";
        report += Text(L"File: ", L"Файл: ") + selected_ + L"\r\n";
        report += Text(L"Matching processes: ", L"Совпадающих процессов: ") +
                  std::to_wstring(result_.hits.size()) + L"\r\n";
        for (const auto& hit : result_.hits) {
            const std::wstring image = hit.image.empty() ?
                Text(L"[unavailable]", L"[недоступно]") : hit.image;
            report += L"PID " + std::to_wstring(hit.pid) + L" | " + image + L" | " +
                      (hit.match == Match::FileId ? L"File ID" :
                       Text(L"Path (heuristic)", L"Путь (эвристика)")) +
                      (hit.service.empty() ? L"" :
                       std::wstring(Text(L" | Service: ", L" | Служба: ")) + hit.service) + L"\r\n";
        }
        if (result_.timeout) {
            report += Text(L"Scan timed out. Results incomplete.\r\n",
                           L"Время проверки истекло. Результат неполный.\r\n");
        }
        if (result_.win32Error) {
            report += Text(L"Windows error: ", L"Ошибка Windows: ") +
                      std::to_wstring(result_.win32Error) + L"\r\n";
        }
        if (result_.ntstatus) {
            wchar_t buffer[24]{};
            swprintf_s(buffer, L"0x%08X", result_.ntstatus);
            report += std::wstring(L"NTSTATUS: ") + buffer + L"\r\n";
        }
        if (result_.inaccessible) {
            report += Text(L"Inaccessible processes: ", L"Недоступных процессов: ") +
                      std::to_wstring(result_.inaccessible) + L"\r\n";
        }
        report += Text(L"Not proof of a deletion lock. ", L"Не доказывает блокировку удаления. ");
        report += (result_.partial || result_.timeout || result_.win32Error) ?
                  Text(L"Scan incomplete.", L"Проверка неполная.") :
                  Text(L"No other handles are guaranteed to be absent.", L"Отсутствие других дескрипторов не гарантировано.");
        report += L"\r\n";
        return report;
    }
    void Copy() {
        if (!OpenClipboard(window_)) return;
        EmptyClipboard();
        const auto report = Report();
        const size_t bytes = (report.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            void* ptr = GlobalLock(mem);
            if (ptr) {
                std::memcpy(ptr, report.c_str(), bytes);
                GlobalUnlock(mem);
                if (SetClipboardData(CF_UNICODETEXT, mem)) mem = nullptr;
            }
            if (mem) GlobalFree(mem);
        }
        CloseClipboard();
    }
    void Elevate() {
        if (selected_.empty() || Elevated()) return;
        std::wstring exe(32768, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (!n || n >= exe.size()) return;
        exe.resize(n);
        const auto quoted = QuoteArgument(selected_);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.hwnd = window_;
        info.lpVerb = L"runas";
        info.lpFile = exe.c_str();
        info.lpParameters = quoted.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&info)) {
            if (info.hProcess) CloseHandle(info.hProcess);
        } else {
            const DWORD err = GetLastError();
            if (err != ERROR_CANCELLED) ShowStatus(Text(L"Elevation failed: ", L"Не удалось повысить права: ") + std::to_wstring(err));
        }
    }

public:
    explicit App(HINSTANCE instance) : instance_(instance) {
        english_ = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_RUSSIAN;
    }
    ~App() {
        InterlockedExchange(&stop_, 1);
        if (thread_.joinable()) thread_.join();
        if (font_) DeleteObject(font_);
        if (headingFont_) DeleteObject(headingFont_);
        if (emphasisFont_) DeleteObject(emphasisFont_);
        if (whiteBrush_) DeleteObject(whiteBrush_);
    }
    int Run(const std::wstring& initialFile, int show) {
        INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&cc);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpszClassName = L"LockPeekMainWindow";
        wc.lpfnWndProc = WindowProc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        if (!RegisterClassExW(&wc)) return 1;
        window_ = CreateWindowExW(0, wc.lpszClassName, L"LockPeek",
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 980, 620, nullptr, nullptr, instance_, this);
        if (!window_) return 2;
        ShowWindow(window_, show);
        UpdateWindow(window_);
        if (!initialFile.empty()) Select(initialFile);
        MSG msg{};
        BOOL received = 0;
        while ((received = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return received < 0 ? 3 : static_cast<int>(msg.wParam);
    }
private:
    static LRESULT CALLBACK WindowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
            auto* self = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = h;
        }
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->Dispatch(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }
    LRESULT Dispatch(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_CREATE: {
            whiteBrush_ = CreateSolidBrush(RGB(255, 255, 255));
            title_ = Make(L"STATIC", L"LockPeek", SS_LEFT);
            hint_ = Make(L"STATIC", Text(L"Find processes that have a file open",
                                          L"Найдите процессы, открывшие выбранный файл"), SS_LEFT);
            fileLabel_ = Make(L"STATIC", Text(L"FILE", L"ФАЙЛ"), SS_LEFT);
            file_ = Make(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY, 0, WS_EX_CLIENTEDGE);
            choose_ = Make(L"BUTTON", Text(L"Choose file", L"Выбрать файл"), WS_TABSTOP | BS_PUSHBUTTON, kChoose);
            section_ = Make(L"STATIC", Text(L"DETECTED PROCESSES", L"ОБНАРУЖЕННЫЕ ПРОЦЕССЫ"), SS_LEFT);
            list_ = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kProcessList, WS_EX_CLIENTEDGE);
            ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                      LVS_EX_LABELTIP);
            const wchar_t* headersEn[] = {L"Process", L"PID", L"Match", L"Executable path"};
            const wchar_t* headersRu[] = {L"Процесс", L"PID", L"Совпадение", L"Путь к программе"};
            for (int i = 0; i < 4; ++i) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                col.iSubItem = i;
                col.cx = 160;
                col.pszText = const_cast<wchar_t*>(english_ ? headersEn[i] : headersRu[i]);
                ListView_InsertColumn(list_, i, &col);
            }
            statusPanel_ = Make(L"STATIC", L"", SS_WHITERECT);
            status_ = Make(L"STATIC", Text(L"No file selected", L"Файл не выбран"), SS_LEFT, kStatusSummary);
            statusDetails_ = Make(L"STATIC",
                Text(L"Choose a file above or drop it onto the window.",
                     L"Выберите файл выше или перетащите его в окно."), SS_LEFT, kStatusDetails);
            retry_ = Make(L"BUTTON", Text(L"Scan again", L"Проверить снова"), WS_TABSTOP | BS_PUSHBUTTON, kRetry);
            wait_ = Make(L"BUTTON", Text(L"Wait for release", L"Ждать освобождения"), WS_TABSTOP | BS_PUSHBUTTON, kWait);
            copy_ = Make(L"BUTTON", Text(L"Copy report", L"Копировать отчёт"), WS_TABSTOP | BS_PUSHBUTTON, kCopy);
            elevate_ = Make(L"BUTTON", Text(L"Run as admin", L"От администратора"), WS_TABSTOP | BS_PUSHBUTTON, kElevate);
            RebuildFont();
            DragAcceptFiles(h, TRUE);
            Resize();
            UpdateButtons();
            return 0;
        }
        case WM_SIZE: Resize(); return 0;
        case WM_DPICHANGED: {
            const auto* area = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, area->left, area->top, area->right - area->left,
                         area->bottom - area->top, SWP_NOZORDER | SWP_NOACTIVATE);
            RebuildFont();
            Resize();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(l);
            const int scale = static_cast<int>(WindowDpi(h));
            info->ptMinTrackSize.x = MulDiv(760, scale, 96);
            info->ptMinTrackSize.y = MulDiv(510, scale, 96);
            return 0;
        }
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(w);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            if (count) {
                const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
                if (length && length < 32767) {
                    std::wstring name(length + 1, L'\0');
                    DragQueryFileW(drop, 0, name.data(), static_cast<UINT>(name.size()));
                    name.resize(length);
                    Select(name);
                }
            }
            DragFinish(drop);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(w);
            const auto control = reinterpret_cast<HWND>(l);
            SetBkMode(dc, TRANSPARENT);
            if (control == file_) {
                SetTextColor(dc, RGB(31, 41, 55));
                SetBkColor(dc, RGB(255, 255, 255));
                return reinterpret_cast<LRESULT>(whiteBrush_ ? whiteBrush_ : GetSysColorBrush(COLOR_WINDOW));
            }
            if (control == status_ || control == statusDetails_ || control == statusPanel_) {
                SetTextColor(dc, control == status_ ? RGB(31, 41, 55) : RGB(82, 94, 110));
                return reinterpret_cast<LRESULT>(whiteBrush_ ? whiteBrush_ : GetSysColorBrush(COLOR_WINDOW));
            }
            const bool muted = control == hint_ || control == fileLabel_;
            SetTextColor(dc, muted ? RGB(92, 102, 116) : RGB(31, 41, 55));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case kChoose: Choose(); break;
            case kRetry: Start(); break;
            case kWait: ToggleWait(); break;
            case kCopy: Copy(); break;
            case kElevate: Elevate(); break;
            default: break;
            }
            return 0;
        case WM_TIMER:
            if (w == kWaitTimer && waiting_ && !busy_) Start();
            return 0;
        case kResultMessage: {
            if (l != 0 || static_cast<uint64_t>(w) != generation_) return 0;
            std::unique_ptr<ScanResult> incoming;
            {
                std::lock_guard<std::mutex> lock(resultMutex_);
                if (pendingGeneration_ != generation_ || !pendingResult_) return 0;
                incoming = std::move(pendingResult_);
            }
            result_ = std::move(*incoming);
            Finish();
            return 0;
        }
        case WM_CLOSE:
            InterlockedExchange(&stop_, 1);
            waiting_ = false;
            KillTimer(h, kWaitTimer);
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            InterlockedExchange(&stop_, 1);
            if (thread_.joinable()) thread_.join();
            // Drain notifications; their LPARAM never carries owned memory.
            // The process-owned pendingResult_ is destroyed with App.
            MSG pending{};
            while (PeekMessageW(&pending, h, kResultMessage, kResultMessage, PM_REMOVE)) {}
            PostQuitMessage(0);
            return 0;
        }
        default: return DefWindowProcW(h, m, w, l);
        }
    }
};
} // namespace

int RunApplication(HINSTANCE instance, const std::wstring& initialFile, int show) {
    App app(instance);
    return app.Run(initialFile, show);
}
} // namespace lockpeek
