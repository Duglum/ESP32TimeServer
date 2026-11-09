// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The RADIUS client task ("radius"): checks user names and passwords for the web page, so that the waits for a server
// (up to servers x (1 + retries) x timeout) never hold up the web server task. The time server has no PSRAM: the task
// and its memory (about 8 kB for the jobs and the datagram buffer, 6 kB stack) are only taken once RADIUS is needed
// (a login method with RADIUS, or the test of the page), then they stay.
//
// A login is handed over with radius_submit() and its callback is called exactly once, in the radius task, with the
// outcome. At most RADIUS_ACTIVE_MAX logins are asked at the same time, RADIUS_JOBS_MAX wait in all, and at most one
// per source address (a second one from the same address is refused as busy: parallel guesses would get around the
// login throttling, which counts a failure only when its answer is there).
//
// One UDP socket per server, connected to it (answers from other addresses are not seen; the local address of the
// socket is the NAS-IP-Address / NAS-IPv6-Address unless set). A DNS name is looked up at every login (lwIP caches).
// No secret and no password is ever logged.
#pragma once

#include <cstddef>
#include <cstdint>

#include "auth/auth_config.h"

enum class radius_result_t : uint8_t
{
    accept,            // Access-Accept with the privilege level and the required attribute
    reject,            // Access-Reject (or an Access-Challenge: not supported)
    low_privilege,     // Access-Accept below the privilege level of the settings
    missing_attribute, // Access-Accept without the required attribute
    no_answer,         // no server answered (or every one is dead, or none is set)
    busy,              // too many logins at once, or one from this source is still asked
};
const char *radius_result_text(radius_result_t result);

struct radius_outcome_t
{
    radius_result_t result;
    uint8_t server;      // 0 or 1: the server that answered
    int privilege;       // of an Access-Accept
    uint32_t rtt_ms;
    bool challenge;      // the answer was an Access-Challenge
    char summary[RADIUS_SUMMARY_SIZE]; // the attributes of the answer (the test of the page)
};

using radius_done_t = void (*)(void *context, uint32_t ticket, const radius_outcome_t &outcome);

struct radius_login_t
{
    char user[RADIUS_USER_MAX + 1];
    char password[RADIUS_PASSWORD_MAX + 1];
    uint8_t source[16];          // the client, IPv4 as IPv4-mapped IPv6 (Calling-Station-Id, one login per source)
    uint32_t nas_port;           // 0 = the web page
    char nas_port_id[8];         // "https"
    bool test;                   // the test of the page: not limited per source
    const radius_settings_t *settings; // the settings to use; nullptr = those in use (radius_task_apply)
};

// The web server has 3 sockets: more logins than that cannot wait at the same time
static constexpr size_t RADIUS_ACTIVE_MAX = 2;
static constexpr size_t RADIUS_JOBS_MAX = 4;

// Starts the task (once; later calls return true at once). false: no memory
bool radius_task_start(const radius_settings_t &settings, const char *hostname);
// The task runs
bool radius_task_running();
// Stack of the task in bytes and its least free bytes since the start (Diagnostics); false while it does not run
bool radius_task_stack(uint32_t &size, uint32_t &least_free);
// The settings in use (Apply): every server is asked again (a dead mark is forgotten); a change of a server's address
// forgets what is known of it
void radius_task_apply(const radius_settings_t &settings, const char *hostname);
// Hands a login over. Returns its ticket (never 0); 0 = refused (busy, or the task is not running): then the
// callback is not called. The password is cleared in login (it is copied).
uint32_t radius_submit(radius_login_t &login, radius_done_t done, void *context);

// Status ("radius" in /api/state)
struct radius_server_status_t
{
    bool used;
    char host[AUTH_RADIUS_HOST_SIZE];
    char address[46];   // the address last used ("" before the first login)
    uint16_t port;
    bool dead;
    uint32_t dead_for_s;
    int64_t last_answer_ago_s; // -1 = none
    uint32_t last_rtt_ms;
    uint32_t requests;
    uint32_t accepts;
    uint32_t rejects;
    uint32_t timeouts;
    uint32_t dropped;
};
struct radius_status_t
{
    bool running;
    radius_server_status_t server[RADIUS_SERVERS];
    uint32_t accepted;
    uint32_t rejected;
    uint32_t low_privilege; // refused by the privilege level or the required attribute
    uint32_t no_answer;
    uint32_t busy;
    uint32_t active;        // logins being asked now
};
void radius_task_status(radius_status_t &out);
