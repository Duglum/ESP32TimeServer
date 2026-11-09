// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//

#include "auth/radius_client.h"
#include "auth/radius_proto.h"

namespace
{
    // The first server from `from` on that has an address and is alive; RADIUS_SERVERS if none
    size_t next_alive(const radius_timing_t &t, const radius_servers_t &servers, size_t from, int64_t now_ms)
    {
        for (size_t i = from; i < RADIUS_SERVERS; ++i)
            if (t.server_used[i] && !radius_server_dead(servers.server[i], now_ms))
                return i;
        return RADIUS_SERVERS;
    }
} // namespace

bool radius_server_dead(const radius_server_state_t &s, int64_t now_ms)
{
    return s.dead_until_ms > now_ms;
}

radius_step_t radius_exchange_start(radius_exchange_t &x, const radius_timing_t &t, const radius_servers_t &servers, int64_t now_ms)
{
    x = radius_exchange_t{};
    x.started = true;
    const size_t first = next_alive(t, servers, 0, now_ms);
    if (first == RADIUS_SERVERS)
    {
        x.done = true;
        return radius_step_t::no_answer;
    }
    x.server = static_cast<uint8_t>(first);
    return radius_step_t::send_new;
}

void radius_exchange_sent(radius_exchange_t &x, const radius_timing_t &t, radius_servers_t &servers, int64_t now_ms)
{
    if (x.tries == 0)
        x.sent_ms = now_ms;
    ++x.tries;
    x.last_ms = now_ms;
    x.deadline_ms = now_ms + t.timeout_ms;
    ++servers.server[x.server].requests;
}

radius_step_t radius_exchange_tick(radius_exchange_t &x, const radius_timing_t &t, radius_servers_t &servers, int64_t now_ms)
{
    if (!x.started || x.done)
        return radius_step_t::wait;
    if (x.tries == 0) // not sent yet (the caller could not send): give up on this server like a timeout
        x.deadline_ms = now_ms;
    if (now_ms < x.deadline_ms)
        return radius_step_t::wait;
    if (x.tries > 0 && x.tries <= t.retries)
        return radius_step_t::resend;
    // This server did not answer
    radius_server_state_t &s = servers.server[x.server];
    ++s.timeouts;
    if (t.dead_time_ms > 0)
        s.dead_until_ms = now_ms + t.dead_time_ms;
    const size_t next = next_alive(t, servers, x.server + 1u, now_ms);
    if (next == RADIUS_SERVERS)
    {
        x.done = true;
        return radius_step_t::no_answer;
    }
    x.server = static_cast<uint8_t>(next);
    x.tries = 0;
    return radius_step_t::send_new;
}

void radius_exchange_answered(radius_exchange_t &x, radius_servers_t &servers, uint8_t code, int64_t now_ms)
{
    radius_server_state_t &s = servers.server[x.server];
    s.dead_until_ms = 0;
    s.last_answer_ms = now_ms;
    s.last_rtt_ms = static_cast<uint32_t>(now_ms - x.last_ms);
    if (code == RADIUS_ACCESS_ACCEPT)
        ++s.accepts;
    else
        ++s.rejects;
    x.done = true;
}

bool radius_id_take(radius_ids_t &ids, uint8_t &out)
{
    for (unsigned k = 0; k < 256; ++k)
    {
        const uint8_t id = static_cast<uint8_t>(ids.next + k);
        if (!radius_id_in_use(ids, id))
        {
            ids.used[id / 32] |= 1u << (id % 32);
            ids.next = static_cast<uint8_t>(id + 1);
            out = id;
            return true;
        }
    }
    return false;
}

void radius_id_release(radius_ids_t &ids, uint8_t id)
{
    ids.used[id / 32] &= ~(1u << (id % 32));
}

bool radius_id_in_use(const radius_ids_t &ids, uint8_t id)
{
    return (ids.used[id / 32] & (1u << (id % 32))) != 0;
}
