#include "platform.hpp"
#include <bcrypt.h>
#include <cmath>
#include <endpointvolume.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mmdeviceapi.h>
#include <mutex>
#include <shlobj.h>
#include <thread>
#include <wincodec.h>
#include <wincrypt.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace mc {
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&o) noexcept : h(o.h) {
        o.h = INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const {
        return h && h != INVALID_HANDLE_VALUE;
    }
};
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void hr(HRESULT code) {
    check(SUCCEEDED(code), "Windows component operation failed");
}
std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    auto n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    check(n > 0, "Invalid UTF-8");
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string utf8(const std::wstring &w) {
    if (w.empty())
        return {};
    auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr,
                                 0, nullptr, nullptr);
    check(n > 0, "Invalid Unicode");
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}
std::string nonce() {
    unsigned char b[16];
    check(BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
          "Random generation failed");
    std::ostringstream s;
    for (auto v : b)
        s << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(v);
    return s.str();
}
std::wstring dataDir() {
    PWSTR path = nullptr;
    hr(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path));
    std::wstring d = path;
    CoTaskMemFree(path);
    d += L"\\MissionControl";
    check(CreateDirectoryW(d.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS,
          "Cannot create local settings folder");
    return d;
}
static std::string protect(const std::string &token) {
    if (token.empty())
        return {};
    DATA_BLOB in{static_cast<DWORD>(token.size()),
                 reinterpret_cast<BYTE *>(const_cast<char *>(token.data()))},
        out{};
    check(CryptProtectData(&in, L"MissionControl Telegram token", nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &out) != 0,
          "DPAPI protection failed");
    std::string hex;
    static constexpr char digits[] = "0123456789abcdef";
    for (DWORD i = 0; i < out.cbData; ++i) {
        hex += digits[out.pbData[i] >> 4];
        hex += digits[out.pbData[i] & 15];
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return hex;
}
static std::string unprotect(const std::string &hex) {
    if (hex.empty())
        return {};
    check(hex.size() % 2 == 0 && hex.size() < 32768, "Invalid token storage");
    std::vector<BYTE> b;
    auto nib = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        throw std::runtime_error("Invalid token encoding");
    };
    for (size_t i = 0; i < hex.size(); i += 2)
        b.push_back(static_cast<BYTE>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    DATA_BLOB in{static_cast<DWORD>(b.size()), b.data()}, out{};
    check(CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) != 0,
          "Cannot unlock token for this Windows account");
    std::string token(reinterpret_cast<char *>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return token;
}
Config loadConfig() {
    Config c;
    auto file = std::filesystem::path(dataDir()) / L"config.json";
    if (!std::filesystem::exists(file))
        return c;
    check(std::filesystem::file_size(file) < 65536, "Settings too large");
    std::ifstream f(file);
    json j;
    f >> j;
    c.token = unprotect(j.value("token_dpapi", std::string{}));
    c.users = j.value("users", std::vector<int64_t>{});
    c.chats = j.value("private_chats", std::vector<int64_t>{});
    c.roots = j.value("roots", std::vector<std::string>{});
    for (auto [p, label] : features) {
        (void)label;
        c.permissions[p] = j.value("permissions", json::object()).value(key(p), false);
    }
    return c;
}
void saveConfig(const Config &c) {
    json perms = json::object();
    for (auto [p, label] : features) {
        (void)label;
        perms[key(p)] = c.enabled(p);
    }
    auto text = json{
        {"token_dpapi", protect(c.token)},
        {"users", c.users},
        {"private_chats", c.chats},
        {"roots", c.roots},
        {"permissions",
         perms}}.dump(2);
    auto dest = dataDir() + L"\\config.json", temp = dataDir() + L"\\settings-" + wide(nonce()) + L".tmp";
    {
        Handle h(
            CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        check(bool(h), "Cannot save settings");
        DWORD written{};
        if (!WriteFile(h.h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
            written != text.size() || !FlushFileBuffers(h.h)) {
            DeleteFileW(temp.c_str());
            throw std::runtime_error("Settings write failed");
        }
    }
    if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Settings replace failed");
    }
}
void setStartup(bool enabled) {
    HKEY k{};
    check(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
                          0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS,
          "Cannot open startup setting");
    LSTATUS s;
    if (enabled) {
        wchar_t path[32768]{};
        auto n = GetModuleFileNameW(nullptr, path, 32768);
        if (!n || n >= 32768) {
            RegCloseKey(k);
            throw std::runtime_error("Executable path unavailable");
        }
        std::wstring cmd = L"\"" + std::wstring(path) + L"\" --startup";
        s = RegSetValueExW(k, L"MissionControl", 0, REG_SZ, reinterpret_cast<const BYTE *>(cmd.c_str()),
                           static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        s = RegDeleteValueW(k, L"MissionControl");
        if (s == ERROR_FILE_NOT_FOUND)
            s = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    check(s == ERROR_SUCCESS, "Startup setting update failed");
}
bool startupEnabled() {
    DWORD type{}, n{};
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        L"MissionControl", RRF_RT_REG_SZ, &type, nullptr, &n) == ERROR_SUCCESS;
}
void audit(const std::string &event) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    try {
        auto path = std::filesystem::path(dataDir()) / L"activity.log";
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1024 * 1024) {
            std::error_code ec;
            auto old = path;
            old += L".1";
            std::filesystem::remove(old, ec);
            std::filesystem::rename(path, old, ec);
        }
        std::ofstream f(path, std::ios::app);
        SYSTEMTIME t;
        GetLocalTime(&t);
        f << std::setfill('0') << t.wYear << '-' << std::setw(2) << t.wMonth << '-' << std::setw(2) << t.wDay
          << ' ' << std::setw(2) << t.wHour << ':' << std::setw(2) << t.wMinute << ':' << std::setw(2)
          << t.wSecond << " " << event << "\n";
    } catch (...) {
    }
}
std::vector<Window> windows() {
    std::vector<Window> result;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER))
                return TRUE;
            wchar_t title[512]{};
            if (GetWindowTextW(h, title, 512) > 0) {
                DWORD pid{};
                GetWindowThreadProcessId(h, &pid);
                if (pid != GetCurrentProcessId())
                    try {
                        reinterpret_cast<std::vector<Window> *>(p)->push_back({h, pid, utf8(title)});
                    } catch (...) {
                    }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    if (result.size() > 40)
        result.resize(40);
    return result;
}
bool closeWindow(const Window &w) {
    DWORD pid{};
    GetWindowThreadProcessId(w.hwnd, &pid);
    if (pid != w.pid || !IsWindow(w.hwnd) || pid == GetCurrentProcessId())
        return false;
    DWORD_PTR result{};
    return SendMessageTimeoutW(w.hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0;
}
std::vector<unsigned char> screenshot(HWND target) {
    RECT r{};
    if (target) {
        check(IsWindow(target) && !IsIconic(target), "Window unavailable or minimized");
        check(GetWindowRect(target, &r) != 0, "Window bounds unavailable");
    } else {
        r = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    int width = r.right - r.left, height = r.bottom - r.top;
    check(width > 0 && height > 0 && static_cast<int64_t>(width) * height <= 40000000,
          "Capture bounds too large");
    HDC screen = GetDC(nullptr);
    check(screen != nullptr, "Desktop unavailable");
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    if (!memory || !bitmap) {
        if (memory)
            DeleteDC(memory);
        if (bitmap)
            DeleteObject(bitmap);
        ReleaseDC(nullptr, screen);
        throw std::runtime_error("Capture allocation failed");
    }
    auto old = SelectObject(memory, bitmap);
    BOOL ok = BitBlt(memory, 0, 0, width, height, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!ok) {
        DeleteObject(bitmap);
        throw std::runtime_error("Capture failed");
    }
    try {
        ComPtr<IWICImagingFactory> factory;
        hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        ComPtr<IWICBitmap> image;
        hr(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image));
        DeleteObject(bitmap);
        bitmap = nullptr;
        ComPtr<IStream> stream;
        hr(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
        ComPtr<IWICBitmapEncoder> encoder;
        hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
        hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        hr(encoder->CreateNewFrame(&frame, nullptr));
        hr(frame->Initialize(nullptr));
        hr(frame->SetSize(width, height));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        hr(frame->SetPixelFormat(&fmt));
        hr(frame->WriteSource(image.Get(), nullptr));
        hr(frame->Commit());
        hr(encoder->Commit());
        STATSTG st{};
        hr(stream->Stat(&st, STATFLAG_NONAME));
        check(st.cbSize.QuadPart <= 20 * 1024 * 1024, "PNG exceeds transfer cap");
        LARGE_INTEGER zero{};
        hr(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
        std::vector<unsigned char> bytes(static_cast<size_t>(st.cbSize.QuadPart));
        ULONG read{};
        hr(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
        check(read == bytes.size(), "PNG stream incomplete");
        return bytes;
    } catch (...) {
        if (bitmap)
            DeleteObject(bitmap);
        throw;
    }
}
std::string systemStatus() {
    MEMORYSTATUSEX mem{sizeof mem};
    check(GlobalMemoryStatusEx(&mem) != 0, "Memory status unavailable");
    wchar_t name[256]{};
    DWORD n = 256;
    GetComputerNameW(name, &n);
    SYSTEM_POWER_STATUS battery{};
    GetSystemPowerStatus(&battery);
    ULARGE_INTEGER free{}, total{};
    wchar_t drive[MAX_PATH]{};
    GetWindowsDirectoryW(drive, MAX_PATH);
    drive[3] = 0;
    GetDiskFreeSpaceExW(drive, &free, &total, nullptr);
    std::ostringstream s;
    s << "MissionControl on " << utf8(name) << "\nUptime: " << GetTickCount64() / 60000
      << " min\nMemory: " << mem.dwMemoryLoad << "% used (" << mem.ullAvailPhys / (1024 * 1024)
      << " MiB available)\nSystem drive: " << free.QuadPart / (1024 * 1024 * 1024) << " GiB free\nAC power: "
      << (battery.ACLineStatus == 1   ? "connected"
          : battery.ACLineStatus == 0 ? "battery"
                                      : "unknown")
      << "\nAccount privileges: current user; no elevation";
    return s.str();
}
std::string audio(const std::string &operation) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&enumerator)));
    ComPtr<IMMDevice> device;
    hr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
    ComPtr<IAudioEndpointVolume> volume;
    hr(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                        reinterpret_cast<void **>(volume.GetAddressOf())));
    float v{};
    BOOL muted{};
    hr(volume->GetMasterVolumeLevelScalar(&v));
    hr(volume->GetMute(&muted));
    if (operation == "mute") {
        hr(volume->SetMute(!muted, nullptr));
        muted = !muted;
    } else if (!operation.empty()) {
        float next = v;
        if (operation == "up")
            next += 0.1f;
        else if (operation == "down")
            next -= 0.1f;
        else {
            size_t end{};
            int value = std::stoi(operation, &end);
            check(end == operation.size() && value >= 0 && value <= 100, "Volume must be 0..100");
            next = value / 100.0f;
        }
        v = std::clamp(next, 0.0f, 1.0f);
        hr(volume->SetMasterVolumeLevelScalar(v, nullptr));
    }
    return "Volume: " + std::to_string(static_cast<int>(std::lround(v * 100))) + "%" +
           (muted ? " (muted)" : "");
}
void power(bool restart) {
    Handle token;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token.h) != 0,
          "Cannot open process token");
    TOKEN_PRIVILEGES priv{};
    priv.PrivilegeCount = 1;
    check(LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &priv.Privileges[0].Luid) != 0,
          "Shutdown privilege unavailable");
    priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    check(AdjustTokenPrivileges(token.h, FALSE, &priv, 0, nullptr, nullptr) != 0 &&
              GetLastError() == ERROR_SUCCESS,
          "Account cannot shut down Windows");
    check(ExitWindowsEx(restart ? EWX_REBOOT : EWX_POWEROFF,
                        SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED) != 0,
          "Windows refused power operation");
}
CommandResult runCommand(const std::string &command, const std::atomic_bool &running) {
    check(CommandPolicy::valid(command), "Command must be a single line, 1..4096 bytes");
    check(running.load(), "Stopped");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    Handle read, write;
    check(CreatePipe(&read.h, &write.h, &sa, 0) != 0, "Cannot create output pipe");
    check(SetHandleInformation(read.h, HANDLE_FLAG_INHERIT, 0) != 0, "Cannot secure output pipe");
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                             nullptr));
    check(bool(input), "Cannot open command input");
    Handle job(CreateJobObjectW(nullptr, nullptr));
    check(bool(job), "Cannot create command job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 16;
    check(SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof limits) != 0,
          "Cannot bound command job");
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    check(InitializeProcThreadAttributeList(attrs, 1, 0, &size) != 0, "Cannot initialize process isolation");
    struct AttrCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST p;
        ~AttrCleanup() {
            DeleteProcThreadAttributeList(p);
        }
    } cleanup{attrs};
    HANDLE inherited[] = {write.h, input.h};
    check(UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited,
                                    nullptr, nullptr) != 0,
          "Cannot restrict inherited handles");
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = input.h;
    si.StartupInfo.hStdOutput = write.h;
    si.StartupInfo.hStdError = write.h;
    si.lpAttributeList = attrs;
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring exe = std::wstring(sys) + L"\\cmd.exe",
                 line = L"\"" + exe + L"\" /d /s /c \"" + wide(command) + L"\"";
    PROCESS_INFORMATION pi{};
    check(CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                         &si.StartupInfo, &pi) != 0,
          "Command creation failed");
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.h, process.h)) {
        TerminateProcess(process.h, 1);
        throw std::runtime_error("Cannot contain command process");
    }
    check(ResumeThread(thread.h) != static_cast<DWORD>(-1), "Cannot start command");
    CloseHandle(write.h);
    write.h = INVALID_HANDLE_VALUE;
    CommandResult result;
    auto start = GetTickCount64();
    for (;;) {
        DWORD available{};
        if (PeekNamedPipe(read.h, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[4096];
            DWORD got{};
            if (ReadFile(read.h, buffer, std::min<DWORD>(available, sizeof buffer), &got, nullptr)) {
                auto room = CommandPolicy::outputCap - result.output.size();
                result.output.append(buffer, std::min<size_t>(room, got));
                if (got > room) {
                    result.truncated = true;
                    TerminateJobObject(job.h, 1);
                    break;
                }
            }
        }
        if (!running.load()) {
            result.cancelled = true;
            TerminateJobObject(job.h, 1);
            break;
        }
        if (GetTickCount64() - start >= CommandPolicy::timeoutMs) {
            result.timedOut = true;
            TerminateJobObject(job.h, 1);
            break;
        }
        if (WaitForSingleObject(process.h, 10) == WAIT_OBJECT_0) {
            DWORD avail{};
            if (!PeekNamedPipe(read.h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                break;
        }
    }
    TerminateJobObject(job.h, 1);
    WaitForSingleObject(process.h, 2000);
    GetExitCodeProcess(process.h, &result.exitCode);
    // cmd output follows the OEM code page. Preserve binary-safe raw bytes for document output elsewhere.
    return result;
}
struct LockedPath {
    std::wstring path;
    std::vector<Handle> guards;
};
static void validateHandle(HANDLE h, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(h, &info) != 0, "Cannot inspect file handle");
    check(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Reparse points are not allowed");
    check(bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory, "Unexpected path type");
    if (!directory)
        check(info.nNumberOfLinks == 1, "Hard-linked files are not allowed");
}
static LockedPath lockPath(const std::string &root, const std::string &relativeDirectory) {
    auto rootParts = rootComponents(root);
    auto child = components(relativeDirectory, true);
    rootParts.insert(rootParts.end(), child.begin(), child.end());
    LockedPath out;
    out.path = wide(root.substr(0, 3));
    check(GetDriveTypeW(out.path.c_str()) == DRIVE_FIXED ||
              GetDriveTypeW(out.path.c_str()) == DRIVE_REMOVABLE,
          "Only local drives are allowed");
    for (auto &p : rootParts) {
        if (out.path.back() != L'\\' && out.path.back() != L'/')
            out.path += L'\\';
        out.path += wide(p);
        check(out.path.size() < 32000, "Path too long");
        Handle h(CreateFileW(out.path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        check(bool(h), "Cannot lock folder (missing, busy, or denied)");
        validateHandle(h.h, true);
        out.guards.push_back(std::move(h));
    }
    return out;
}
std::vector<unsigned char> readAllowed(const std::string &root, const std::string &relative,
                                       const std::atomic_bool &running) {
    auto parts = components(relative);
    std::string dir;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!dir.empty())
            dir += '/';
        dir += parts[i];
    }
    auto locked = lockPath(root, dir);
    auto path = locked.path + L"\\" + wide(parts.back());
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    check(bool(file), "Cannot open file");
    validateHandle(file.h, false);
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file.h, &size) != 0 && size.QuadPart >= 0 && size.QuadPart <= 20 * 1024 * 1024,
          "File exceeds 20 MiB cap");
    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    size_t offset{};
    while (offset < bytes.size()) {
        check(running.load(), "Stopped");
        DWORD got{};
        check(ReadFile(file.h, bytes.data() + offset,
                       static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - offset)), &got, nullptr) &&
                  got > 0,
              "File read failed");
        offset += got;
    }
    return bytes;
}
void writeAllowed(const std::string &root, const std::string &directory, const std::string &filename,
                  const std::vector<unsigned char> &bytes, const std::atomic_bool &running) {
    check(components(filename).size() == 1, "Document filename must be a plain filename");
    check(bytes.size() <= 20 * 1024 * 1024, "Document exceeds 20 MiB cap");
    auto locked = lockPath(root, directory);
    auto path = locked.path + L"\\" + wide(filename);
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(bool(file), "Cannot create new file (exists, busy, or denied)");
    try {
        validateHandle(file.h, false);
        size_t offset{};
        while (offset < bytes.size()) {
            check(running.load(), "Stopped");
            DWORD wrote{};
            check(WriteFile(file.h, bytes.data() + offset,
                            static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - offset)), &wrote,
                            nullptr) &&
                      wrote > 0,
                  "Document write failed");
            offset += wrote;
        }
        check(running.load(), "Stopped");
        check(FlushFileBuffers(file.h) != 0, "Cannot flush document");
    } catch (...) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file.h, FileDispositionInfo, &disposition, sizeof disposition);
        throw;
    }
}
} // namespace mc
