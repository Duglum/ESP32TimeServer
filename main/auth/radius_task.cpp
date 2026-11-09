// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//

#include "auth/radius_task.h"
#include "auth/radius_client.h"
#include "auth/radius_proto.h"
#include "net/ip_text.h"
#include "web/web_server.h" // app_eth_netif()

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip6.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

namespace
{
    const char *const TAG = "radius";

    // Stack: the packet checks (MD5, HMAC-MD5 through PSA), getaddrinfo and select; the datagram buffer is static
    constexpr uint32_t STACK = 6144;
    constexpr UBaseType_t PRIORITY = 2; // below the web server (3) and every timing task
    constexpr BaseType_t CORE = 0;      // with the web server: core 1 has the hardware NTP task
    constexpr size_t PACKET_MAX = 512;  // an Access-Request: at most about 420 bytes
    constexpr int64_t IDLE_WAIT_MS = 1000;
    constexpr int64_t POLL_MS = 100;    // the longest select while logins are asked (new jobs start at most this late)

    struct job_t
    {
        bool used;
        bool active;
        uint32_t ticket;
        radius_login_t login;
        radius_settings_t settings;
        char hostname[33];
        radius_done_t done;
        void *context;
        radius_servers_t test_servers; // the state of the servers of a test (not that of the logins)
        radius_exchange_t x;
        int fd;
        uint8_t identifier;
        uint8_t authenticator[RADIUS_AUTHENTICATOR];
        uint8_t packet[PACKET_MAX];
        size_t packet_length;
    };

    SemaphoreHandle_t s_lock = nullptr;
    TaskHandle_t s_task = nullptr;
    // The jobs and the datagram buffer (about 8 kB), taken by radius_task_start
    struct memory_t
    {
        job_t jobs[RADIUS_JOBS_MAX];
        uint8_t datagram[RADIUS_PACKET_MAX]; // the task only
    };
    memory_t *s_mem = nullptr;
    radius_settings_t s_settings{};
    char s_hostname[33] = "";
    radius_servers_t s_servers{};
    char s_server_address[RADIUS_SERVERS][IP_TEXT_V6_SIZE] = {};
    uint32_t s_next_ticket = 1;
    uint8_t s_next_identifier = 0;
    uint32_t s_accepted = 0;
    uint32_t s_rejected = 0;
    uint32_t s_low = 0;
    uint32_t s_no_answer = 0;
    uint32_t s_busy = 0;

    class lock_t
    {
    public:
        lock_t() { xSemaphoreTake(s_lock, portMAX_DELAY); }
        ~lock_t() { xSemaphoreGive(s_lock); }
        lock_t(const lock_t &) = delete;
        lock_t &operator=(const lock_t &) = delete;
    };

    int64_t now_ms()
    {
        return esp_timer_get_time() / 1000;
    }

    bool is_v4_mapped(const uint8_t a[16])
    {
        static constexpr uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        return memcmp(a, prefix, sizeof(prefix)) == 0;
    }

    void address_text(const sockaddr_storage &a, char *out, size_t size)
    {
        if (a.ss_family == AF_INET)
            ip_format_ipv4(reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in *>(&a)->sin_addr), out, size);
        else if (a.ss_family == AF_INET6)
            ip_format_ipv6(reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in6 *>(&a)->sin6_addr), out, size);
        else
            snprintf(out, size, "?");
    }

    radius_servers_t &servers_of(job_t &job)
    {
        return job.login.test ? job.test_servers : s_servers;
    }

    void close_socket(job_t &job)
    {
        if (job.fd >= 0)
            close(job.fd);
        job.fd = -1;
    }

    // The IPv6 source address lwIP takes for a packet to `dest` (its own selection, run in the TCP/IP task)
    struct v6_source_t
    {
        struct netif *netif;
        ip6_addr_t dest;
        uint8_t source[16];
        bool found;
    };

    esp_err_t select_v6_source(void *context)
    {
        v6_source_t &q = *static_cast<v6_source_t *>(context);
        const ip_addr_t *chosen = ip6_select_source_address(q.netif, &q.dest);
        if (chosen != nullptr && !ip6_addr_isany(ip_2_ip6(chosen)))
        {
            memcpy(q.source, ip_2_ip6(chosen)->addr, sizeof(q.source));
            q.found = true;
        }
        return ESP_OK;
    }

    // lwIP leaves the local address of a connected UDP socket unspecified until a packet goes out (the route is chosen
    // per packet): then the address of the Ethernet interface is the one used. IPv6: the address lwIP selects for the
    // server (a unique local one for a unique local server, a global one for a global server), else a global or unique
    // local one.
    void local_address(sockaddr_storage &local, const sockaddr_storage &remote)
    {
        esp_netif_t *netif = app_eth_netif();
        if (netif == nullptr)
            return;
        if (local.ss_family == AF_INET)
        {
            sockaddr_in &v4 = reinterpret_cast<sockaddr_in &>(local);
            esp_netif_ip_info_t info{};
            if (v4.sin_addr.s_addr == 0 && esp_netif_get_ip_info(netif, &info) == ESP_OK)
                v4.sin_addr.s_addr = info.ip.addr;
        }
        else if (local.ss_family == AF_INET6)
        {
            sockaddr_in6 &v6 = reinterpret_cast<sockaddr_in6 &>(local);
            static constexpr uint8_t ANY[16] = {};
            if (memcmp(&v6.sin6_addr, ANY, sizeof(ANY)) != 0)
                return;
            if (remote.ss_family == AF_INET6)
            {
                v6_source_t q{};
                q.netif = static_cast<struct netif *>(esp_netif_get_netif_impl(netif));
                memcpy(q.dest.addr, &reinterpret_cast<const sockaddr_in6 &>(remote).sin6_addr, 16);
                ip6_addr_clear_zone(&q.dest);
                if (q.netif != nullptr && esp_netif_tcpip_exec(select_v6_source, &q) == ESP_OK && q.found)
                {
                    memcpy(&v6.sin6_addr, q.source, 16);
                    return;
                }
            }
            esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES]{};
            const int count = esp_netif_get_all_ip6(netif, addresses);
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < count; ++i)
                {
                    const esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type(&addresses[i]);
                    if (type == (pass == 0 ? ESP_IP6_ADDR_IS_GLOBAL : ESP_IP6_ADDR_IS_UNIQUE_LOCAL))
                    {
                        memcpy(&v6.sin6_addr, addresses[i].addr, 16);
                        return;
                    }
                }
        }
    }

    // Resolves the server of the exchange and connects a new socket to it. false: no address or no socket (the
    // exchange then moves on like a timeout).
    bool open_socket(job_t &job, sockaddr_storage &local)
    {
        close_socket(job);
        const radius_server_settings_t &server = job.settings.server[job.x.server];
        addrinfo hints{};
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_family = AF_UNSPEC;
        addrinfo *found = nullptr;
        char port[8];
        snprintf(port, sizeof(port), "%u", static_cast<unsigned>(server.port));
        if (getaddrinfo(server.host, port, &hints, &found) != 0 || found == nullptr)
        {
            ESP_LOGW(TAG, "RADIUS server %u: %s could not be resolved", job.x.server + 1u, server.host);
            return false;
        }
        sockaddr_storage remote{};
        memcpy(&remote, found->ai_addr, found->ai_addrlen < sizeof(remote) ? found->ai_addrlen : sizeof(remote));
        const socklen_t remote_length = static_cast<socklen_t>(found->ai_addrlen);
        freeaddrinfo(found);
        if (!job.login.test)
        {
            lock_t lock;
            address_text(remote, s_server_address[job.x.server], sizeof(s_server_address[job.x.server]));
        }
        const int fd = socket(remote.ss_family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0)
        {
            ESP_LOGW(TAG, "RADIUS: no socket (errno %d)", errno);
            return false;
        }
        socklen_t local_length = sizeof(local);
        if (connect(fd, reinterpret_cast<const sockaddr *>(&remote), remote_length) != 0 ||
            getsockname(fd, reinterpret_cast<sockaddr *>(&local), &local_length) != 0)
        {
            ESP_LOGW(TAG, "RADIUS server %u: no route (errno %d)", job.x.server + 1u, errno);
            close(fd);
            return false;
        }
        job.fd = fd;
        local_address(local, remote);
        return true;
    }

    // A new request (new identifier and authenticator) for the server of the exchange
    bool build_request(job_t &job)
    {
        sockaddr_storage local{};
        if (!open_socket(job, local))
            return false;
        const radius_settings_t &s = job.settings;
        radius_request_t r{};
        r.user = job.login.user;
        r.user_length = strlen(job.login.user);
        r.password = job.login.password;
        r.password_length = strlen(job.login.password);
        r.nas_identifier = s.nas_identifier[0] != '\0' ? s.nas_identifier : job.hostname;
        r.nas_port_id = job.login.nas_port_id;
        char calling[IP_TEXT_V6_SIZE];
        if (is_v4_mapped(job.login.source))
            ip_format_ipv4(job.login.source + 12, calling, sizeof(calling));
        else
            ip_format_ipv6(job.login.source, calling, sizeof(calling));
        r.calling_station_id = calling;
        if (s.nas_ip[0] != '\0')
            r.nas_ip_set = ip_parse_ipv4(s.nas_ip, r.nas_ip);
        else if (local.ss_family == AF_INET)
        {
            memcpy(r.nas_ip, &reinterpret_cast<const sockaddr_in *>(&local)->sin_addr, 4);
            r.nas_ip_set = true;
        }
        if (s.nas_ipv6[0] != '\0')
            r.nas_ipv6_set = ip_parse_ipv6(s.nas_ipv6, r.nas_ipv6);
        else if (local.ss_family == AF_INET6)
        {
            memcpy(r.nas_ipv6, &reinterpret_cast<const sockaddr_in6 *>(&local)->sin6_addr, 16);
            r.nas_ipv6_set = true;
        }
        r.nas_port_set = true;
        r.nas_port = job.login.nas_port;
        r.nas_port_type = s.nas_port_type;
        r.service_type = s.service_type;
        job.identifier = s_next_identifier++;
        esp_fill_random(job.authenticator, sizeof(job.authenticator));
        const radius_server_settings_t &server = s.server[job.x.server];
        job.packet_length = radius_build_access_request(r, job.identifier, job.authenticator, reinterpret_cast<const uint8_t *>(server.secret),
                                                        strlen(server.secret), job.packet, sizeof(job.packet));
        if (job.packet_length == 0)
            ESP_LOGW(TAG, "RADIUS: the request could not be built (user name, password or texts too long)");
        return job.packet_length > 0;
    }

    void finish(job_t &job, radius_outcome_t &outcome)
    {
        close_socket(job);
        const radius_done_t done = job.done;
        void *context = job.context;
        const uint32_t ticket = job.ticket;
        {
            lock_t lock;
            if (!job.login.test) // the test of the page is not counted
                switch (outcome.result)
                {
                case radius_result_t::accept: ++s_accepted; break;
                case radius_result_t::reject: ++s_rejected; break;
                case radius_result_t::low_privilege:
                case radius_result_t::missing_attribute: ++s_low; break;
                case radius_result_t::no_answer: ++s_no_answer; break;
                case radius_result_t::busy: ++s_busy; break;
                }
            memset(&job, 0, sizeof(job)); // the password and the secrets
            job.fd = -1;
        }
        done(context, ticket, outcome);
        memset(&outcome, 0, sizeof(outcome));
    }

    // Sends the packet of the job and counts the try (a failed send counts as well: it times out like a lost packet)
    void send_packet(job_t &job, int64_t now)
    {
        if (send(job.fd, job.packet, job.packet_length, 0) != static_cast<ssize_t>(job.packet_length))
            ESP_LOGW(TAG, "RADIUS server %u: the request could not be sent (errno %d)", job.x.server + 1u, errno);
        lock_t lock;
        radius_exchange_sent(job.x, radius_timing_of(job.settings), servers_of(job), now);
    }

    // Runs the timing of one login: start, resend, next server, give up
    void step(job_t &job, int64_t now)
    {
        const radius_timing_t timing = radius_timing_of(job.settings);
        radius_step_t next;
        {
            lock_t lock;
            next = job.active ? radius_exchange_tick(job.x, timing, servers_of(job), now)
                              : radius_exchange_start(job.x, timing, servers_of(job), now);
            job.active = true;
        }
        // at most every server once more (a server without an address or a route is passed at once)
        for (size_t round = 0; round <= RADIUS_SERVERS; ++round)
        {
            if (next == radius_step_t::wait)
                return;
            if (next == radius_step_t::no_answer)
            {
                radius_outcome_t outcome{};
                outcome.result = radius_result_t::no_answer;
                ESP_LOGW(TAG, "RADIUS: no server answered for the login of '%s'", job.login.user);
                finish(job, outcome);
                return;
            }
            if (next == radius_step_t::send_new && !build_request(job))
            {
                // no address, no route, no socket: this server counts as not answering (its tries are 0)
                close_socket(job);
                lock_t lock;
                next = radius_exchange_tick(job.x, timing, servers_of(job), now);
                continue;
            }
            send_packet(job, now);
            return;
        }
    }

    // The jobs being asked (a snapshot under the lock: radius_submit fills free slots meanwhile)
    size_t active_jobs(job_t **out)
    {
        lock_t lock;
        size_t n = 0;
        for (job_t &job : s_mem->jobs)
            if (job.used && job.active)
                out[n++] = &job;
        return n;
    }

    void answer(job_t &job, size_t length, int64_t now)
    {
        const radius_server_settings_t &server = job.settings.server[job.x.server];
        radius_required_t required{};
        radius_parse_required(job.settings.required, required);
        radius_answer_t a{};
        const radius_check_t check =
            radius_parse_answer(s_mem->datagram, length, job.identifier, job.authenticator, reinterpret_cast<const uint8_t *>(server.secret),
                                strlen(server.secret), job.settings.require_message_authenticator, required, a);
        if (check != radius_check_t::ok)
        {
            {
                lock_t lock;
                ++servers_of(job).server[job.x.server].dropped;
            }
            ESP_LOGW(TAG, "RADIUS server %u: answer dropped: %s", job.x.server + 1u, radius_check_text(check));
            return;
        }
        radius_outcome_t outcome{};
        outcome.server = job.x.server;
        {
            lock_t lock;
            radius_exchange_answered(job.x, servers_of(job), a.code, now);
            outcome.rtt_ms = servers_of(job).server[job.x.server].last_rtt_ms;
        }
        snprintf(outcome.summary, sizeof(outcome.summary), "%s", a.summary);
        outcome.privilege = radius_privilege(a);
        outcome.challenge = a.code == RADIUS_ACCESS_CHALLENGE;
        if (a.code != RADIUS_ACCESS_ACCEPT)
            outcome.result = radius_result_t::reject;
        else if (outcome.privilege < job.settings.min_privilege)
            outcome.result = radius_result_t::low_privilege;
        else if (!a.required_found)
            outcome.result = radius_result_t::missing_attribute;
        else
            outcome.result = radius_result_t::accept;
        ESP_LOGI(TAG, "RADIUS server %u: %s for '%s' after %u ms (privilege level %d)%s", job.x.server + 1u,
                 a.code == RADIUS_ACCESS_ACCEPT ? "Access-Accept" : outcome.challenge ? "Access-Challenge (not supported)" : "Access-Reject",
                 job.login.user, static_cast<unsigned>(outcome.rtt_ms), outcome.privilege,
                 outcome.result == radius_result_t::accept || a.code != RADIUS_ACCESS_ACCEPT ? "" : ": refused by the settings");
        finish(job, outcome);
    }

    void task(void *)
    {
        job_t *active[RADIUS_JOBS_MAX];
        for (;;)
        {
            // Start waiting jobs (the oldest first) while fewer than RADIUS_ACTIVE_MAX are asked
            for (;;)
            {
                job_t *oldest = nullptr;
                {
                    lock_t lock;
                    size_t asked = 0;
                    for (const job_t &job : s_mem->jobs)
                        asked += job.used && job.active ? 1 : 0;
                    if (asked < RADIUS_ACTIVE_MAX)
                        for (job_t &job : s_mem->jobs)
                            if (job.used && !job.active && (oldest == nullptr || job.ticket - oldest->ticket > 0x80000000u))
                                oldest = &job;
                }
                if (oldest == nullptr)
                    break;
                step(*oldest, now_ms());
            }

            // Wait for answers or the next deadline (a new job wakes the task when none is asked)
            const size_t count = active_jobs(active);
            if (count == 0)
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_WAIT_MS));
                continue;
            }
            fd_set readable;
            FD_ZERO(&readable);
            int max_fd = -1;
            int64_t wait_ms = POLL_MS;
            const int64_t now = now_ms();
            for (size_t i = 0; i < count; ++i)
            {
                if (active[i]->fd >= 0)
                {
                    FD_SET(active[i]->fd, &readable);
                    max_fd = active[i]->fd > max_fd ? active[i]->fd : max_fd;
                }
                const int64_t left = active[i]->x.deadline_ms - now;
                wait_ms = left < wait_ms ? left : wait_ms;
            }
            if (wait_ms < 1)
                wait_ms = 1;
            if (max_fd < 0)
                vTaskDelay(pdMS_TO_TICKS(wait_ms) > 0 ? pdMS_TO_TICKS(wait_ms) : 1);
            else
            {
                timeval tv{};
                tv.tv_sec = 0;
                tv.tv_usec = static_cast<long>(wait_ms * 1000);
                if (select(max_fd + 1, &readable, nullptr, nullptr, &tv) > 0)
                    for (size_t i = 0; i < count; ++i)
                    {
                        job_t &job = *active[i];
                        if (!job.used || job.fd < 0 || !FD_ISSET(job.fd, &readable))
                            continue;
                        const ssize_t length = recv(job.fd, s_mem->datagram, sizeof(s_mem->datagram), MSG_DONTWAIT);
                        if (length > 0)
                            answer(job, static_cast<size_t>(length), now_ms());
                    }
            }
            // The deadlines of the jobs still asked
            const int64_t later = now_ms();
            const size_t still = active_jobs(active);
            for (size_t i = 0; i < still; ++i)
                step(*active[i], later);
        }
    }

    bool same_server(const radius_server_settings_t &a, const radius_server_settings_t &b)
    {
        return strcmp(a.host, b.host) == 0 && a.port == b.port;
    }
} // namespace

const char *radius_result_text(radius_result_t result)
{
    switch (result)
    {
    case radius_result_t::accept: return "accepted";
    case radius_result_t::reject: return "rejected";
    case radius_result_t::low_privilege: return "privilege level too low";
    case radius_result_t::missing_attribute: return "the required attribute is missing";
    case radius_result_t::no_answer: return "no RADIUS server answered";
    case radius_result_t::busy: return "busy";
    }
    return "?";
}

bool radius_task_start(const radius_settings_t &settings, const char *hostname)
{
    if (s_lock != nullptr)
        return true;
    if (s_mem == nullptr)
        s_mem = static_cast<memory_t *>(heap_caps_calloc(1, sizeof(memory_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_mem == nullptr)
        return false;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == nullptr)
        return false;
    for (job_t &job : s_mem->jobs)
        job.fd = -1;
    s_next_identifier = static_cast<uint8_t>(esp_random());
    s_lock = lock;
    radius_task_apply(settings, hostname);
    if (xTaskCreatePinnedToCore(task, "radius", STACK, nullptr, PRIORITY, &s_task, CORE) != pdPASS)
    {
        // Not running: radius_submit refuses every login (s_lock is the mark)
        s_lock = nullptr;
        vSemaphoreDelete(lock);
        s_task = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "RADIUS client started (%u bytes for the logins)", static_cast<unsigned>(sizeof(memory_t)));
    return true;
}

bool radius_task_running()
{
    return s_task != nullptr;
}

bool radius_task_stack(uint32_t &size, uint32_t &least_free)
{
    if (s_task == nullptr)
        return false;
    size = STACK;
    least_free = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task));
    return true;
}

void radius_task_apply(const radius_settings_t &settings, const char *hostname)
{
    if (s_lock == nullptr)
        return;
    lock_t lock;
    for (size_t i = 0; i < RADIUS_SERVERS; ++i)
    {
        if (!same_server(settings.server[i], s_settings.server[i]))
        {
            s_servers.server[i] = radius_server_state_t{};
            s_server_address[i][0] = '\0';
        }
        s_servers.server[i].dead_until_ms = 0; // Apply: the servers are asked again (the setting may have been the fault)
    }
    s_settings = settings;
    snprintf(s_hostname, sizeof(s_hostname), "%s", hostname);
}

uint32_t radius_submit(radius_login_t &login, radius_done_t done, void *context)
{
    uint32_t ticket = 0;
    if (s_lock != nullptr && done != nullptr)
    {
        lock_t lock;
        job_t *slot = nullptr;
        bool same_source = false;
        for (job_t &job : s_mem->jobs)
        {
            if (!job.used)
                slot = slot == nullptr ? &job : slot;
            else if (!login.test && !job.login.test && memcmp(job.login.source, login.source, sizeof(login.source)) == 0)
                same_source = true;
        }
        if (slot != nullptr && !same_source)
        {
            *slot = job_t{};
            slot->fd = -1;
            slot->login = login;
            slot->login.settings = nullptr;
            slot->settings = login.settings != nullptr ? *login.settings : s_settings;
            snprintf(slot->hostname, sizeof(slot->hostname), "%s", s_hostname);
            slot->done = done;
            slot->context = context;
            ticket = s_next_ticket++;
            if (s_next_ticket == 0)
                s_next_ticket = 1;
            slot->ticket = ticket;
            slot->used = true;
        }
        else
            ++s_busy;
    }
    memset(login.password, 0, sizeof(login.password));
    if (ticket != 0)
        xTaskNotifyGive(s_task);
    return ticket;
}

void radius_task_status(radius_status_t &out)
{
    out = radius_status_t{};
    if (s_lock == nullptr)
        return;
    const int64_t now = now_ms();
    lock_t lock;
    out.running = true;
    for (size_t i = 0; i < RADIUS_SERVERS; ++i)
    {
        radius_server_status_t &o = out.server[i];
        const radius_server_settings_t &cfg = s_settings.server[i];
        const radius_server_state_t &st = s_servers.server[i];
        o.used = radius_server_used(cfg);
        snprintf(o.host, sizeof(o.host), "%s", cfg.host);
        static_assert(sizeof(o.address) == sizeof(s_server_address[i]), "the address texts");
        memcpy(o.address, s_server_address[i], sizeof(o.address)); // always terminated (snprintf, address_text)
        o.port = cfg.port;
        o.dead = radius_server_dead(st, now);
        o.dead_for_s = o.dead ? static_cast<uint32_t>((st.dead_until_ms - now + 999) / 1000) : 0;
        o.last_answer_ago_s = st.last_answer_ms > 0 ? (now - st.last_answer_ms) / 1000 : -1;
        o.last_rtt_ms = st.last_rtt_ms;
        o.requests = st.requests;
        o.accepts = st.accepts;
        o.rejects = st.rejects;
        o.timeouts = st.timeouts;
        o.dropped = st.dropped;
    }
    out.accepted = s_accepted;
    out.rejected = s_rejected;
    out.low_privilege = s_low;
    out.no_answer = s_no_answer;
    out.busy = s_busy;
    for (const job_t &job : s_mem->jobs)
        out.active += job.used ? 1 : 0;
}
