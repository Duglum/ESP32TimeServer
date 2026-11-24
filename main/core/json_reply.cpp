// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)

#include "core/json_reply.h"
#include "core/json_writer.h"

#include <cstring>

bool json_reply_message(char *out, size_t size, bool ok, const char *message)
{
    if (message != nullptr && strpbrk(message, "\b\f") != nullptr)
        return false;
    json_writer json(out, size);
    json.begin_object();
    if (ok)
    {
        json.member_bool("ok", true);
        if (message != nullptr)
            json.member("message", message);
    }
    else
        json.member("error", message);
    json.end_object();
    return json.ok();
}
