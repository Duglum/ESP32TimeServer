// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// JSON text into a fixed buffer, without allocation (no ESP-IDF, host tested and fuzzed). Once the buffer is full
// every further call does nothing and ok() is false; the text is always terminated. Texts are escaped (", \, \n, \r,
// \t and the other control characters); bytes of 0x80 and above are copied as they are (the inputs are UTF-8).

#pragma once

#include <cstddef>
#include <cstdint>

class json_writer
{
public:
    json_writer(char *buffer, size_t size);

    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    // The name of the next member of an object
    void key(const char *name);

    void string(const char *text);
    void number(int64_t value);
    // value / 10^decimals with exactly that many decimals (decimals 0 to 3)
    void fixed(int64_t value, int decimals);
    void boolean(bool value);
    void null();

    // key + value in one call
    void member(const char *name, const char *text) { key(name); string(text); }
    void member_number(const char *name, int64_t value) { key(name); number(value); }
    void member_fixed(const char *name, int64_t value, int decimals) { key(name); fixed(value, decimals); }
    void member_bool(const char *name, bool value) { key(name); boolean(value); }

    bool ok() const { return !overflow_ && depth_ == 0; }
    bool overflow() const { return overflow_; }
    size_t length() const { return length_; }
    const char *text() const { return buffer_; }

private:
    static constexpr int MAX_DEPTH = 16;
    void put(char c);
    void put(const char *text);
    void put_string(const char *text); // quoted and escaped
    void separator(); // comma before a value or key when needed

    char *buffer_;
    size_t size_;
    size_t length_ = 0;
    bool overflow_ = false;
    int depth_ = 0;
    bool first_[MAX_DEPTH] = {};
    bool after_key_ = false;
};
