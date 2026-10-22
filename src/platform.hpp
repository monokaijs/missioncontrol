#pragma once
#include "policy.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <windows.h>
namespace mc {
std::wstring wide(const std::string &s);
std::string utf8(const std::wstring &s);
std::string nonce();
std::wstring dataDir();
Config loadConfig();
void saveConfig(const Config &c);
void setStartup(bool enabled);
bool startupEnabled();
void audit(const std::string &event);
struct Window {
    HWND hwnd{};
    DWORD pid{};
    std::string title;
};
std::vector<Window> windows();
bool closeWindow(const Window &w);
std::vector<unsigned char> screenshot(HWND target = nullptr);
std::string systemStatus();
std::string audio(const std::string &operation);
void power(bool restart);
struct CommandResult {
    std::string output;
    DWORD exitCode{};
    bool timedOut{}, cancelled{}, truncated{};
};
CommandResult runCommand(const std::string &command, const std::atomic_bool &running);
std::vector<unsigned char> readAllowed(const std::string &root, const std::string &relative,
                                       const std::atomic_bool &running);
void writeAllowed(const std::string &root, const std::string &directory, const std::string &filename,
                  const std::vector<unsigned char> &bytes, const std::atomic_bool &running);
} // namespace mc
