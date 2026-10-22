#pragma once
#include "platform.hpp"
#include <mutex>
#include <thread>
#include <winhttp.h>
namespace mc {
class Telegram {
    Config config;
    std::function<void(std::string)> report;
    std::thread worker;
    std::mutex httpMutex;
    HINTERNET active{};
    HINTERNET session{}, connection{};
    Confirmations confirmations;
    struct WindowRef {
        Window value;
        std::chrono::steady_clock::time_point expires;
    };
    std::map<std::string, WindowRef> windowRefs;
    struct Output {
        Identity who;
        std::string data;
        std::chrono::steady_clock::time_point expires;
    };
    std::map<std::string, Output> outputs;
    std::vector<unsigned char> request(const std::wstring &path, const std::string &contentType,
                                       const std::vector<unsigned char> &body,
                                       size_t limit = 2 * 1024 * 1024);
    json api(const std::string &method, json body);
    void document(int64_t chat, const std::string &name, const std::vector<unsigned char> &bytes,
                  const std::string &caption);
    void message(int64_t chat, const std::string &text, json buttons = json::array());
    void menu(int64_t chat);
    void handle(const json &update);
    void loop();
    void log(const std::string &text);

  public:
    std::atomic_bool running{false};
    Telegram(Config c, std::function<void(std::string)> callback)
        : config(std::move(c)), report(std::move(callback)) {}
    ~Telegram() {
        stop();
    }
    void start();
    void stop();
};
} // namespace mc
