#include "telegram.hpp"
#include <commctrl.h>
#include <ctime>
#include <memory>
#include <set>
#include <shellapi.h>
namespace {
using namespace mc;
constexpr UINT WM_LOG = WM_APP + 1, WM_TRAY = WM_APP + 2;
constexpr int ID_SAVE = 101, ID_START = 102, ID_STOP = 103, ID_STARTUP = 104, ID_TAB = 105, ID_ACTIVITY = 106,
              ID_SHOW = 201, ID_EXIT = 202, ID_PERMISSION_BASE = 500;
HWND mainWindow{}, tokenEdit{}, usersEdit{}, chatsEdit{}, rootsEdit{}, startupCheck{}, statusLabel{},
    activityEdit{}, tabControl{};
HFONT font{}, titleFont{};
std::map<Feature, HWND> checks;
std::vector<HWND> settingsControls;
std::unique_ptr<Telegram> bot;
NOTIFYICONDATAW tray{};
Config current;
bool uiTest = false;
int dpi = 96;
int scrollY = 0;
void scrollTo(int pos) {
    RECT r{};
    GetClientRect(mainWindow, &r);
    int maxScroll = std::max(0, MulDiv(902, dpi, 96) - static_cast<int>(r.bottom));
    pos = std::clamp(pos, 0, maxScroll);
    ScrollWindowEx(mainWindow, 0, scrollY - pos, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    scrollY = pos;
    SetScrollPos(mainWindow, SB_VERT, scrollY, TRUE);
}
int scale(int n) {
    return MulDiv(n, dpi, 96);
}
HWND control(const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id = 0,
             bool settings = true) {
    auto handle = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, text,
                                  WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(w), scale(h),
                                  mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (settings)
        settingsControls.push_back(handle);
    return handle;
}
std::string text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(n + 1, 0);
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(n);
    return utf8(w);
}
void setText(HWND h, const std::string &s) {
    SetWindowTextW(h, wide(s).c_str());
}
void showLog(const std::string &s) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t time[32];
    swprintf_s(time, L"%02u:%02u:%02u  ", t.wHour, t.wMinute, t.wSecond);
    auto line = std::wstring(time) + wide(s) + L"\r\n";
    if (GetWindowTextLengthW(activityEdit) > 60000)
        SetWindowTextW(activityEdit, L"");
    SendMessageW(activityEdit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(activityEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    setText(statusLabel, s);
    tray.uFlags = NIF_TIP;
    wcsncpy_s(tray.szTip,
              bot && bot->running ? L"MissionControl — connected / connecting" : L"MissionControl — stopped",
              _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}
void postLog(std::string s) {
    auto p = new std::string(std::move(s));
    if (!PostMessageW(mainWindow, WM_LOG, 0, reinterpret_cast<LPARAM>(p)))
        delete p;
}
std::vector<int64_t> ids(std::string s) {
    for (auto &c : s)
        if (c == ',' || c == '\n' || c == '\r')
            c = ' ';
    std::istringstream input(s);
    std::vector<int64_t> out;
    std::string word;
    while (input >> word) {
        if (word.empty() || word.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("IDs must be positive decimal numbers");
        size_t end{};
        auto n = std::stoll(word, &end);
        if (n <= 0 || end != word.size())
            throw std::runtime_error("IDs must be positive numbers");
        if (std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
        if (out.size() > 16)
            throw std::runtime_error("At most 16 users and 16 private chats");
    }
    if (out.empty())
        throw std::runtime_error("Enter allowed user IDs AND private chat IDs locally");
    return out;
}
std::string join(const std::vector<int64_t> &numbers) {
    std::string s;
    for (auto n : numbers) {
        if (!s.empty())
            s += ", ";
        s += std::to_string(n);
    }
    return s;
}
void stop() {
    if (bot) {
        bot->stop();
        bot.reset();
    }
    showLog("Stopped. Polling cancelled; shell job terminated; confirmations cleared.");
}
Config readSettings() {
    Config c;
    c.token = text(tokenEdit);
    auto sep = c.token.find(':');
    if (sep == std::string::npos || sep == 0 || sep + 16 >= c.token.size() || c.token.size() > 256 ||
        c.token.substr(0, sep).find_first_not_of("0123456789") != std::string::npos ||
        c.token.substr(sep + 1).find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::runtime_error("Enter a valid BotFather token");
    c.users = ids(text(usersEdit));
    c.chats = ids(text(chatsEdit));
    std::istringstream roots(text(rootsEdit));
    std::string line;
    while (std::getline(roots, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        rootComponents(line);
        c.roots.push_back(line);
    }
    if (c.roots.size() > 16)
        throw std::runtime_error("At most 16 allowed roots");
    for (auto [p, h] : checks)
        c.permissions[p] = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if ((c.enabled(Feature::Download) || c.enabled(Feature::Upload)) && c.roots.empty())
        throw std::runtime_error("Configure at least one dedicated allowed folder for transfers");
    return c;
}
void start() {
    if (bot)
        stop();
    bot = std::make_unique<Telegram>(current, postLog);
    showLog("Connecting to Telegram...");
    bot->start();
}
void save(bool connect) {
    auto next = readSettings();
    if (bot)
        stop();
    saveConfig(next);
    current = std::move(next);
    audit("Settings saved locally");
    showLog("Settings saved. Token encrypted for this Windows account.");
    if (connect)
        start();
}
void trayMenu() {
    POINT point;
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SHOW, L"Open MissionControl");
    AppendMenuW(menu, MF_STRING, ID_STOP, L"Stop remote control");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Quit MissionControl");
    SetForegroundWindow(mainWindow);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, mainWindow, nullptr);
    DestroyMenu(menu);
}
void setup(HWND h) {
    mainWindow = h;
    dpi = static_cast<int>(GetDpiForWindow(h));
    font =
        CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    titleFont =
        CreateFontW(-scale(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto title = control(L"STATIC", L"MissionControl", SS_LEFT, 24, 16, 500, 38, 0, false);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
    control(L"STATIC", wide(std::string("Your Windows PC, under your control  •  v") + MC_VERSION).c_str(),
            SS_LEFT, 25, 55, 780, 23, 0, false);
    tabControl = control(WC_TABCONTROLW, L"", WS_TABSTOP, 20, 87, 822, 30, ID_TAB, false);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t *>(L"Connection & permissions");
    TabCtrl_InsertItem(tabControl, 0, &item);
    item.pszText = const_cast<wchar_t *>(L"Activity audit");
    TabCtrl_InsertItem(tabControl, 1, &item);
    control(L"STATIC", L"Telegram bot token (stored with Windows DPAPI)", 0, 26, 130, 800, 22);
    tokenEdit = control(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, 26, 155, 808, 30);
    SendMessageW(tokenEdit, EM_SETLIMITTEXT, 256, 0);
    control(L"STATIC", L"Allowed Telegram user IDs", 0, 26, 200, 392, 22);
    control(L"STATIC", L"Allowed private chat IDs", 0, 442, 200, 392, 22);
    usersEdit = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 26, 225, 392, 30);
    chatsEdit = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 442, 225, 392, 30);
    SendMessageW(usersEdit, EM_SETLIMITTEXT, 512, 0);
    SendMessageW(chatsEdit, EM_SETLIMITTEXT, 512, 0);
    control(L"STATIC",
            L"Both lists must match. Comma-separated IDs. No pairing, groups, channels, or inline mode.", 0,
            26, 263, 808, 22);
    control(L"STATIC", L"Allowed local file roots — one existing folder per line (root 1, root 2, ...)", 0,
            26, 303, 808, 22);
    rootsEdit = control(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP | ES_WANTRETURN,
                        26, 329, 808, 83);
    SendMessageW(rootsEdit, EM_SETLIMITTEXT, 8192, 0);
    control(L"STATIC",
            L"Example: C:\\RemoteFiles   •   Use dedicated folders. Junctions and overwrites are rejected.",
            0, 26, 420, 808, 22);
    control(L"STATIC", L"Remote permissions — every feature starts disabled", 0, 26, 460, 808, 24);
    int i = 0;
    for (auto [p, label] : features) {
        int row = i / 2, col = i % 2;
        checks[p] = control(L"BUTTON", wide(label).c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, 26 + col * 416,
                            492 + row * 32, 402, 28, ID_PERMISSION_BASE + static_cast<int>(p));
        ++i;
    }
    control(
        L"STATIC",
        L"Shell access can read or change anything your account can. File-root limits do not restrict shell.",
        0, 26, 662, 808, 23);
    startupCheck = control(L"BUTTON", L"Start MissionControl when I sign in to Windows",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 26, 704, 808, 30, ID_STARTUP);
    SendMessageW(startupCheck, BM_SETCHECK, (!uiTest && startupEnabled()) ? BST_CHECKED : BST_UNCHECKED, 0);
    control(L"BUTTON", L"Save settings", BS_PUSHBUTTON | WS_TABSTOP, 26, 754, 160, 34, ID_SAVE);
    control(L"BUTTON", L"Save & start", BS_DEFPUSHBUTTON | WS_TABSTOP, 198, 754, 180, 34, ID_START);
    control(L"BUTTON", L"Stop remote control", BS_PUSHBUTTON | WS_TABSTOP, 390, 754, 190, 34, ID_STOP);
    activityEdit = control(L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 26, 130,
                           808, 604, ID_ACTIVITY, false);
    ShowWindow(activityEdit, SW_HIDE);
    statusLabel = control(L"STATIC", L"Stopped — configure locally, then Save & start.", SS_LEFT, 26, 810,
                          808, 42, 0, false);
    control(L"STATIC", L"Closing this window keeps the visible tray icon running. Use tray → Quit to exit.",
            0, 26, 860, 808, 24, 0, false);
    if (uiTest) {
        PostMessageW(h, WM_APP + 4, 0, 0);
        return;
    }
    tray.cbSize = sizeof tray;
    tray.hWnd = h;
    tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray.uCallbackMessage = WM_TRAY;
    tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray.szTip, L"MissionControl — stopped");
    Shell_NotifyIconW(NIM_ADD, &tray);
    tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray);
    try {
        current = loadConfig();
        setText(tokenEdit, current.token);
        setText(usersEdit, join(current.users));
        setText(chatsEdit, join(current.chats));
        std::string roots;
        for (auto &r : current.roots)
            roots += r + "\r\n";
        setText(rootsEdit, roots);
        for (auto [p, c] : checks)
            SendMessageW(c, BM_SETCHECK, current.enabled(p) ? BST_CHECKED : BST_UNCHECKED, 0);
        if (!current.token.empty() && !current.users.empty() && !current.chats.empty())
            PostMessageW(h, WM_APP + 3, 0, 0);
    } catch (const std::exception &e) {
        showLog(e.what());
        MessageBoxW(h, wide(e.what()).c_str(), L"Settings could not be loaded", MB_ICONWARNING);
    }
    SetTimer(h, 1, 1000, nullptr);
}
LRESULT CALLBACK procedure(HWND h, UINT message, WPARAM w, LPARAM l) {
    try {
        switch (message) {
        case WM_CREATE:
            setup(h);
            return 0;
        case WM_APP + 3:
            start();
            return 0;
        case WM_APP + 4: {
            bool valid =
                tokenEdit && usersEdit && chatsEdit && rootsEdit && activityEdit && checks.size() == 9;
            for (auto [p, c] : checks) {
                (void)p;
                valid = valid && IsWindow(c) && SendMessageW(c, BM_GETCHECK, 0, 0) == BST_UNCHECKED;
            }
            DestroyWindow(h);
            if (!valid)
                PostQuitMessage(1);
            return 0;
        }
        case WM_LOG: {
            std::unique_ptr<std::string> p(reinterpret_cast<std::string *>(l));
            showLog(*p);
            return 0;
        }
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR *>(l)->idFrom == ID_TAB &&
                reinterpret_cast<NMHDR *>(l)->code == TCN_SELCHANGE) {
                bool settings = TabCtrl_GetCurSel(tabControl) == 0;
                for (auto c : settingsControls)
                    ShowWindow(c, settings ? SW_SHOW : SW_HIDE);
                ShowWindow(activityEdit, settings ? SW_HIDE : SW_SHOW);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(w) >= ID_PERMISSION_BASE && LOWORD(w) < ID_PERMISSION_BASE + 9 &&
                HIWORD(w) == BN_CLICKED) {
                stop();
                showLog("Permissions changed. Stopped; Save & start to apply new settings.");
                return 0;
            }
            switch (LOWORD(w)) {
            case ID_SAVE:
                save(false);
                return 0;
            case ID_START:
                save(true);
                return 0;
            case ID_STOP:
                stop();
                return 0;
            case ID_STARTUP: {
                bool on = SendMessageW(startupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                try {
                    setStartup(on);
                    audit(on ? "Windows startup enabled locally" : "Windows startup disabled locally");
                } catch (...) {
                    SendMessageW(startupCheck, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
                    throw;
                }
                return 0;
            }
            case ID_SHOW:
                ShowWindow(h, SW_RESTORE);
                SetForegroundWindow(h);
                return 0;
            case ID_EXIT:
                DestroyWindow(h);
                return 0;
            }
            break;
        case WM_TRAY:
            if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == WM_RBUTTONUP)
                trayMenu();
            else if (LOWORD(l) == NIN_SELECT || LOWORD(l) == WM_LBUTTONDBLCLK) {
                ShowWindow(h, SW_RESTORE);
                SetForegroundWindow(h);
            }
            return 0;
        case WM_SIZE: {
            RECT r{};
            GetClientRect(h, &r);
            SCROLLINFO info{sizeof info,
                            SIF_RANGE | SIF_PAGE | SIF_POS,
                            0,
                            scale(902) - 1,
                            static_cast<UINT>(r.bottom),
                            scrollY,
                            0};
            SetScrollInfo(h, SB_VERT, &info, TRUE);
            scrollTo(scrollY);
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO info{sizeof info, SIF_ALL};
            GetScrollInfo(h, SB_VERT, &info);
            int pos = scrollY;
            switch (LOWORD(w)) {
            case SB_LINEUP:
                pos -= scale(36);
                break;
            case SB_LINEDOWN:
                pos += scale(36);
                break;
            case SB_PAGEUP:
                pos -= static_cast<int>(info.nPage);
                break;
            case SB_PAGEDOWN:
                pos += static_cast<int>(info.nPage);
                break;
            case SB_THUMBTRACK:
                pos = info.nTrackPos;
                break;
            }
            scrollTo(pos);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scrollTo(scrollY - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * scale(72));
            return 0;
        case WM_TIMER:
            EnableWindow(GetDlgItem(h, ID_STOP), bot != nullptr);
            return 0;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);
            return 0;
        case WM_DESTROY:
            KillTimer(h, 1);
            stop();
            Shell_NotifyIconW(NIM_DELETE, &tray);
            if (font)
                DeleteObject(font);
            if (titleFont)
                DeleteObject(titleFont);
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception &e) {
        MessageBoxW(h, wide(e.what()).c_str(), L"MissionControl", MB_OK | MB_ICONWARNING);
    }
    return DefWindowProcW(h, message, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    if (std::wstring(commandLine) == L"--self-test") {
        mc::Config c;
        auto update = mc::json{{"message",
                                {{"from", {{"id", 1}, {"is_bot", false}}},
                                 {"chat", {{"id", 1}, {"type", "private"}}},
                                 {"text", "/exec echo test"}}}};
        return mc::route(update, c) ? 1 : 0;
    }
    uiTest = std::wstring(commandLine) == L"--ui-test";
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MissionControl.SingleInstance");
    if (!mutex)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        auto existing = FindWindowW(L"MissionControl.Main", nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = procedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MissionControl.Main";
    RegisterClassExW(&wc);
    // Scale to fit common 1080p displays; native controls remain keyboard accessible.
    UINT systemDpi = GetDpiForSystem();
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT area{0, 0, MulDiv(885, systemDpi, 96),
              std::min(MulDiv(902, systemDpi, 96),
                       static_cast<int>(work.bottom - work.top) - MulDiv(90, systemDpi, 96))};
    AdjustWindowRectExForDpi(&area, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL,
                             FALSE, 0, systemDpi);
    auto h = CreateWindowExW(0, wc.lpszClassName, L"MissionControl",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL,
                             CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
                             nullptr, nullptr, instance, nullptr);
    if (!h) {
        CloseHandle(mutex);
        return 1;
    }
    ShowWindow(h, (uiTest || std::wstring(commandLine) == L"--startup") ? SW_HIDE : show);
    UpdateWindow(h);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    MSG pending;
    while (PeekMessageW(&pending, h, WM_LOG, WM_LOG, PM_REMOVE))
        delete reinterpret_cast<std::string *>(pending.lParam);
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
