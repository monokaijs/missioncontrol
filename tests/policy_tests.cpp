#include "policy.hpp"
#include <functional>
#include <iostream>
using namespace mc;
int checks = 0;
void expect(bool condition, const char *reason) {
    ++checks;
    if (!condition)
        throw std::runtime_error(reason);
}
bool rejects(const std::function<void()> &fn) {
    try {
        fn();
        return false;
    } catch (...) {
        return true;
    }
}
json message(int64_t user = 101, int64_t chat = 101, std::string type = "private",
             std::string text = "/status") {
    return {{"update_id", 1},
            {"message",
             {{"date", 100},
              {"from", {{"id", user}, {"is_bot", false}}},
              {"chat", {{"id", chat}, {"type", type}}},
              {"text", text}}}};
}
json callback(int64_t user = 101, int64_t chat = 101, std::string type = "private",
              std::string action = "status") {
    return {{"update_id", 2},
            {"callback_query",
             {{"id", "callback-test"},
              {"from", {{"id", user}, {"is_bot", false}}},
              {"message", {{"date", 100}, {"chat", {{"id", chat}, {"type", type}}}}},
              {"data", action}}}};
}
int main() {
    try {
        Config c;
        c.users = {101, 202};
        c.chats = {101, 303};
        for (auto [feature, label] : features) {
            (void)label;
            expect(!c.enabled(feature), "Every permission defaults off");
        }
        expect(!route(message(), c), "Disabled status rejected");
        c.permissions[Feature::Status] = true;
        expect(bool(route(message(), c)), "Authorized message accepted");
        expect(bool(route(callback(), c)), "Authorized callback accepted");
        for (auto user : {0, 99, 303}) {
            expect(!route(message(user), c), "Unauthorized user rejected");
            expect(!route(callback(user), c), "Unauthorized callback user rejected");
        }
        for (auto chat : {0, -101, 999}) {
            expect(!route(message(101, chat), c), "Unauthorized chat rejected");
            expect(!route(callback(101, chat), c), "Unauthorized callback chat rejected");
        }
        for (auto type : {"group", "supergroup", "channel", ""}) {
            expect(!route(message(101, 101, type), c), "Non-private message rejected");
            expect(!route(callback(101, 101, type), c), "Non-private callback rejected");
        }
        auto m = message();
        m["message"]["from"]["is_bot"] = true;
        expect(!route(m, c), "Bot sender rejected");
        m["message"].erase("from");
        expect(!route(m, c), "Missing sender rejected");
        auto cb = callback();
        cb["callback_query"]["message"]["date"] = 0;
        expect(!route(cb, c), "Inaccessible callback rejected");
        cb = callback();
        cb["callback_query"]["inline_message_id"] = "inline";
        expect(!route(cb, c), "Inline callback rejected");
        cb["callback_query"].erase("message");
        expect(!route(cb, c), "Callback without chat rejected");
        expect(!route(json{{"edited_message", message()["message"]}}, c), "Edited messages ignored");
        expect(!route(json{{"channel_post", message()["message"]}}, c), "Channel posts ignored");
        expect(!route(message(101, 101, "private", "/exec echo hello"), c), "Shell default off");
        c.permissions[Feature::Shell] = true;
        expect(bool(route(message(101, 101, "private", "/exec echo hello"), c)), "Opt-in shell available");
        expect(!route(message(999, 101, "private", "/exec echo hello"), c), "Shell still checks user");
        auto doc = message(101, 101, "private", "");
        doc["message"].erase("text");
        doc["message"]["caption"] = "/put 1";
        doc["message"]["document"] = {{"file_id", "fixture"}, {"file_name", "safe.txt"}};
        expect(!route(doc, c), "Upload default off");
        c.permissions[Feature::Download] = true;
        expect(!route(doc, c), "Download does not grant upload");
        c.permissions[Feature::Upload] = true;
        expect(bool(route(doc, c)), "Opt-in document accepted");
        doc["message"]["from"]["id"] = 999;
        expect(!route(doc, c), "Document checks user");
        doc["message"]["from"]["id"] = 101;
        doc["message"]["chat"]["id"] = 999;
        expect(!route(doc, c), "Document checks chat");
        doc["message"]["chat"] = {{"id", 101}, {"type", "group"}};
        expect(!route(doc, c), "Document checks private chat");
        expect(!route(message(101, 101, "private", "/put 1"), c), "Upload requires document");
        Confirmations confirm;
        auto now = Confirmations::Time{};
        std::string n(32, 'a'), n2(32, 'b');
        confirm.issue(n, {101, 101}, "shutdown", now);
        expect(!confirm.consume(n, {202, 101}, now), "Confirmation bound to user");
        expect(!confirm.consume(n, {101, 303}, now), "Confirmation bound to chat");
        expect(confirm.consume(n, {101, 101}, now) == "shutdown", "Confirmation returns original action");
        expect(!confirm.consume(n, {101, 101}, now), "Confirmation single use");
        confirm.issue(n, {101, 101}, "restart", now);
        expect(!confirm.consume(n, {101, 101}, now + std::chrono::seconds(30)),
               "Confirmation expires exactly at deadline");
        confirm.issue(n, {101, 101}, "shutdown", now);
        confirm.issue(n2, {101, 101}, "restart", now);
        expect(!confirm.consume(n, {101, 101}, now), "New confirmation invalidates old");
        confirm.clear();
        expect(!confirm.consume(n2, {101, 101}, now), "Stop clears confirmations");
        expect(rejects([&] { confirm.issue("short", {101, 101}, "restart", now); }), "Nonce length enforced");
        for (auto path : {"../x",     "a/../x", "/absolute", "C:\\Windows", "\\\\server\\share",
                          "a//b",     "a/",     "a\\..\\b",  "CON",         "nul.txt",
                          "COM1.foo", "LPT9",   "x:stream",  "x.",          "x ",
                          " a",       "a\nb",   "?",         "*",           "a\"b",
                          "."})
            expect(rejects([&] { components(path); }), "Unsafe path rejected");
        expect(components("folder/sub file.txt").size() == 2, "Relative subpath allowed");
        expect(components("资料/report.txt").size() == 2, "Unicode path allowed");
        expect(components("", true).empty(), "Root directory upload allowed");
        for (auto root : {"C:\\", "relative", "C:relative", "\\\\host\\share", "\\\\?\\C:\\folder",
                          "C:\\folder\\..\\escape"})
            expect(rejects([&] { rootComponents(root); }), "Unsafe root rejected");
        expect(rootComponents("C:\\RemoteFiles").size() == 1, "Dedicated absolute root allowed");
        expect(fileRequest("1 folder/file.txt", 2).root == 0, "Root number maps to configured root");
        expect(fileRequest("2", 2, true).relative.empty(), "Upload to root supported");
        for (auto req : {"0 a", "-1 a", "3 a", "x a", "1 ../a", "1 C:\\x"})
            expect(rejects([&] { fileRequest(req, 2); }), "Invalid file request rejected");
        expect(CommandPolicy::valid("echo hello && echo world"), "Explicit shell syntax accepted");
        expect(!CommandPolicy::valid(""), "Empty command rejected");
        expect(!CommandPolicy::valid(std::string(4097, 'x')), "Oversized command rejected");
        expect(!CommandPolicy::valid("echo a\necho b"), "Multiline command rejected");
        expect(!CommandPolicy::valid(std::string("a\0b", 3)), "NUL rejected");
        expect(CommandPolicy::timeoutMs == 15000 && CommandPolicy::outputCap == 65536,
               "Command bounds enforced");
        int calls = 0;
        auto mock = [&](const json &u) {
            if (route(u, c))
                ++calls;
        };
        mock(message(999));
        mock(callback(999));
        mock(doc);
        expect(calls == 0, "Denied updates never reach action mock");
        mock(message());
        expect(calls == 1, "Authorized update reaches action mock once");
        std::cout << checks << " security policy checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
