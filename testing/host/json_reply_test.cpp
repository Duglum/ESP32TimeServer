// ESP32 Time Server host tests (the json_reply test of ESP32-WebTherm's output_test, a3fb9aa): the short answers of the
// web API written without allocation (main/core/json_reply.cpp, json_writer.cpp), byte for byte as cJSON prints them
#define TEST_NAME "json_reply_test"
#include "check.h"

#include "core/json_reply.h"
#include "cJSON.h"

#include <memory>
#include <random>
#include <string>

namespace
{
    // json_reply_message(): as cJSON_PrintUnformatted() prints the same tree, or false
    std::string cjson_reply(bool ok, const char *message)
    {
        cJSON *root = cJSON_CreateObject();
        if (ok)
        {
            cJSON_AddBoolToObject(root, "ok", true);
            if (message != nullptr)
                cJSON_AddStringToObject(root, "message", message);
        }
        else
            cJSON_AddStringToObject(root, "error", message);
        char *text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        std::string result = text != nullptr ? text : "";
        cJSON_free(text);
        return result;
    }

    void check_reply(bool ok, const char *message)
    {
        char out[512];
        const bool written = json_reply_message(out, sizeof(out), ok, message);
        const bool b_or_f = message != nullptr && strpbrk(message, "\b\f") != nullptr;
        CHECK(written == !b_or_f);
        if (written)
            CHECK(cjson_reply(ok, message) == out);
    }
} // namespace

void run_tests()
{
    char out[64];
    CHECK(json_reply_message(out, sizeof(out), true, nullptr));
    CHECK_STR(out, "{\"ok\":true}");
    CHECK(json_reply_message(out, sizeof(out), true, "Token deleted."));
    CHECK_STR(out, "{\"ok\":true,\"message\":\"Token deleted.\"}");
    CHECK(json_reply_message(out, sizeof(out), false, "Out of memory"));
    CHECK_STR(out, "{\"error\":\"Out of memory\"}");
    check_reply(true, nullptr);
    check_reply(true, "");
    check_reply(false, "");
    check_reply(false, "Stored security data of the device was lost: logins are refused. Recovery: erase NVS over the USB port (see the README)");
    // Every byte as the whole message and between two letters
    for (int c = 1; c < 256; ++c)
    {
        const char one[2] = {static_cast<char>(c), 0};
        const char three[4] = {'a', static_cast<char>(c), 'z', 0};
        for (const bool ok : {true, false})
        {
            check_reply(ok, one);
            check_reply(ok, three);
        }
    }
    // Random texts of every byte but NUL
    std::mt19937 random(16);
    for (int i = 0; i < 3000; ++i)
    {
        std::string text(random() % 80, 'x');
        for (char &c : text)
            c = static_cast<char>(1 + random() % 255);
        check_reply((i & 1) != 0, text.c_str());
    }
    // A buffer too small: false, never beyond it (ASan); just large enough: the text
    const std::string full = cjson_reply(true, "Settings saved.");
    for (size_t size = 0; size <= full.size() + 1; ++size)
    {
        std::unique_ptr<char[]> buffer(new char[size + 1]);
        const bool written = json_reply_message(buffer.get(), size, true, "Settings saved.");
        CHECK(written == (size > full.size()));
        if (written)
            CHECK(full == buffer.get());
    }
}
