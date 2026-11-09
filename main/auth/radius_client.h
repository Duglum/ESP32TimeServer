// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The timing of a RADIUS login (pure, no ESP-IDF): which server is asked, when a request is sent again and when the
// login gives up, as a Cisco switch does it (radius-server timeout / retransmit / deadtime).
//
// A login asks the servers in their order. Each is tried 1 + retries times, timeout_ms apart; a server that does not
// answer any of them is "dead" for dead_time_ms (0: never) and the next server is asked. Dead servers are skipped, so
// while every server is dead a login gives up at once (the local password may then be used as the fallback). A server
// that answers is alive again. A valid answer of any kind (Accept, Reject, Challenge) ends the login; an invalid one
// is dropped and the login keeps waiting.
//
// The identifiers: one set of 256 per server socket (radius_ids_t); a login keeps its identifier and its Request
// Authenticator over the tries to one server (RFC 2865 2.5: a retransmission is the same packet) and gets new ones for
// the next server.
//
// Not thread safe: the RADIUS task owns all of it.
#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t RADIUS_SERVERS = 2;
constexpr uint32_t RADIUS_TIMEOUT_MIN_S = 1;
constexpr uint32_t RADIUS_TIMEOUT_MAX_S = 30;
constexpr uint32_t RADIUS_RETRIES_MAX = 5;
constexpr uint32_t RADIUS_DEAD_TIME_MAX_MIN = 60;

struct radius_timing_t
{
    bool server_used[RADIUS_SERVERS]; // an address is set
    uint32_t timeout_ms;
    uint32_t retries;
    uint32_t dead_time_ms;
};

// What the device knows about a server (Status)
struct radius_server_state_t
{
    int64_t dead_until_ms;   // > now: skipped
    int64_t last_answer_ms;  // 0 = none yet
    uint32_t last_rtt_ms;
    uint32_t requests;       // packets sent (retransmissions included)
    uint32_t accepts;
    uint32_t rejects;        // Access-Reject and Access-Challenge
    uint32_t timeouts;       // logins this server did not answer
    uint32_t dropped;        // invalid answers (radius_check_t other than ok)
};

struct radius_servers_t
{
    radius_server_state_t server[RADIUS_SERVERS];
};

bool radius_server_dead(const radius_server_state_t &s, int64_t now_ms);

enum class radius_step_t : uint8_t
{
    wait,      // nothing to do until the deadline (radius_exchange_t::deadline_ms) or an answer
    send_new,  // build a new request (new identifier and authenticator) for exchange.server and send it
    resend,    // send the same packet to exchange.server again
    no_answer, // no server answered (or every one is dead): the login fails or falls back to the local password
};

struct radius_exchange_t
{
    uint8_t server;      // the server asked now
    uint8_t tries;       // packets sent to it
    int64_t sent_ms;     // of the first packet to it (round trip time)
    int64_t last_ms;     // of the last packet to it
    int64_t deadline_ms; // of the try in flight
    bool started;
    bool done;
};

// Begins a login: send_new to the first server alive, or no_answer
radius_step_t radius_exchange_start(radius_exchange_t &x, const radius_timing_t &t, const radius_servers_t &servers, int64_t now_ms);
// Call after a send of either kind: counts the packet and sets the deadline
void radius_exchange_sent(radius_exchange_t &x, const radius_timing_t &t, radius_servers_t &servers, int64_t now_ms);
// The time passed: wait, resend, send_new (to the next server; the last one is marked dead) or no_answer
radius_step_t radius_exchange_tick(radius_exchange_t &x, const radius_timing_t &t, radius_servers_t &servers, int64_t now_ms);
// A valid answer from exchange.server with the code (radius_code_t): the login is done, the server is alive
void radius_exchange_answered(radius_exchange_t &x, radius_servers_t &servers, uint8_t code, int64_t now_ms);

// Identifiers of one server socket
struct radius_ids_t
{
    uint32_t used[8];
    uint8_t next;
};
// A free identifier (the one after the last given, so that a late answer to a finished login does not match a new
// one); false if all 256 are in use
bool radius_id_take(radius_ids_t &ids, uint8_t &out);
void radius_id_release(radius_ids_t &ids, uint8_t id);
bool radius_id_in_use(const radius_ids_t &ids, uint8_t id);
