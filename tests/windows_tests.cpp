#include "platform.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace mc;
int checks = 0;
void expect(bool c, const char *why) {
    ++checks;
    if (!c)
        throw std::runtime_error(why);
}
template <typename Fn> bool rejects(Fn fn) {
    try {
        fn();
        return false;
    } catch (...) {
        return true;
    }
}
int main() {
    std::filesystem::path base;
    try {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        base = std::filesystem::path(temp) / (L"MissionControlTests-" + wide(nonce()));
        std::filesystem::create_directories(base / L"root" / L"sub");
        std::filesystem::create_directory(base / L"outside");
        std::atomic_bool active{true};
        auto root = utf8((base / L"root").wstring());
        writeAllowed(root, "sub", "fixture.txt", {'h', 'e', 'l', 'l', 'o'}, active);
        expect(readAllowed(root, "sub/fixture.txt", active) ==
                   std::vector<unsigned char>({'h', 'e', 'l', 'l', 'o'}),
               "Round trip fixture");
        expect(rejects([&] { writeAllowed(root, "sub", "fixture.txt", {'x'}, active); }), "Overwrite denied");
        expect(rejects([&] { readAllowed(root, "../outside/secret", active); }), "Traversal denied");
        expect(rejects([&] { writeAllowed(root, "", "../escape.txt", {}, active); }),
               "Filename traversal denied");
        expect(rejects([&] { readAllowed(root, "sub/fixture.txt:stream", active); }),
               "Alternate stream denied");
        std::ofstream(base / L"outside" / L"secret.txt") << "outside";
        expect(CreateHardLinkW((base / L"root" / L"hard.txt").c_str(),
                               (base / L"outside" / L"secret.txt").c_str(), nullptr) != 0,
               "Create hardlink fixture");
        expect(rejects([&] { readAllowed(root, "hard.txt", active); }), "Hardlink denied");
        expect(CreateSymbolicLinkW((base / L"root" / L"link").c_str(), (base / L"outside").c_str(),
                                   SYMBOLIC_LINK_FLAG_DIRECTORY |
                                       SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0,
               "Create reparse fixture (runner requires developer symlinks)");
        expect(rejects([&] { readAllowed(root, "link/secret.txt", active); }),
               "Reparse directory read denied");
        expect(rejects([&] { writeAllowed(root, "link", "new.txt", {}, active); }),
               "Reparse directory write denied");
        expect(
            rejects([&] { readAllowed(utf8((base / L"root" / L"link").wstring()), "secret.txt", active); }),
            "Configured reparse root denied");
        active = false;
        expect(rejects([&] { writeAllowed(root, "", "cancelled.txt", {'a'}, active); }),
               "Cancelled upload denied");
        expect(!std::filesystem::exists(base / L"root" / L"cancelled.txt"), "Cancelled partial file deleted");
        active = true;
        auto result = runCommand("echo missioncontrol-fixture & exit /b 7", active);
        expect(result.output.find("missioncontrol-fixture") != std::string::npos && result.exitCode == 7,
               "Runner-only command captures output and exit code");
        auto capped = runCommand(
            "for /L %i in (1,1,100000) do @echo 01234567890123456789012345678901234567890123456789", active);
        expect(capped.truncated && capped.output.size() <= CommandPolicy::outputCap,
               "Output cap kills command job");
        auto timeout = runCommand("ping -n 30 127.0.0.1 >nul", active);
        expect(timeout.timedOut, "Command timeout kills job");
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            active = false;
        });
        auto cancelled = runCommand("ping -n 30 127.0.0.1 >nul", active);
        cancel.join();
        expect(cancelled.cancelled, "Stop cancels command job");
        std::filesystem::remove_all(base);
        std::cout << checks
                  << " Windows safety checks passed; fixtures only, no Telegram or desktop capture\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED: " << e.what() << " Windows error=" << GetLastError() << "\n";
        if (!base.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }
        return 1;
    }
}
