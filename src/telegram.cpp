#include "telegram.hpp"
#include <ctime>
namespace mc {
static std::vector<unsigned char> bytes(const std::string &s) {
    return {s.begin(), s.end()};
}
static json button(const std::string &title, const std::string &action) {
    return json{{"text", title}, {"callback_data", action}};
}
void Telegram::log(const std::string &text) {
    audit(text);
    report(text);
}
void Telegram::start() {
    if (worker.joinable())
        return;
    running = true;
    worker = std::thread([this] { loop(); });
}
void Telegram::stop() {
    running = false;
    {
        std::lock_guard lock(httpMutex);
        if (active) {
            WinHttpCloseHandle(active);
            active = nullptr;
        }
    }
    if (worker.joinable())
        worker.join();
    confirmations.clear();
    windowRefs.clear();
    outputs.clear();
}
std::vector<unsigned char> Telegram::request(const std::wstring &path, const std::string &contentType,
                                             const std::vector<unsigned char> &body, size_t limit) {
    HINTERNET req{};
    {
        std::lock_guard lock(httpMutex);
        if (!running)
            throw std::runtime_error("Stopped");
        req = WinHttpOpenRequest(connection, path.starts_with(L"/file/") ? L"GET" : L"POST", path.c_str(),
                                 nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
        if (!req)
            throw std::runtime_error("Cannot create HTTPS request");
        active = req;
    }
    auto close = [&] {
        std::lock_guard lock(httpMutex);
        if (active == req) {
            WinHttpCloseHandle(req);
            active = nullptr;
        }
    };
    try {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
        std::wstring headers = L"Content-Type: " + wide(contentType) + L"\r\n";
        if (!WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(headers.size()),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA
                                             : const_cast<unsigned char *>(body.data()),
                                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
            !WinHttpReceiveResponse(req, nullptr))
            throw std::runtime_error("HTTPS request failed or was cancelled");
        DWORD status{}, size = sizeof status;
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
                                 &size, nullptr))
            throw std::runtime_error("HTTPS status unavailable");
        if (status != 200) {
            if (status == 401)
                running = false;
            throw std::runtime_error("Telegram HTTP status " + std::to_string(status) +
                                     " (check token, bot ownership, network or rate limits)");
        }
        std::vector<unsigned char> result;
        for (;;) {
            if (!running)
                throw std::runtime_error("Stopped");
            DWORD available{};
            if (!WinHttpQueryDataAvailable(req, &available))
                throw std::runtime_error("HTTPS read failed");
            if (!available)
                break;
            if (result.size() + available > limit)
                throw std::runtime_error("Response exceeds size cap");
            auto offset = result.size();
            result.resize(offset + available);
            DWORD got{};
            if (!WinHttpReadData(req, result.data() + offset, available, &got))
                throw std::runtime_error("HTTPS read failed");
            result.resize(offset + got);
            if (!got)
                break;
        }
        close();
        return result;
    } catch (...) {
        close();
        throw;
    }
}
json Telegram::api(const std::string &method, json body) {
    auto data =
        request(L"/bot" + wide(config.token) + L"/" + wide(method), "application/json", bytes(body.dump()));
    auto response = json::parse(data);
    if (!response.value("ok", false))
        throw std::runtime_error("Telegram API rejected request");
    return response.at("result");
}
void Telegram::message(int64_t chat, const std::string &text, json buttons) {
    if (!running)
        return;
    json body{{"chat_id", chat}, {"text", text}, {"protect_content", true}};
    if (!buttons.empty())
        body["reply_markup"] = {{"inline_keyboard", buttons}};
    api("sendMessage", body);
}
void Telegram::document(int64_t chat, const std::string &name, const std::vector<unsigned char> &data,
                        const std::string &caption) {
    if (!running)
        return;
    if (data.size() > 20 * 1024 * 1024)
        throw std::runtime_error("Transfer exceeds 20 MiB cap");
    std::string boundary = "MissionControl" + nonce();
    std::string prefix = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
                         std::to_string(chat) + "\r\n--" + boundary +
                         "\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n--" +
                         boundary +
                         "\r\nContent-Disposition: form-data; name=\"protect_content\"\r\n\r\ntrue\r\n--" +
                         boundary + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"";
    // Never put an untrusted filename in multipart headers.
    std::string safe;
    for (unsigned char c : name) {
        if (c < 128 && (std::isalnum(c) || c == '.' || c == '_' || c == '-'))
            safe += static_cast<char>(c);
        else
            safe += '_';
    }
    if (safe.empty())
        safe = "download.bin";
    prefix += safe + "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    auto body = bytes(prefix);
    body.insert(body.end(), data.begin(), data.end());
    auto suffix = bytes("\r\n--" + boundary + "--\r\n");
    body.insert(body.end(), suffix.begin(), suffix.end());
    auto result = request(L"/bot" + wide(config.token) + L"/sendDocument",
                          "multipart/form-data; boundary=" + boundary, body);
    if (!json::parse(result).value("ok", false))
        throw std::runtime_error("Telegram rejected document");
}
void Telegram::menu(int64_t chat) {
    json rows = json::array();
    if (config.enabled(Feature::Status))
        rows.push_back(json::array({button("System status", "status")}));
    if (config.enabled(Feature::Capture))
        rows.push_back(json::array({button("Desktop screenshot", "screen")}));
    if (config.enabled(Feature::Windows))
        rows.push_back(json::array({button("Windows / capture / close", "windows")}));
    if (config.enabled(Feature::Audio)) {
        rows.push_back(json::array(
            {button("Volume -", "volume down"), button("Volume +", "volume up"), button("Mute", "mute")}));
    }
    if (config.enabled(Feature::Lock))
        rows.push_back(json::array({button("Lock Windows", "lock")}));
    if (config.enabled(Feature::Power))
        rows.push_back(json::array({button("Shutdown...", "shutdown"), button("Restart...", "restart")}));
    if (config.enabled(Feature::Download) || config.enabled(Feature::Upload))
        rows.push_back(json::array({button("Allowed file roots", "roots")}));
    message(chat,
            "MissionControl is online. Only locally allowed users AND private chats can act.\n/menu - "
            "controls\n/exec <command> - opt-in shell, 15s / 64 KiB\n/get <root-number> <relative-file> - "
            "download\nSend a document with caption /put <root-number> [relative-folder] - upload (new files "
            "only)\n/volume 0..100\nStop locally to disconnect and cancel commands.",
            rows);
}
static std::string commandText(const std::string &raw) {
    if (raw.empty())
        return "(no output)";
    int n = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (!n)
        return "(binary output; request document)";
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), w.data(), n);
    for (auto &c : w)
        if (c == 0)
            c = L' ';
    return utf8(w);
}
void Telegram::handle(const json &update) {
    if (!running)
        return;
    auto id = identity(update, config);
    if (!id) {
        log("Rejected unauthorized or unsupported update");
        return;
    }
    // Command output documents use short-lived, peer-bound capabilities and Shell permission.
    if (update.contains("callback_query")) {
        const auto &q = update.at("callback_query");
        auto value = q.value("data", std::string{});
        if (value.starts_with("output ")) {
            if (!config.enabled(Feature::Shell))
                return;
            auto i = outputs.find(value.substr(7));
            if (i == outputs.end() || i->second.who.user != id->user || i->second.who.chat != id->chat ||
                i->second.expires <= std::chrono::steady_clock::now())
                return;
            api("answerCallbackQuery", {{"callback_query_id", q.at("id")}});
            auto data = std::move(i->second.data);
            outputs.erase(i);
            log("Authorized command output document user=" + std::to_string(id->user));
            document(id->chat, "command-output.txt", bytes(data), "Captured shell output (bounded)");
            return;
        }
    }
    auto intent = route(update, config);
    if (!intent) {
        log("Rejected disabled feature or invalid request user=" + std::to_string(id->user));
        return;
    }
    auto &in = *intent;
    if (in.callback)
        api("answerCallbackQuery", {{"callback_query_id", in.callbackId}});
    if (!running)
        return;
    log("Authorized " + in.verb + " user=" + std::to_string(in.who.user) +
        " chat=" + std::to_string(in.who.chat));
    auto now = std::chrono::steady_clock::now();
    for (auto i = windowRefs.begin(); i != windowRefs.end();)
        if (i->second.expires <= now)
            i = windowRefs.erase(i);
        else
            ++i;
    for (auto i = outputs.begin(); i != outputs.end();)
        if (i->second.expires <= now)
            i = outputs.erase(i);
        else
            ++i;
    try {
        if (in.verb == "menu")
            menu(in.who.chat);
        else if (in.verb == "status")
            message(in.who.chat, systemStatus());
        else if (in.verb == "screen")
            document(in.who.chat, "desktop.png", screenshot(), "Desktop screenshot");
        else if (in.verb == "windows") {
            auto list = windows();
            windowRefs.clear();
            std::string text = "Visible windows (buttons expire in 2 minutes):\n";
            json rows = json::array();
            size_t index{};
            for (auto &w : list) {
                auto token = nonce();
                windowRefs[token] = {w, now + std::chrono::minutes(2)};
                text += std::to_string(++index) + ". " + w.title.substr(0, [&] {
                    size_t n = std::min<size_t>(70, w.title.size());
                    while (n < w.title.size() && n > 0 &&
                           (static_cast<unsigned char>(w.title[n]) & 0xc0) == 0x80)
                        --n;
                    return n;
                }()) + "\n";
                json row = json::array();
                if (config.enabled(Feature::Capture))
                    row.push_back(button("Capture " + std::to_string(index), "shot " + token));
                row.push_back(button("Close " + std::to_string(index), "close " + token));
                rows.push_back(row);
            }
            if (list.empty())
                text += "No visible windows.";
            message(in.who.chat, text, rows);
        } else if (in.verb == "shot" || in.verb == "close") {
            auto i = windowRefs.find(in.arg);
            if (i == windowRefs.end())
                throw std::runtime_error("Window selection expired. Request /windows again.");
            auto w = i->second.value;
            DWORD pid{};
            GetWindowThreadProcessId(w.hwnd, &pid);
            if (pid != w.pid || !IsWindow(w.hwnd))
                throw std::runtime_error("Selected window is no longer available");
            if (in.verb == "shot")
                document(in.who.chat, "window.png", screenshot(w.hwnd),
                         "Selected window bounds (visible pixels; overlapping windows may appear)");
            else {
                windowRefs.erase(i);
                if (!closeWindow(w))
                    throw std::runtime_error("Window refused graceful close or is no longer available");
                message(in.who.chat, "Sent graceful close. The app may ask locally to save work.");
            }
        } else if (in.verb == "volume" || in.verb == "mute")
            message(in.who.chat, audio(in.verb == "mute" ? "mute" : in.arg));
        else if (in.verb == "lock") {
            if (!LockWorkStation())
                throw std::runtime_error("Windows refused lock request");
            message(in.who.chat, "Workstation lock requested.");
        } else if (in.verb == "shutdown" || in.verb == "restart") {
            auto token = nonce();
            confirmations.issue(token, in.who, in.verb, now);
            message(in.who.chat,
                    "Confirm " + in.verb +
                        "? Unsaved work may be lost. This one-time confirmation expires in 30 seconds.",
                    json::array({json::array(
                        {button("Confirm " + in.verb, "confirm " + token), button("Cancel", "cancel")})}));
        } else if (in.verb == "cancel") {
            confirmations.clear();
            message(in.who.chat, "Pending power confirmations cancelled.");
        } else if (in.verb == "confirm") {
            auto action = confirmations.consume(in.arg, in.who, now);
            if (!action)
                throw std::runtime_error(
                    "Confirmation expired, already used, or belongs to another user/chat");
            if (!running)
                return;
            log("Executing confirmed " + *action + " user=" + std::to_string(in.who.user));
            power(*action == "restart");
        } else if (in.verb == "roots") {
            std::string text = "Configured local file roots:\n";
            for (size_t i = 0; i < config.roots.size(); ++i)
                text += std::to_string(i + 1) + ". " + config.roots[i] + "\n";
            if (config.roots.empty())
                text += "None. Configure folders in the local GUI.";
            message(in.who.chat, text);
        } else if (in.verb == "get") {
            auto f = fileRequest(in.arg, config.roots.size());
            auto data = readAllowed(config.roots[f.root], f.relative, running);
            auto parts = components(f.relative);
            document(in.who.chat, parts.back(), data,
                     "Requested file from allowed root " + std::to_string(f.root + 1));
        } else if (in.verb == "put") {
            auto f = fileRequest(in.arg, config.roots.size(), true);
            const auto &doc = update.at("message").at("document");
            auto name = doc.at("file_name").get<std::string>();
            if (components(name).size() != 1)
                throw std::runtime_error("Invalid document name");
            if (doc.value("file_size", int64_t(0)) > 20 * 1024 * 1024)
                throw std::runtime_error("Document exceeds 20 MiB cap");
            auto info = api("getFile", {{"file_id", doc.at("file_id")}});
            auto path = info.at("file_path").get<std::string>();
            components(path);
            auto data = request(L"/file/bot" + wide(config.token) + L"/" + wide(path),
                                "application/octet-stream", {}, 20 * 1024 * 1024);
            writeAllowed(config.roots[f.root], f.relative, name, data, running);
            message(in.who.chat, "Saved new document to allowed root " + std::to_string(f.root + 1) +
                                     ". Files are never executed.");
        } else if (in.verb == "exec") {
            auto r = runCommand(in.arg, running);
            if (!running)
                return;
            std::string summary = "Exit code: " + std::to_string(r.exitCode) +
                                  (r.timedOut ? " | timed out (15s)" : "") +
                                  (r.truncated ? " | output cap reached; process stopped" : "") +
                                  (r.cancelled ? " | cancelled" : "");
            auto text = commandText(r.output);
            json rows = json::array();
            if (text.size() > 2800) {
                auto token = nonce();
                outputs.clear();
                outputs[token] = {in.who, r.output, now + std::chrono::minutes(2)};
                rows.push_back(json::array({button("Download full captured output", "output " + token)}));
                text =
                    "Output is long. Use the button within 2 minutes to receive up to 64 KiB as a document.";
            }
            message(in.who.chat, summary + "\n" + text, rows);
        }
    } catch (const std::exception &e) {
        log("Action " + in.verb + " failed");
        if (running)
            message(in.who.chat, std::string("Action failed: ") + e.what());
    }
}
void Telegram::loop() {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        session = WinHttpOpen(L"MissionControl/" MC_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            throw std::runtime_error("Cannot initialize HTTPS");
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 30000);
        connection = WinHttpConnect(session, L"api.telegram.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection)
            throw std::runtime_error("Cannot connect to Telegram");
        api("getMe", json::object());
        auto webhook = api("getWebhookInfo", json::object());
        if (!webhook.value("url", std::string{}).empty())
            throw std::runtime_error(
                "This bot has a webhook. Use a dedicated polling bot; remove the webhook first.");
        // Drop stale queued work on each locally initiated session. Never execute offline backlog.
        int64_t offset{};
        auto old = api("getUpdates", {{"offset", -1},
                                      {"timeout", 0},
                                      {"allowed_updates", json::array({"message", "callback_query"})}});
        for (auto &u : old)
            offset = std::max(offset, u.at("update_id").get<int64_t>() + 1);
        if (offset)
            api("getUpdates", {{"offset", offset}, {"timeout", 0}, {"limit", 1}});
        log("Connected to Telegram; remote control enabled");
        for (auto chat : config.chats) {
            if (!running)
                break;
            menu(chat);
        }
        while (running) {
            try {
                auto updates =
                    api("getUpdates", {{"offset", offset},
                                       {"timeout", 20},
                                       {"limit", 20},
                                       {"allowed_updates", json::array({"message", "callback_query"})}});
                for (auto &u : updates) {
                    if (!running)
                        break;
                    auto next = u.at("update_id").get<int64_t>() + 1;
                    if (next <= offset)
                        continue;
                    offset = next;
                    try {
                        handle(u);
                    } catch (...) {
                        log("Update failed (details omitted to protect credentials)");
                    }
                }
            } catch (const std::exception &e) {
                if (!running)
                    break;
                log(e.what());
                for (int i = 0; i < 50 && running; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    } catch (const std::exception &e) {
        if (running)
            log(e.what());
    }
    running = false;
    {
        std::lock_guard lock(httpMutex);
        if (active) {
            WinHttpCloseHandle(active);
            active = nullptr;
        }
    }
    if (connection) {
        WinHttpCloseHandle(connection);
        connection = nullptr;
    }
    if (session) {
        WinHttpCloseHandle(session);
        session = nullptr;
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    log("Disconnected; remote actions stopped");
}
} // namespace mc
