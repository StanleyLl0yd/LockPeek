#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app.h"
#include "scanner.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
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

class App {
    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND file_ = nullptr, list_ = nullptr, status_ = nullptr, hint_ = nullptr;
    HWND choose_ = nullptr, retry_ = nullptr, wait_ = nullptr, copy_ = nullptr, elevate_ = nullptr;
    HFONT font_ = nullptr;
    std::wstring selected_;
    ScanResult result_{};
    std::thread thread_;
    volatile LONG stop_ = 0;
    bool busy_ = false;
    bool pending_ = false;
    bool waiting_ = false;
    bool hadMatches_ = false;
    bool english_ = true;
    uint64_t generation_ = 0;

    const wchar_t* Text(const wchar_t* en, const wchar_t* ru) const { return english_ ? en : ru; }
    void SetFont(HWND control) {
        if (control && font_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
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
        metrics.lfMessageFont.lfHeight = -MulDiv(14, static_cast<int>(WindowDpi(window_)), 72);
        HFONT next = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!next) return;
        const auto previous = font_;
        font_ = next;
        for (HWND c : {file_, list_, status_, hint_, choose_, retry_, wait_, copy_, elevate_})
            SetFont(c);
        if (previous) DeleteObject(previous);
    }
    void Resize() {
        if (!window_) return;
        RECT rc{};
        GetClientRect(window_, &rc);
        const int scale = static_cast<int>(WindowDpi(window_));
        auto u = [scale](int v) { return MulDiv(v, scale, 96); };
        const int gap = u(14), row = u(30), button = u(142);
        const int width = rc.right - rc.left;
        const int inner = std::max(u(200), width - gap * 2);
        const int y = gap;
        MoveWindow(file_, gap, y, std::max(u(100), inner - button - u(10)), row, TRUE);
        MoveWindow(choose_, width - gap - button, y, button, row, TRUE);
        MoveWindow(hint_, gap, y + row + u(8), inner, u(24), TRUE);
        const int listTop = y + row + u(40);
        const int controlsTop = std::max(listTop + u(90), rc.bottom - u(116));
        MoveWindow(list_, gap, listTop, inner, std::max(u(60), controlsTop - listTop - u(10)), TRUE);
        MoveWindow(status_, gap, controlsTop, inner, u(39), TRUE);
        const int buttonsY = controlsTop + u(43);
        const int smallGap = u(7);
        const int w = std::max(u(90), (inner - smallGap * 3) / 4);
        MoveWindow(retry_, gap, buttonsY, w, row, TRUE);
        MoveWindow(wait_, gap + w + smallGap, buttonsY, w, row, TRUE);
        MoveWindow(copy_, gap + (w + smallGap) * 2, buttonsY, w, row, TRUE);
        MoveWindow(elevate_, gap + (w + smallGap) * 3, buttonsY, w, row, TRUE);
        const int firstCol = std::max(u(145), inner / 4);
        ListView_SetColumnWidth(list_, 0, firstCol);
        ListView_SetColumnWidth(list_, 1, u(86));
        ListView_SetColumnWidth(list_, 2, u(110));
        ListView_SetColumnWidth(list_, 3, std::max(u(120), inner - firstCol - u(200)));
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
    void ShowStatus(std::wstring text) {
        SetWindowTextW(status_, text.c_str());
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
        std::wstring message;
        if (result_.canceled) message = Text(L"Scan canceled.", L"Проверка отменена.");
        else if (result_.timeout) message = Text(L"Scan timed out; results unknown.", L"Время проверки истекло; результат неизвестен.");
        else if (result_.win32Error) {
            message = Text(L"Windows error: ", L"Ошибка Windows: ") + std::to_wstring(result_.win32Error);
            const auto detail = WindowsMessage(result_.win32Error);
            if (!detail.empty()) message += L": " + detail;
            if (result_.ntstatus) {
                wchar_t number[24]{};
                swprintf_s(number, L" (NTSTATUS 0x%08X)", result_.ntstatus);
                message += number;
            }
        } else if (result_.hits.empty()) {
            message = Text(L"No matching open file handles found. Deletion is not guaranteed.",
                           L"Совпадающие дескрипторы не найдены. Удаление файла не гарантировано.");
        } else {
            message = std::to_wstring(result_.hits.size()) +
                      Text(L" process(es) with matching open handles. This does NOT prove a delete lock.",
                           L" процесс(ов) с открытыми дескрипторами. Это НЕ доказывает блокировку удаления.");
        }
        if (result_.partial || result_.truncated || result_.inaccessible || result_.uninspectable) {
            message += Text(L" Scan incomplete/limited.", L" Проверка неполная/ограниченная.");
        }
        if (result_.inaccessible) message += Text(L" Access denied for ", L" Нет доступа к процессам: ") +
                                                 std::to_wstring(result_.inaccessible) + L".";
        ShowStatus(std::move(message));
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
        if (waiting_ && hadMatches_ && result_.hits.empty() && !result_.partial &&
            !result_.timeout && !result_.win32Error) {
            waiting_ = false;
            KillTimer(window_, kWaitTimer);
            ShowStatus(Text(L"Previously detected processes no longer have matching handles. Deletion is not guaranteed.",
                            L"Ранее найденные процессы больше не удерживают обнаруженные дескрипторы. Удаление не гарантировано."));
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
                if (!PostMessageW(destination, kResultMessage,
                                  static_cast<WPARAM>(currentGeneration),
                                  reinterpret_cast<LPARAM>(scan.get()))) return;
                scan.release();
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
        ofn.lpstrFilter = L"All files\0*.*\0\0";
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
            report += L"PID " + std::to_wstring(hit.pid) + L" | " +
                      (hit.image.empty() ? L"[unavailable]" : hit.image) + L" | " +
                      (hit.match == Match::FileId ? L"File ID" : L"Path (heuristic)") +
                      (hit.service.empty() ? L"" : L" | Service: " + hit.service) + L"\r\n";
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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&wc)) return 1;
        window_ = CreateWindowExW(0, wc.lpszClassName, L"LockPeek",
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 1000, 550, nullptr, nullptr, instance_, this);
        if (!window_) return 2;
        ShowWindow(window_, show);
        UpdateWindow(window_);
        if (!initialFile.empty()) Select(initialFile);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
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
            RebuildFont();
            file_ = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 0, WS_EX_CLIENTEDGE);
            choose_ = Make(L"BUTTON", Text(L"Choose file", L"Выбрать файл"), WS_TABSTOP | BS_PUSHBUTTON, kChoose);
            hint_ = Make(L"STATIC", Text(L"Drop a file here or choose one to inspect", L"Перетащите файл сюда или выберите его"), SS_LEFT);
            list_ = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, WS_EX_CLIENTEDGE);
            ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
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
            status_ = Make(L"STATIC", Text(L"No file selected.", L"Файл не выбран."), SS_LEFT);
            retry_ = Make(L"BUTTON", Text(L"Scan again", L"Проверить снова"), WS_TABSTOP | BS_PUSHBUTTON, kRetry);
            wait_ = Make(L"BUTTON", Text(L"Wait for release", L"Ждать освобождения"), WS_TABSTOP | BS_PUSHBUTTON, kWait);
            copy_ = Make(L"BUTTON", Text(L"Copy result", L"Копировать результат"), WS_TABSTOP | BS_PUSHBUTTON, kCopy);
            elevate_ = Make(L"BUTTON", Text(L"Run as admin", L"От администратора"), WS_TABSTOP | BS_PUSHBUTTON, kElevate);
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
            info->ptMinTrackSize.x = MulDiv(650, scale, 96);
            info->ptMinTrackSize.y = MulDiv(390, scale, 96);
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
            std::unique_ptr<ScanResult> incoming(reinterpret_cast<ScanResult*>(l));
            if (static_cast<uint64_t>(w) == generation_) {
                if (incoming) result_ = std::move(*incoming);
                else { result_ = {}; result_.win32Error = ERROR_NOT_ENOUGH_MEMORY; result_.partial = true; }
                Finish();
            }
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
            // A worker may have posted while WM_DESTROY was joining it.
            // Remove and free any result payloads that the loop won't consume.
            MSG pending{};
            while (PeekMessageW(&pending, h, kResultMessage, kResultMessage, PM_REMOVE))
                delete reinterpret_cast<ScanResult*>(pending.lParam);
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
