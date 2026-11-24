// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// JSON text into a fixed buffer, see json_writer.h

#include "json_writer.h"

#include <cstdio>

json_writer::json_writer(char *buffer, size_t size) : buffer_(buffer), size_(size)
{
    if (size_ == 0)
        overflow_ = true;
    else
        buffer_[0] = '\0';
}

void json_writer::put(char c)
{
    if (overflow_)
        return;
    if (length_ + 1 >= size_)
    {
        overflow_ = true;
        return;
    }
    buffer_[length_++] = c;
    buffer_[length_] = '\0';
}

void json_writer::put(const char *text)
{
    while (*text != '\0' && !overflow_)
        put(*text++);
}

void json_writer::separator()
{
    if (after_key_)
    {
        after_key_ = false;
        return;
    }
    if (depth_ > 0)
    {
        if (!first_[depth_ - 1])
            put(',');
        first_[depth_ - 1] = false;
    }
}

void json_writer::begin_object()
{
    separator();
    put('{');
    if (depth_ < MAX_DEPTH)
        first_[depth_++] = true;
    else
        overflow_ = true;
}

void json_writer::end_object()
{
    put('}');
    if (depth_ > 0)
        --depth_;
}

void json_writer::begin_array()
{
    separator();
    put('[');
    if (depth_ < MAX_DEPTH)
        first_[depth_++] = true;
    else
        overflow_ = true;
}

void json_writer::end_array()
{
    put(']');
    if (depth_ > 0)
        --depth_;
}

void json_writer::key(const char *name)
{
    separator();
    put_string(name);
    put(':');
    after_key_ = true;
}

void json_writer::string(const char *text)
{
    separator();
    put_string(text);
}

void json_writer::put_string(const char *text)
{
    put('"');
    for (const char *p = text == nullptr ? "" : text; *p != '\0'; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\')
        {
            put('\\');
            put(static_cast<char>(c));
        }
        else if (c == '\n' || c == '\r' || c == '\t')
        {
            put('\\');
            put(c == '\n' ? 'n' : c == '\r' ? 'r' : 't');
        }
        else if (c < 0x20)
        {
            char escaped[8];
            snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            put(escaped);
        }
        else
            put(static_cast<char>(c));
    }
    put('"');
}

void json_writer::number(int64_t value)
{
    separator();
    char text[24];
    snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
    put(text);
}

void json_writer::fixed(int64_t value, int decimals)
{
    separator();
    if (decimals < 0)
        decimals = 0;
    if (decimals > 3)
        decimals = 3;
    int64_t scale = 1;
    for (int i = 0; i < decimals; ++i)
        scale *= 10;
    const uint64_t magnitude = value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);
    char text[32];
    if (decimals == 0)
        snprintf(text, sizeof(text), "%s%llu", value < 0 ? "-" : "", static_cast<unsigned long long>(magnitude));
    else
        snprintf(text, sizeof(text), "%s%llu.%0*llu", value < 0 ? "-" : "", static_cast<unsigned long long>(magnitude / scale),
                 decimals, static_cast<unsigned long long>(magnitude % scale));
    put(text);
}

void json_writer::boolean(bool value)
{
    separator();
    put(value ? "true" : "false");
}

void json_writer::null()
{
    separator();
    put("null");
}
