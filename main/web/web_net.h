// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Network settings of the web admin page applied to the Ethernet interface at run time:
// IPv4 DHCP or static, IPv6 SLAAC on/off, an additional static IPv6 address with a static default gateway
// (lwIP has no API for that: the ND6 gateway hook below returns it), DNS servers.

#pragma once

#include <cstddef>
#include <cstdint>

#include "web_settings.h"

typedef struct esp_netif_obj esp_netif_t;

struct web_net_status_t
{
    bool ipv4_dhcp_running;
    char ipv4_address[WEB_IPV4_TEXT_SIZE];
    uint8_t ipv4_prefix;
    char ipv4_gateway[WEB_IPV4_TEXT_SIZE];
    size_t ipv6_count;
    char ipv6[8][WEB_IPV6_TEXT_SIZE + 12]; // address and its kind, e.g. "2001:db8::1 (SLAAC)"
    char dns[WEB_DNS_SERVER_COUNT][WEB_IPV6_TEXT_SIZE];
};

// Applies the network part of s. previous = the network settings in use before (nullptr at boot): a static IPv6
// address of previous that is no longer configured is removed. Returns false if a step failed.
bool web_net_apply(esp_netif_t *netif, const web_settings_t &s, const web_settings_t *previous);

// The DHCP client overwrites the DNS servers with the ones of the lease: call after every new IPv4 lease
void web_net_reapply_dns(esp_netif_t *netif, const char (*dns_servers)[WEB_IPV6_TEXT_SIZE]);

// Current state of the interface for the status part of the page
void web_net_status(esp_netif_t *netif, const web_settings_t &s, web_net_status_t &status);

// true if the local address (of an accepted connection) is one of the configured static addresses of s
bool web_net_is_static_address(const web_settings_t &s, const struct sockaddr_storage &local);
