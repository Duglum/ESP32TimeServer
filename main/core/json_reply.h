// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The short answers of the web API without allocation: {"ok":true}, {"ok":true,"message":"..."} and
// {"error":"..."}, byte for byte as cJSON_PrintUnformatted() prints the same tree (host tested against cJSON). So
// that the answer of a request that has already acted (a password changed, a token deleted, a restart scheduled)
// does not depend on free memory, and an error answer is not lost for lack of it.

#pragma once

#include <cstddef>

// ok: {"ok":true} and, when message is not nullptr, "message"; else {"error":message} (nullptr: an empty text).
// False when the text does not fit size, or the message has a backspace or a form feed (cJSON escapes them as \b
// and \f, json_writer as \u0008 and \u000c): the caller builds that answer with cJSON.
bool json_reply_message(char *out, size_t size, bool ok, const char *message);
