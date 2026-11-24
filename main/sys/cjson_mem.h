// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE). From ESP32-WebTherm (sys/cjson_mem.h of a3fb9aa), adapted.
//
// Memory of cJSON: malloc() as before, zeroed before it is freed (sys/secret.h), and a note when an allocation fails,
// so that the web server does not send a tree with a member missing as a complete answer.
//
// cJSON gives back nullptr where a node or a printed text could not be allocated, and the functions that add a
// member to an object (cJSON_Add...ToObject) drop that member: a caller that does not check every call builds an
// answer without it. Every allocation of cJSON goes through the function of this file; one that fails in the
// watched task is noted. Only one task is watched at a time: the web server task, which calls
// cjson_mem_watch_begin() before each request and each queued answer (web_server.cpp). In this firmware only the web
// server uses cJSON (its handlers, auth/auth_config.cpp called by them, the RADIUS answers built in a work item of
// the same task); a failure in another task would not be noted and so could not mark an answer of the web server.
#pragma once

// Sets the hooks of cJSON (once, at the start, before the first use of cJSON)
void cjson_mem_init();

// From now on the allocations of cJSON in the calling task are watched, and none has failed yet
void cjson_mem_watch_begin();

// True when an allocation of cJSON failed in the calling task since its last cjson_mem_watch_begin()
bool cjson_mem_failed();
