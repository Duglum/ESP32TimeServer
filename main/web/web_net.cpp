// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "web_net.h"

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C"
{
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/ip6_addr.h"
#include "lwip/nd6.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
}

#if !defined(CONFIG_LWIP_HOOK_ND6_GET_GW_CUSTOM)
#error "CONFIG_LWIP_HOOK_ND6_GET_GW_CUSTOM is required (see sdkconfig.defaults; delete sdkconfig and build again)"
#endif

namespace
{
    const char *TAG = "web_net";

    // ---- Static IPv6 default gateway, read by the ND6 hook in the lwIP thread (seqlock, never blocks) ----
    std::atomic<uint32_t> s_gateway_seq{0};
    std::atomic<bool> s_gateway_valid{false};
    std::atomic<struct netif *> s_gateway_netif{nullptr};
    uint8_t s_gateway[16] = {};

    void publish_gateway(struct netif *netif, const uint8_t *gateway)
    {
        const uint32_t s = s_gateway_seq.load(std::memory_order_relaxed);
        s_gateway_seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        if (gateway != nullptr)
            memcpy(s_gateway, gateway, 16);
        s_gateway_netif.store(netif, std::memory_order_relaxed);
        s_gateway_valid.store(gateway != nullptr, std::memory_order_relaxed);
        s_gateway_seq.store(s + 2, std::memory_order_release);
    }

    struct slaac_request_t
    {
        struct netif *netif;
        bool enabled;
        bool has_static;
        uint8_t static_address[16];
    };

    // lwIP thread: SLAAC on/off; when switched off, the addresses created by SLAAC are removed (link-local and the
    // static address stay). Afterwards the destination cache is cleared (gateway or addresses changed).
    esp_err_t slaac_in_lwip_thread(void *context)
    {
        const slaac_request_t *request = static_cast<const slaac_request_t *>(context);
        struct netif *netif = request->netif;
        netif_set_ip6_autoconfig_enabled(netif, request->enabled ? 1 : 0);
        if (!request->enabled)
        {
            for (s8_t i = 1; i < LWIP_IPV6_NUM_ADDRESSES; ++i)
            {
                if (ip6_addr_isinvalid(netif_ip6_addr_state(netif, i)))
                    continue;
                const ip6_addr_t *address = netif_ip6_addr(netif, i);
                if (ip6_addr_islinklocal(address) ||
                    (request->has_static && memcmp(address->addr, request->static_address, 16) == 0))
                    continue;
                netif_ip6_addr_set_state(netif, i, IP6_ADDR_INVALID);
            }
        }
        nd6_clear_destination_cache();
        return ESP_OK;
    }

    void to_esp_ip(const char *text, esp_ip_addr_t &out)
    {
        memset(&out, 0, sizeof(out));
        uint32_t v4 = 0;
        uint8_t v6[16];
        if (web_parse_ipv4(text, &v4))
        {
            out.type = ESP_IPADDR_TYPE_V4;
            out.u_addr.ip4.addr = v4;
        }
        else if (web_parse_ipv6(text, v6))
        {
            out.type = ESP_IPADDR_TYPE_V6;
            memcpy(out.u_addr.ip6.addr, v6, 16);
        }
    }

    bool apply_dns(esp_netif_t *netif, const char (*dns_servers)[WEB_IPV6_TEXT_SIZE])
    {
        bool any = false;
        for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
            any = any || dns_servers[i][0] != '\0';
        if (!any)
            return true; // nothing configured: the servers of the DHCP lease (or none) stay in use

        static constexpr esp_netif_dns_type_t types[WEB_DNS_SERVER_COUNT] = {ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK};
        bool ok = true;
        for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
        {
            esp_netif_dns_info_t info{};
            to_esp_ip(dns_servers[i], info.ip); // an empty entry clears the slot
            ok = esp_netif_set_dns_info(netif, types[i], &info) == ESP_OK && ok;
        }
        return ok;
    }
} // namespace

// ND6 gateway hook (CONFIG_LWIP_HOOK_ND6_GET_GW_CUSTOM): called in the lwIP thread for destinations that are not
// on-link and have no destination cache entry. Returns the static IPv6 gateway, or nullptr to let lwIP select one
// of the routers learned from router advertisements.
extern "C" const ip6_addr_t *lwip_hook_nd6_get_gw(struct netif *netif, const ip6_addr_t *dest)
{
    (void)dest;
    static ip6_addr_t result; // only used in the lwIP thread
    uint8_t gateway[16];
    struct netif *gateway_netif = nullptr;
    uint32_t s1, s2;
    do
    {
        s1 = s_gateway_seq.load(std::memory_order_acquire);
        if (!s_gateway_valid.load(std::memory_order_relaxed))
            return nullptr;
        memcpy(gateway, s_gateway, 16);
        gateway_netif = s_gateway_netif.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        s2 = s_gateway_seq.load(std::memory_order_relaxed);
    } while (s1 != s2 || (s1 & 1u));

    if (gateway_netif != netif)
        return nullptr;
    memcpy(result.addr, gateway, 16);
    ip6_addr_clear_zone(&result);
    ip6_addr_assign_zone(&result, IP6_UNICAST, netif);
    return &result;
}

bool web_net_apply(esp_netif_t *netif, const web_settings_t &s, const web_settings_t *previous)
{
    if (netif == nullptr)
        return false;
    bool ok = true;
    struct netif *lwip_netif = static_cast<struct netif *>(esp_netif_get_netif_impl(netif));

    // ---- IPv4 ----
    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcpc_get_status(netif, &dhcp_status);
    if (s.ipv4_dhcp)
    {
        // Only a client that was stopped for a static address is started again; at the start (INIT) the
        // interface starts it by itself when the link comes up
        if (dhcp_status == ESP_NETIF_DHCP_STOPPED)
        {
            esp_netif_ip_info_t none{};
            esp_netif_set_ip_info(netif, &none); // drop the static address before the lease arrives
            const esp_err_t err = esp_netif_dhcpc_start(netif);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
            {
                ESP_LOGE(TAG, "DHCP client could not be started: %s", esp_err_to_name(err));
                ok = false;
            }
        }
    }
    else
    {
        const esp_err_t stop_err = esp_netif_dhcpc_stop(netif);
        esp_netif_ip_info_t info{};
        uint32_t address = 0;
        uint32_t gateway = 0;
        web_parse_ipv4(s.ipv4_address, &address);
        web_parse_ipv4(s.ipv4_gateway, &gateway);
        info.ip.addr = address;
        info.gw.addr = gateway;
        info.netmask.addr = htonl(0xFFFFFFFFu << (32 - s.ipv4_prefix));
        const esp_err_t err = (stop_err == ESP_OK || stop_err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
                                  ? esp_netif_set_ip_info(netif, &info)
                                  : stop_err;
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Static IPv4 address could not be set: %s", esp_err_to_name(err));
            ok = false;
        }
    }

    // ---- IPv6 static address ----
    uint8_t new_address[16] = {};
    const bool has_static = s.ipv6_address[0] != '\0' && web_parse_ipv6(s.ipv6_address, new_address);
    if (previous != nullptr && previous->ipv6_address[0] != '\0' && strcmp(previous->ipv6_address, s.ipv6_address) != 0)
    {
        esp_ip6_addr_t old{};
        if (web_parse_ipv6(previous->ipv6_address, reinterpret_cast<uint8_t *>(old.addr)))
            esp_netif_remove_ip6_address(netif, &old);
    }
    publish_gateway(nullptr, nullptr); // the gateway is set again below
    if (has_static && (previous == nullptr || strcmp(previous->ipv6_address, s.ipv6_address) != 0))
    {
        esp_ip6_addr_t address{};
        memcpy(address.addr, new_address, 16);
        const esp_err_t err = esp_netif_add_ip6_address(netif, address, true);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Static IPv6 address could not be added: %s", esp_err_to_name(err));
            ok = false;
        }
    }
    uint8_t gateway[16];
    if (has_static && s.ipv6_gateway[0] != '\0' && web_parse_ipv6(s.ipv6_gateway, gateway))
        publish_gateway(lwip_netif, gateway);

    // ---- SLAAC (and destination cache) ----
    if (lwip_netif != nullptr)
    {
        slaac_request_t request{lwip_netif, s.ipv6_slaac, has_static, {}};
        memcpy(request.static_address, new_address, 16);
        esp_netif_tcpip_exec(slaac_in_lwip_thread, &request);
    }

    // ---- DNS ----
    if (!apply_dns(netif, s.dns))
    {
        ESP_LOGE(TAG, "DNS servers could not be set");
        ok = false;
    }
    return ok;
}

void web_net_reapply_dns(esp_netif_t *netif, const char (*dns_servers)[WEB_IPV6_TEXT_SIZE])
{
    if (netif != nullptr)
        apply_dns(netif, dns_servers);
}

void web_net_status(esp_netif_t *netif, const web_settings_t &s, web_net_status_t &status)
{
    status = web_net_status_t{};
    if (netif == nullptr)
        return;

    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcpc_get_status(netif, &dhcp_status);
    status.ipv4_dhcp_running = dhcp_status == ESP_NETIF_DHCP_STARTED;
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0)
    {
        inet_ntop(AF_INET, &info.ip.addr, status.ipv4_address, sizeof(status.ipv4_address));
        inet_ntop(AF_INET, &info.gw.addr, status.ipv4_gateway, sizeof(status.ipv4_gateway));
        status.ipv4_prefix = static_cast<uint8_t>(__builtin_popcount(info.netmask.addr));
    }

    esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES]{};
    const int count = esp_netif_get_all_ip6(netif, addresses);
    uint8_t static_address[16] = {};
    const bool has_static = web_parse_ipv6(s.ipv6_address, static_address);
    for (int i = 0; i < count && status.ipv6_count < 8; ++i)
    {
        char text[WEB_IPV6_TEXT_SIZE] = "";
        inet_ntop(AF_INET6, addresses[i].addr, text, sizeof(text));
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(addresses[i].addr);
        const char *kind = (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80) ? "link-local"
                           : (has_static && memcmp(bytes, static_address, 16) == 0) ? "static"
                           : (bytes[0] & 0xFE) == 0xFC                              ? "SLAAC, unique local"
                                                                                    : "SLAAC, global";
        snprintf(status.ipv6[status.ipv6_count++], sizeof(status.ipv6[0]), "%s (%s)", text, kind);
    }

    static constexpr esp_netif_dns_type_t types[WEB_DNS_SERVER_COUNT] = {ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK};
    for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
    {
        esp_netif_dns_info_t dns{};
        if (esp_netif_get_dns_info(netif, types[i], &dns) != ESP_OK)
            continue;
        if (dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0)
            inet_ntop(AF_INET, &dns.ip.u_addr.ip4.addr, status.dns[i], sizeof(status.dns[i]));
        else if (dns.ip.type == ESP_IPADDR_TYPE_V6)
        {
            static constexpr uint8_t zero[16] = {};
            if (memcmp(dns.ip.u_addr.ip6.addr, zero, 16) != 0)
                inet_ntop(AF_INET6, dns.ip.u_addr.ip6.addr, status.dns[i], sizeof(status.dns[i]));
        }
    }
}

bool web_net_is_static_address(const web_settings_t &s, const struct sockaddr_storage &local)
{
    if (local.ss_family == AF_INET6)
    {
        const uint8_t *address = reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in6 *>(&local)->sin6_addr);
        static constexpr uint8_t v4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (memcmp(address, v4_mapped, 12) == 0)
        {
            uint32_t v4 = 0;
            return !s.ipv4_dhcp && web_parse_ipv4(s.ipv4_address, &v4) && memcmp(address + 12, &v4, 4) == 0;
        }
        uint8_t configured[16];
        return web_parse_ipv6(s.ipv6_address, configured) && memcmp(address, configured, 16) == 0;
    }
    if (local.ss_family == AF_INET)
    {
        uint32_t v4 = 0;
        return !s.ipv4_dhcp && web_parse_ipv4(s.ipv4_address, &v4) &&
               reinterpret_cast<const sockaddr_in *>(&local)->sin_addr.s_addr == v4;
    }
    return false;
}
