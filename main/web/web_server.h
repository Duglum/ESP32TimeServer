// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Web admin page: HTTPS on TCP 443 only (IPv4 and IPv6), self-signed per-device certificate.

#pragma once

#include <cstddef>
#include <cstdint>

#include "web_settings.h"

typedef struct esp_netif_obj esp_netif_t;

struct web_app_status_t
{
    char firmware_version[24];
    char build[48];
    char chip_revision[8];        // e.g. "v3.2"
    bool ptp_loopback_supported;  // chip revision 3.0 or newer (PTP PPS loopback)
    bool time_set;
    bool pps_active;
    bool gnss_locked;
    bool stratum_one;
    bool pps_holdover;            // stratum 1 in the PPS holdover (PPS lost)
    bool ublox_receiver;          // UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED: the baud rate is detected automatically
    uint32_t gnss_baud_in_use;
    char gnss_dynamic_model[16];  // U-Blox UBX: dynamic platform model read from the receiver ("" = not known)
    bool lcd_present;
    bool led_active;
    bool mqtt_connected;
    bool mqtt_tf_queue;           // MQTT reports are queued on the TF card (QoS 1 or 2 and a usable FAT32 card)
    char mac_address[WEB_MAC_TEXT_SIZE]; // MAC in use since the start
    bool ntp_auth_available;
    size_t ntp_auth_compiled_keys;
    size_t ntp_auth_keys_in_use;
    uint32_t free_heap;
};

// Starts the web admin in its own task (TLS identity, password hash, HTTPS server). Call once the Ethernet
// interface exists. If the settings of this boot came from a one-shot set that changed the network, the 120 s
// confirmation starts as well.
void web_admin_start();

// Implemented in main.cpp
void app_get_status(web_app_status_t &status);
esp_netif_t *app_eth_netif();
// The network configuration of the interface changed: refresh the address cache used by the NTP and LCD code
void app_network_settings_changed();
// Over The Ethernet update (firmware upload, see web_ote.h): switched on for this boot, upload progress, failure
// (reason shown on the LCD, at most 20 characters) and an accepted image (the device restarts a few seconds later)
bool app_ote_enabled();
void app_ote_progress(size_t written, size_t total);
void app_ote_failed(const char *reason);
void app_ote_accepted();
