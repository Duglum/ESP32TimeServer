// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "web_settings.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>

extern "C"
{
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
}

namespace
{
    const char *TAG = "web_settings";

    constexpr char NVS_NAMESPACE[] = "webcfg";
    constexpr char NVS_KEY_SAVED[] = "saved";
    constexpr char NVS_KEY_ONESHOT[] = "oneshot";
    constexpr char AUTH_NVS_NAMESPACE[] = "webauth"; // admin password hash and allow-list (see web_auth.cpp)

    constexpr uint32_t BLOB_MAGIC = 0x57534554u; // "WSET"
    // Increase when web_settings_t changes: a saved set of another version is ignored (the defaults are used)
    // 2: GNSS protocol and LED colours added; 3: Over The Ethernet update settings added; 4: up to 8 NTP keys;
    // 5: MAC address, MQTT topic / QoS / retain / period / report contents / TF client limit, TF card pins;
    // 6: PPS holdover in the place of the former OTE port, behaviour after the holdover appended at the end
    //    (a version 5 set is still read: its fields are a prefix of web_settings_t, the new ones get the defaults);
    // 7: root dispersion growth while coasting appended at the end (versions 5 and 6 are still read the same way);
    // 8: NTP client rate limit appended at the end (versions 5 to 7 are still read the same way);
    // 9: minimum GNSS fix age before stratum 1 appended at the end (versions 5 to 8 are still read the same way);
    // 10: minimum satellites in use before stratum 1 appended at the end (versions 5 to 9 are still read the same way);
    // 11: address families and the address shown on the LCD appended at the end (versions 5 to 10 are still read the
    //     same way);
    // 12: uptime / restart button appended at the end (versions 5 to 11 are still read the same way);
    // 13: MQTT broker, port, TLS and CA certificate appended at the end in the place of the broker URI, whose bytes
    //     stay in the layout (mqtt_uri_unused): the URI of versions 5 to 12 is read into the new fields;
    // 14: PTP master appended at the end (versions 5 to 13 are still read, PTP gets its defaults: off).
    // The OTE device name and OTE password were removed later without a new version: their places stay in the layout
    // (ote_name_unused, ote_password_unused) and are zeroed when a set is read.
    constexpr uint32_t BLOB_VERSION = 14;
    constexpr uint32_t BLOB_VERSION_5 = 5;
    constexpr uint32_t BLOB_VERSION_6 = 6;
    constexpr uint32_t BLOB_VERSION_7 = 7;
    constexpr uint32_t BLOB_VERSION_8 = 8;
    constexpr uint32_t BLOB_VERSION_9 = 9;
    constexpr uint32_t BLOB_VERSION_10 = 10;
    constexpr uint32_t BLOB_VERSION_11 = 11;
    constexpr uint32_t BLOB_VERSION_12 = 12;
    constexpr uint32_t BLOB_VERSION_13 = 13;
    // Size of web_settings_t in an older version: everything before the first field appended later, with the tail
    // padding
    constexpr size_t settings_size_before(size_t first_new_field)
    {
        return (first_new_field + alignof(web_settings_t) - 1) / alignof(web_settings_t) * alignof(web_settings_t);
    }
    constexpr size_t SETTINGS_SIZE_5 = settings_size_before(offsetof(web_settings_t, pps_lost_stratum));
    constexpr size_t SETTINGS_SIZE_6 = settings_size_before(offsetof(web_settings_t, pps_dispersion_ppm));
    constexpr size_t SETTINGS_SIZE_7 = settings_size_before(offsetof(web_settings_t, ntp_client_rate_limit));
    constexpr size_t SETTINGS_SIZE_8 = settings_size_before(offsetof(web_settings_t, gnss_min_fix_age_s));
    constexpr size_t SETTINGS_SIZE_9 = settings_size_before(offsetof(web_settings_t, gnss_min_satellites));
    constexpr size_t SETTINGS_SIZE_10 = settings_size_before(offsetof(web_settings_t, ip_mode));
    constexpr size_t SETTINGS_SIZE_11 = settings_size_before(offsetof(web_settings_t, button_enabled));
    constexpr size_t SETTINGS_SIZE_12 = settings_size_before(offsetof(web_settings_t, mqtt_broker));
    constexpr size_t SETTINGS_SIZE_13 = settings_size_before(offsetof(web_settings_t, ptp_l2_enabled));
    // Version 7 fits into the tail padding of version 6 (same size), and versions 8 to 12 may fit into that of the
    // version before: the version number tells them apart, and an older set always gets the defaults for the newer
    // fields instead of its padding bytes
    static_assert(SETTINGS_SIZE_5 <= SETTINGS_SIZE_6 && SETTINGS_SIZE_6 <= SETTINGS_SIZE_7 &&
                      SETTINGS_SIZE_7 <= SETTINGS_SIZE_8 && SETTINGS_SIZE_8 <= SETTINGS_SIZE_9 &&
                      SETTINGS_SIZE_9 <= SETTINGS_SIZE_10 && SETTINGS_SIZE_10 <= SETTINGS_SIZE_11 &&
                      SETTINGS_SIZE_11 <= SETTINGS_SIZE_12 && SETTINGS_SIZE_12 <= SETTINGS_SIZE_13 &&
                      SETTINGS_SIZE_13 <= sizeof(web_settings_t),
                  "older settings must be a prefix of web_settings_t");

    // Defaults of the fields that later blob versions appended (web_settings_defaults() uses them as well)
    constexpr uint16_t DEFAULT_PPS_HOLDOVER_S = 2;         // see web_settings_defaults()
    constexpr uint8_t DEFAULT_PPS_LOST_STRATUM = 16;
    constexpr bool DEFAULT_PPS_LOST_SERVE = true;
    constexpr uint16_t DEFAULT_PPS_LOST_SERVE_MIN = 60;
    constexpr uint16_t DEFAULT_PPS_DISPERSION_PPM = 15;
    constexpr uint16_t DEFAULT_NTP_CLIENT_RATE_LIMIT = 20;
    constexpr uint16_t DEFAULT_GNSS_MIN_FIX_AGE_S = 0;
    constexpr uint8_t DEFAULT_GNSS_MIN_SATELLITES = 0;
    constexpr bool DEFAULT_BUTTON_ENABLED = false;
    constexpr int8_t DEFAULT_BUTTON_PIN = 3;
    constexpr char DEFAULT_MQTT_TOPIC[] = "ESP32TimeServer";

    // PTP master (version 14): off; the values of the default profile of IEEE 1588-2019
    void ptp_defaults(web_settings_t &out)
    {
        out.ptp_l2_enabled = false;
        out.ptp_udp4_enabled = false;
        out.ptp_udp6_enabled = false;
        out.ptp_domain = 0;
        out.ptp_priority1 = 128;
        out.ptp_priority2 = 128;
        out.ptp_log_sync_interval = 0;          // 1 s
        out.ptp_log_announce_interval = 1;      // 2 s
        out.ptp_log_min_delay_req_interval = 0; // 1 s
        out.ptp_master_selection = true;
        out.ptp_clock_accuracy = 0x22;          // within 250 ns
        out.ptp_follower_timeout_s = 30;
        out.ptp_delay_req_rate_limit = 16;
        out.ptp_utc_offset_s = 37;              // TAI - UTC since 2017-01-01
        out.ptp_ingress_latency_ns = 0;
        out.ptp_egress_latency_ns = 0;
    }

    struct settings_blob_t
    {
        uint32_t magic;
        uint32_t version;
        uint32_t size;
        uint32_t flags; // bit 0: one-shot set changed the network (confirmation required)
        web_settings_t settings;
        uint32_t crc;
    };

    web_settings_t s_boot{};
    web_settings_t s_running{};
    bool s_boot_oneshot = false;
    bool s_boot_network_confirm = false;
    SemaphoreHandle_t s_mutex = nullptr;

    template <size_t N>
    void copy_text(char (&destination)[N], const char *source)
    {
        snprintf(destination, N, "%s", source == nullptr ? "" : source);
    }

    uint32_t blob_crc(const settings_blob_t &blob)
    {
        return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&blob), offsetof(settings_blob_t, crc));
    }

    bool any_byte_set(const char *bytes, size_t length)
    {
        for (size_t i = 0; i < length; ++i)
            if (bytes[i] != '\0')
                return true;
        return false;
    }

    // Broker URI of versions 5 to 12 ("mqtt://host[:port]" or "mqtts://host[:port]", an IPv6 address in brackets)
    // into broker, port and TLS. A URI that does not fit the checks of version 13 leaves the broker empty and MQTT
    // off, and so does TLS (the URI was checked against the IDF CA bundle, version 13 needs the CA certificate of the
    // broker): a saved set must stay valid, or every setting would fall back to its default.
    void read_mqtt_uri(web_settings_t &s, const char *key)
    {
        char uri[WEB_MQTT_URI_SIZE];
        memcpy(uri, s.mqtt_uri_unused, sizeof(uri));
        uri[sizeof(uri) - 1] = '\0';
        memset(s.mqtt_uri_unused, 0, sizeof(s.mqtt_uri_unused));
        memset(s.mqtt_ca_pem, 0, sizeof(s.mqtt_ca_pem));
        if (uri[0] == '\0')
        {
            memset(s.mqtt_broker, 0, sizeof(s.mqtt_broker));
            s.mqtt_port = 0;
            s.mqtt_tls = false;
            s.mqtt_enabled = false; // the URI was required while MQTT is on
            return;
        }
        if (!web_mqtt_split_uri(uri, s))
        {
            ESP_LOGW(TAG, "Stored web settings '%s': the MQTT broker URI %s does not fit the broker field, MQTT is off", key, uri);
            s.mqtt_enabled = false;
            return;
        }
        if (s.mqtt_tls && s.mqtt_enabled)
        {
            ESP_LOGW(TAG, "Stored web settings '%s': MQTT with TLS needs the CA certificate of the broker now, MQTT is off "
                          "until it is set on the web page", key);
            s.mqtt_enabled = false;
        }
        ESP_LOGI(TAG, "Stored web settings '%s': MQTT broker URI %s read as broker %s, port %u, %s", key, uri, s.mqtt_broker,
                 static_cast<unsigned>(web_mqtt_port_in_use(s)), s.mqtt_tls ? "TLS" : "no TLS");
    }

    // Reads a settings blob. Returns true if it exists, matches this firmware (or version 5 to 12) and passes the CRC.
    // *had_ote_fields (optional): the stored set still held the former OTE device name or password (zeroed in blob)
    bool read_blob(nvs_handle_t handle, const char *key, settings_blob_t &blob, bool *had_ote_fields = nullptr)
    {
        if (had_ote_fields != nullptr)
            *had_ote_fields = false;
        // Stored as header (magic, version, size, flags), settings, CRC of everything before it
        constexpr size_t HEADER_SIZE = offsetof(settings_blob_t, settings);
        static_assert(offsetof(settings_blob_t, crc) == HEADER_SIZE + sizeof(web_settings_t), "blob layout");
        uint8_t *raw = static_cast<uint8_t *>(calloc(1, sizeof(settings_blob_t)));
        if (raw == nullptr)
            return false;
        size_t length = sizeof(settings_blob_t);
        const esp_err_t err = nvs_get_blob(handle, key, raw, &length);
        const bool readable = err == ESP_OK && length >= HEADER_SIZE + sizeof(uint32_t);
        // Only the header: a settings_blob_t or web_settings_t (about 1 kB each) must not be on the stack, this runs
        // in app_main (main task stack 3584 bytes)
        struct
        {
            uint32_t magic;
            uint32_t version;
            uint32_t size;
            uint32_t flags;
        } header{};
        static_assert(sizeof(header) == HEADER_SIZE, "blob header layout");
        uint32_t stored_crc = 0;
        if (readable)
        {
            memcpy(&header, raw, HEADER_SIZE);
            memcpy(&stored_crc, raw + length - sizeof(uint32_t), sizeof(uint32_t));
        }
        const bool current = header.version == BLOB_VERSION && header.size == sizeof(web_settings_t);
        const bool version_5 = header.version == BLOB_VERSION_5 && header.size == SETTINGS_SIZE_5;
        const bool version_6 = header.version == BLOB_VERSION_6 && header.size == SETTINGS_SIZE_6;
        const bool version_7 = header.version == BLOB_VERSION_7 && header.size == SETTINGS_SIZE_7;
        const bool version_8 = header.version == BLOB_VERSION_8 && header.size == SETTINGS_SIZE_8;
        const bool version_9 = header.version == BLOB_VERSION_9 && header.size == SETTINGS_SIZE_9;
        const bool version_10 = header.version == BLOB_VERSION_10 && header.size == SETTINGS_SIZE_10;
        const bool version_11 = header.version == BLOB_VERSION_11 && header.size == SETTINGS_SIZE_11;
        const bool version_12 = header.version == BLOB_VERSION_12 && header.size == SETTINGS_SIZE_12;
        const bool version_13 = header.version == BLOB_VERSION_13 && header.size == SETTINGS_SIZE_13;

        bool ok = false;
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
        }
        else if (!readable || (length != sizeof(settings_blob_t) && length != HEADER_SIZE + SETTINGS_SIZE_5 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_6 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_7 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_8 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_9 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_10 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_11 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_12 + sizeof(uint32_t) &&
                               length != HEADER_SIZE + SETTINGS_SIZE_13 + sizeof(uint32_t)))
            ESP_LOGW(TAG, "Stored web settings '%s' have an unexpected size, ignored", key);
        else if (header.magic != BLOB_MAGIC || (!current && !version_5 && !version_6 && !version_7 && !version_8 && !version_9 && !version_10 &&
                                                   !version_11 && !version_12 && !version_13) ||
                 length != HEADER_SIZE + header.size + sizeof(uint32_t))
            ESP_LOGW(TAG, "Stored web settings '%s' belong to another firmware version, ignored", key);
        else if (stored_crc != esp_rom_crc32_le(0, raw, length - sizeof(uint32_t)))
            ESP_LOGW(TAG, "Stored web settings '%s' have a CRC error, ignored", key);
        else
        {
            memset(&blob, 0, sizeof(blob)); // no settings_blob_t{} temporary on the stack
            memcpy(&blob, raw, HEADER_SIZE + header.size);
            if (version_5)
            {
                // The holdover field held the former OTE port; the fields of version 6 get the defaults
                blob.settings.pps_holdover_s = DEFAULT_PPS_HOLDOVER_S;
                blob.settings.pps_lost_stratum = DEFAULT_PPS_LOST_STRATUM;
                blob.settings.pps_lost_serve = DEFAULT_PPS_LOST_SERVE;
                blob.settings.pps_lost_serve_min = DEFAULT_PPS_LOST_SERVE_MIN;
                ESP_LOGI(TAG, "Stored web settings '%s' of version 5 read; the PPS holdover settings have their defaults", key);
            }
            if (version_5 || version_6)
            {
                // Appended in version 7
                blob.settings.pps_dispersion_ppm = DEFAULT_PPS_DISPERSION_PPM;
                if (version_6)
                    ESP_LOGI(TAG, "Stored web settings '%s' of version 6 read; the root dispersion growth has its default", key);
            }
            if (version_5 || version_6 || version_7)
            {
                // Appended in version 8
                blob.settings.ntp_client_rate_limit = DEFAULT_NTP_CLIENT_RATE_LIMIT;
                ESP_LOGI(TAG, "Stored web settings '%s' of version %lu read; the NTP client rate limit has its default", key,
                         static_cast<unsigned long>(header.version));
            }
            if (version_5 || version_6 || version_7 || version_8)
            {
                // Appended in version 9
                blob.settings.gnss_min_fix_age_s = DEFAULT_GNSS_MIN_FIX_AGE_S;
                if (version_8)
                    ESP_LOGI(TAG, "Stored web settings '%s' of version 8 read; the minimum GNSS fix age has its default", key);
            }
            if (version_5 || version_6 || version_7 || version_8 || version_9)
            {
                // Appended in version 10
                blob.settings.gnss_min_satellites = DEFAULT_GNSS_MIN_SATELLITES;
                if (version_9)
                    ESP_LOGI(TAG, "Stored web settings '%s' of version 9 read; the minimum satellites in use have their default", key);
            }
            if (version_5 || version_6 || version_7 || version_8 || version_9 || version_10)
            {
                // Appended in version 11
                blob.settings.ip_mode = web_ip_mode_t::dual;
                blob.settings.lcd_address = web_lcd_address_t::both;
                if (version_10)
                    ESP_LOGI(TAG, "Stored web settings '%s' of version 10 read; the address families have their default", key);
            }
            if (version_5 || version_6 || version_7 || version_8 || version_9 || version_10 || version_11)
            {
                // Appended in version 12
                blob.settings.button_enabled = DEFAULT_BUTTON_ENABLED;
                blob.settings.button_pin = DEFAULT_BUTTON_PIN;
                if (version_11)
                    ESP_LOGI(TAG, "Stored web settings '%s' of version 11 read; the uptime / restart button has its default", key);
            }
            if (!current && !version_13)
            {
                // Version 13: the broker URI into broker, port and TLS; the topic rule is stricter now (no leading $,
                // no trailing /)
                read_mqtt_uri(blob.settings, key);
                if (!web_mqtt_topic_valid(blob.settings.mqtt_topic))
                {
                    ESP_LOGW(TAG, "Stored web settings '%s': the MQTT topic does not fit the topic rules, the default is used", key);
                    copy_text(blob.settings.mqtt_topic, DEFAULT_MQTT_TOPIC);
                }
            }
            if (!current)
            {
                // Appended in version 14
                ptp_defaults(blob.settings);
                if (version_13)
                    ESP_LOGI(TAG, "Stored web settings '%s' of version 13 read; the PTP master has its defaults (off)", key);
                blob.version = BLOB_VERSION;
                blob.size = sizeof(web_settings_t);
            }
            memset(blob.settings.mqtt_uri_unused, 0, sizeof(blob.settings.mqtt_uri_unused));
            // A set that used the keys of the former ESP32TimeServerSettingsSecrets.h: they no longer exist. NTP
            // authentication without a key would make the whole set invalid, so it is switched off.
            if (!blob.settings.ntp_key_override)
            {
                blob.settings.ntp_key_override = true;
                blob.settings.ntp_key_count = 0;
                memset(blob.settings.ntp_keys, 0, sizeof(blob.settings.ntp_keys));
                if (blob.settings.ntp_auth_enabled)
                    ESP_LOGW(TAG, "Stored web settings '%s' used the keys of the former settings file: NTP authentication is off "
                                  "until keys are set on the web page", key);
                blob.settings.ntp_auth_enabled = false;
            }
            // Former OTE device name and OTE password: no longer used
            const bool had_ote = any_byte_set(blob.settings.ote_name_unused, sizeof(blob.settings.ote_name_unused)) ||
                                 any_byte_set(blob.settings.ote_password_unused, sizeof(blob.settings.ote_password_unused));
            memset(blob.settings.ote_name_unused, 0, sizeof(blob.settings.ote_name_unused));
            memset(blob.settings.ote_password_unused, 0, sizeof(blob.settings.ote_password_unused));
            if (had_ote_fields != nullptr)
                *had_ote_fields = had_ote;
            blob.crc = blob_crc(blob);
            ok = true;
        }
        memset(raw, 0, sizeof(settings_blob_t)); // contains secrets
        free(raw);
        if (!ok)
            return false;
        // Text fields are always terminated (the CRC covers them, this only protects against a firmware bug)
        blob.settings.fqdn[WEB_FQDN_SIZE - 1] = '\0';
        blob.settings.mqtt_broker[WEB_FQDN_SIZE - 1] = '\0';
        blob.settings.mqtt_ca_pem[WEB_MQTT_CA_SIZE - 1] = '\0';
        return true;
    }

    esp_err_t write_blob(const char *key, const web_settings_t &settings, uint32_t flags)
    {
        // About 5.5 kB with the CA certificate: on the heap, not on the stack of the web server task
        settings_blob_t *blob = static_cast<settings_blob_t *>(calloc(1, sizeof(settings_blob_t)));
        if (blob == nullptr)
            return ESP_ERR_NO_MEM;
        blob->magic = BLOB_MAGIC;
        blob->version = BLOB_VERSION;
        blob->size = sizeof(web_settings_t);
        blob->flags = flags;
        blob->settings = settings;
        blob->crc = blob_crc(*blob);

        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK)
        {
            err = nvs_set_blob(handle, key, blob, sizeof(settings_blob_t));
            if (err == ESP_OK)
                err = nvs_commit(handle);
            nvs_close(handle);
        }
        memset(blob, 0, sizeof(settings_blob_t)); // contains secrets
        free(blob);
        return err;
    }

    bool is_printable_ascii(const char *text, char lowest, char highest)
    {
        for (const char *c = text; *c != '\0'; ++c)
            if (*c < lowest || *c > highest)
                return false;
        return true;
    }

    // ---- POSIX TZ validation ----
    bool tz_name(const char *&p)
    {
        if (*p == '<')
        {
            ++p;
            int n = 0;
            while (isalnum(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-')
            {
                ++p;
                ++n;
            }
            if (*p != '>' || n < 3)
                return false;
            ++p;
            return true;
        }
        int n = 0;
        while (isalpha(static_cast<unsigned char>(*p)))
        {
            ++p;
            ++n;
        }
        return n >= 3;
    }

    bool tz_number(const char *&p, int min_value, int max_value, int max_digits)
    {
        int value = 0;
        int digits = 0;
        while (isdigit(static_cast<unsigned char>(*p)) && digits < max_digits)
        {
            value = value * 10 + (*p - '0');
            ++p;
            ++digits;
        }
        return digits > 0 && !isdigit(static_cast<unsigned char>(*p)) && value >= min_value && value <= max_value;
    }

    // [+-]hh[:mm[:ss]]
    bool tz_time(const char *&p, int max_hours)
    {
        if (*p == '+' || *p == '-')
            ++p;
        if (!tz_number(p, 0, max_hours, 3))
            return false;
        for (int part = 0; part < 2 && *p == ':'; ++part)
        {
            ++p;
            if (!tz_number(p, 0, 59, 2))
                return false;
        }
        return true;
    }

    // Jn, n or Mm.w.d, optionally followed by /time
    bool tz_rule_date(const char *&p)
    {
        if (*p == 'J')
        {
            ++p;
            if (!tz_number(p, 1, 365, 3))
                return false;
        }
        else if (*p == 'M')
        {
            ++p;
            if (!tz_number(p, 1, 12, 2) || *p++ != '.' || !tz_number(p, 1, 5, 1) || *p++ != '.' || !tz_number(p, 0, 6, 1))
                return false;
        }
        else if (!tz_number(p, 0, 365, 3))
            return false;
        if (*p == '/')
        {
            ++p;
            if (!tz_time(p, 167))
                return false;
        }
        return true;
    }
} // namespace

bool web_parse_ipv4(const char *text, uint32_t *address_be)
{
    struct in_addr address{};
    if (text == nullptr || text[0] == '\0' || strlen(text) >= WEB_IPV4_TEXT_SIZE || inet_pton(AF_INET, text, &address) != 1)
        return false;
    if (address_be != nullptr)
        *address_be = address.s_addr;
    return true;
}

bool web_parse_ipv6(const char *text, uint8_t address[16])
{
    struct in6_addr parsed{};
    if (text == nullptr || text[0] == '\0' || strlen(text) >= WEB_IPV6_TEXT_SIZE || strchr(text, '%') != nullptr ||
        inet_pton(AF_INET6, text, &parsed) != 1)
        return false;
    if (address != nullptr)
        memcpy(address, &parsed, 16);
    return true;
}

bool web_is_valid_time_zone(const char *tz)
{
    if (tz == nullptr || tz[0] == '\0' || strlen(tz) >= WEB_TZ_SIZE || !is_printable_ascii(tz, 0x21, 0x7E))
        return false;
    const char *p = tz;
    if (!tz_name(p) || !tz_time(p, 24))
        return false;
    if (*p == '\0')
        return true;
    if (!tz_name(p))
        return false;
    if (*p != '\0' && *p != ',' && !tz_time(p, 24))
        return false;
    if (*p == '\0')
        return true;
    if (*p++ != ',' || !tz_rule_date(p) || *p++ != ',' || !tz_rule_date(p))
        return false;
    return *p == '\0';
}

bool web_is_reserved_gpio(int gpio)
{
    // Waveshare ESP32-P4-ETH: RMII and SMI of the Ethernet PHY, UART0 (console), strapping pins (34 to 38, partly
    // RMII as well), USB Serial/JTAG and the LCD I2C bus. The TF card pins and the button pin are settings of their
    // own (TF default 39 to 44, the TF slot of the board) and are checked against the other pins by
    // web_settings_validate().
    static constexpr int reserved[] = {
        28, 29, 30, 31, 34, 35, 49, 50, 51, 52, // Ethernet RMII, MDC, MDIO, PHY reset, REF_CLK
        36, 37, 38,                             // strapping; 37/38 = UART0 console
        24, 25,                                 // USB Serial/JTAG
        7, 8,                                   // LCD I2C (SCL, SDA)
    };
    if (gpio < 0 || gpio > 54)
        return true;
    for (int r : reserved)
        if (gpio == r)
            return true;
    return false;
}

bool web_normalise_mac(const char *text, char (&out)[WEB_MAC_TEXT_SIZE])
{
    out[0] = '\0';
    if (text == nullptr)
        return false;
    // Separator positions of the accepted forms: xx:xx:xx:xx:xx:xx / xx-xx-..., xxxx.xxxx.xxxx (Cisco),
    // xxxxxxxxxxxx (no separator)
    const size_t length = strlen(text);
    char separator = '\0';
    size_t group = 0; // hex digits between two separators
    if (length == 17 && (text[2] == ':' || text[2] == '-'))
    {
        separator = text[2];
        group = 2;
    }
    else if (length == 14 && text[4] == '.')
    {
        separator = '.';
        group = 4;
    }
    else if (length != 12)
        return false;

    char digits[12];
    size_t count = 0;
    for (size_t i = 0; i < length; ++i)
    {
        const char c = text[i];
        if (group != 0 && i % (group + 1) == group)
        {
            if (c != separator)
                return false;
            continue;
        }
        if (!isxdigit(static_cast<unsigned char>(c)) || count == sizeof(digits))
            return false;
        digits[count++] = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (count != sizeof(digits))
        return false;
    for (size_t i = 0; i < 6; ++i)
    {
        out[i * 3] = digits[i * 2];
        out[i * 3 + 1] = digits[i * 2 + 1];
        out[i * 3 + 2] = i < 5 ? ':' : '\0';
    }
    return true;
}

bool web_hardware_mac(char (&out)[WEB_MAC_TEXT_SIZE])
{
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) != ESP_OK)
    {
        out[0] = '\0';
        return false;
    }
    snprintf(out, WEB_MAC_TEXT_SIZE, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool web_mqtt_is_mdns_name(const char *host)
{
    const size_t length = strlen(host);
    const size_t end = length > 0 && host[length - 1] == '.' ? length - 1 : length;
    return end >= 6 && strncasecmp(host + end - 6, ".local", 6) == 0;
}

bool web_mqtt_host_valid(const char *host)
{
    if (host == nullptr || host[0] == '\0')
        return false;
    uint8_t v6[16];
    if (web_parse_ipv4(host, nullptr) || web_parse_ipv6(host, v6))
        return true;
    // DNS name (RFC 1123 labels, an optional final dot)
    const size_t length = strlen(host);
    if (length > 253)
        return false;
    size_t label = 0;
    bool all_digits_last = true;
    for (size_t i = 0; i < length; ++i)
    {
        const char c = host[i];
        if (c == '.')
        {
            if (label == 0 || host[i - 1] == '-')
                return false;
            label = 0;
            continue;
        }
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '-')
            return false;
        if (c == '-' && label == 0)
            return false;
        if (label == 0)
            all_digits_last = true;
        all_digits_last = all_digits_last && digit;
        if (++label > 63)
            return false;
    }
    if (label > 0 && host[length - 1] == '-')
        return false;
    // The last label (top-level domain) is not all digits: "1.2.3.400" is a wrong address, not a name
    return !all_digits_last;
}

bool web_mqtt_split_uri(const char *uri, web_settings_t &s)
{
    memset(s.mqtt_broker, 0, sizeof(s.mqtt_broker));
    s.mqtt_port = 0;
    s.mqtt_tls = false;
    const char *host = nullptr;
    if (strncmp(uri, "mqtt://", 7) == 0)
        host = uri + 7;
    else if (strncmp(uri, "mqtts://", 8) == 0)
    {
        host = uri + 8;
        s.mqtt_tls = true;
    }
    else
        return false;
    const char *host_end = nullptr;
    const char *port = nullptr;
    if (host[0] == '[')
    {
        ++host;
        host_end = strchr(host, ']');
        if (host_end != nullptr && host_end[1] == ':')
            port = host_end + 2;
        else if (host_end != nullptr && host_end[1] != '\0')
            host_end = nullptr;
    }
    else
    {
        port = strrchr(host, ':');
        host_end = port != nullptr ? port++ : host + strlen(host);
    }
    long port_value = 0;
    if (port != nullptr)
    {
        char *end = nullptr;
        port_value = strtol(port, &end, 10);
        if (end == port || *end != '\0' || port_value < 1 || port_value > 65535)
            host_end = nullptr;
    }
    const size_t host_length = host_end != nullptr ? static_cast<size_t>(host_end - host) : 0;
    if (host_length > 0 && host_length < sizeof(s.mqtt_broker))
    {
        memcpy(s.mqtt_broker, host, host_length);
        s.mqtt_broker[host_length] = '\0';
    }
    if (s.mqtt_broker[0] == '\0' || !web_mqtt_host_valid(s.mqtt_broker) || web_mqtt_is_mdns_name(s.mqtt_broker))
    {
        memset(s.mqtt_broker, 0, sizeof(s.mqtt_broker));
        s.mqtt_tls = false;
        return false;
    }
    s.mqtt_port = static_cast<uint16_t>(port_value);
    return true;
}

bool web_mqtt_topic_valid(const char *topic)
{
    const size_t length = strnlen(topic, WEB_MQTT_TOPIC_SIZE);
    if (length == 0 || length > WEB_MQTT_TOPIC_SIZE - 1 || topic[0] == '$' || topic[length - 1] == '/')
        return false;
    for (const char *c = topic; *c != '\0'; ++c)
        if (*c == '+' || *c == '#' || static_cast<unsigned char>(*c) < 0x20 || *c == 0x7f)
            return false;
    return true;
}

namespace
{
    // A PEM block with one of the labels at p (after white space): moves p past it; false if it is none or malformed
    bool pem_block(const char *&p, const char *const *labels, size_t label_count)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            ++p;
        for (size_t i = 0; i < label_count; ++i)
        {
            char begin[48];
            char end[48];
            snprintf(begin, sizeof(begin), "-----BEGIN %s-----", labels[i]);
            snprintf(end, sizeof(end), "-----END %s-----", labels[i]);
            if (strncmp(p, begin, strlen(begin)) != 0)
                continue;
            const char *q = p + strlen(begin);
            size_t characters = 0;
            while (strncmp(q, end, strlen(end)) != 0)
            {
                const char c = *q++;
                const bool base64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
                if (c == '\0' || (!base64 && c != '\r' && c != '\n' && c != ' ' && c != '\t'))
                    return false;
                characters += base64 ? 1 : 0;
            }
            if (characters < 16)
                return false;
            p = q + strlen(end);
            return true;
        }
        return false;
    }
} // namespace

bool web_pem_private_key_valid(const char *pem)
{
    static const char *const PARAMETERS[] = {"EC PARAMETERS"};
    static const char *const KEYS[] = {"PRIVATE KEY", "EC PRIVATE KEY", "RSA PRIVATE KEY"};
    const char *p = pem;
    const char *after_parameters = p;
    if (pem_block(after_parameters, PARAMETERS, 1))
        p = after_parameters;
    if (!pem_block(p, KEYS, 3))
        return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    return *p == '\0';
}

bool web_pem_certificates_valid(const char *pem)
{
    static const char BEGIN[] = "-----BEGIN CERTIFICATE-----";
    static const char END[] = "-----END CERTIFICATE-----";
    const char *p = pem;
    size_t blocks = 0;
    for (;;)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            ++p;
        if (*p == '\0')
            return blocks > 0;
        if (strncmp(p, BEGIN, sizeof(BEGIN) - 1) != 0)
            return false;
        p += sizeof(BEGIN) - 1;
        size_t characters = 0;
        while (strncmp(p, END, sizeof(END) - 1) != 0)
        {
            const char c = *p++;
            const bool base64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (c == '\0' || (!base64 && c != '\r' && c != '\n' && c != ' ' && c != '\t'))
                return false;
            characters += base64 ? 1 : 0;
        }
        if (characters < 64)
            return false;
        p += sizeof(END) - 1;
        ++blocks;
    }
}

bool web_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t length)
{
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i)
        difference = difference | (a[i] ^ b[i]);
    return difference == 0;
}

void web_settings_defaults(web_settings_t &out)
{
    memset(&out, 0, sizeof(out)); // no temporary: about 5.5 kB

    // ---- Network: DHCP and SLAAC, both address families, the DNS servers they bring, the hardware MAC ----
    // The FQDN is the host name of the DHCP request and mDNS (<name>.local) and the name of the certificate
    copy_text(out.fqdn, "ESP32TimeServer");
    out.ip_mode = web_ip_mode_t::dual;
    out.ipv4_dhcp = true;
    out.ipv4_prefix = 24;
    out.ipv6_slaac = true;
    out.ipv6_prefix = 64;
    // Requests per second and client IP address; requests over the limit are dropped without an answer. A client is
    // identified by its source address only (chrony uses a new source port for every request, NAT routers rewrite
    // ports), so all clients behind one NAT address share one limit. Why 20:
    // - The worst-case single client is chrony with "minpoll -4 maxpoll -4": one request every 62.5 ms, i.e. 16 per
    //   second. iburst / burst only shorten intervals to 2 s or less and the interleaved mode sends no extra
    //   requests, so neither raises this rate.
    // - The limit allows a burst of one second worth of requests, which absorbs network jitter. It cannot absorb a
    //   lasting rate difference: a limit of exactly 16 would sooner or later drop requests of a 16/s client whose
    //   clock runs slightly fast compared to this server's (chrony slews by up to 83333 ppm). The 25 % margin covers
    //   that with room to spare.
    // - Raise it only if many clients share one NAT address; newer chrony versions also accept minpoll -6 or -7 (64
    //   or 128 requests per second), such clients are limited to this rate.
    out.ntp_client_rate_limit = DEFAULT_NTP_CLIENT_RATE_LIMIT;

    // ---- GNSS and PPS: NMEA at 921600 baud, the pins of the GNSS header wiring, PTP PPS loopback on GPIO 23 ----
    // The baud rate only decides how soon the label of a second arrives after the PPS edge (within 950 ms); the
    // accuracy comes from the PPS. 9600 baud still works with a single RMC sentence per second. The loopback pin is
    // driven as an output inside the pad (nothing wired), it must be free; it needs an ESP32-P4 of revision v3.0 or
    // newer.
    out.gnss_ublox = false;
    out.gnss_tx_pin = 22;
    out.gnss_rx_pin = 21;
    out.gnss_pps_pin = 20;
    out.gnss_baud = 921600;
    out.ptp_loopback_pin = 23;
    // PPS holdover: the PPS counts as lost after 3.5 s without a pulse; for this long after that the server keeps
    // answering at stratum 1, coasting on the learned rate, with the root dispersion growing by 15 ppm (the frequency
    // tolerance PHI of RFC 5905; the board test showed about 1 ppb, so this is conservative). A short holdover makes
    // the clients switch to other servers soon.
    out.pps_holdover_s = DEFAULT_PPS_HOLDOVER_S;
    out.pps_dispersion_ppm = DEFAULT_PPS_DISPERSION_PPM;
    // After the holdover: keep answering for 60 min after the loss at stratum 16 ("not synchronised"), so the clients
    // stop using the server
    out.pps_lost_stratum = DEFAULT_PPS_LOST_STRATUM;
    out.pps_lost_serve = DEFAULT_PPS_LOST_SERVE;
    out.pps_lost_serve_min = DEFAULT_PPS_LOST_SERVE_MIN;
    // Off: 750 s (the 12.5 min of the UTC parameters) is recommended for a receiver without backup battery
    out.gnss_min_fix_age_s = DEFAULT_GNSS_MIN_FIX_AGE_S;
    out.gnss_min_satellites = DEFAULT_GNSS_MIN_SATELLITES;

    // ---- LCD Display ----
    out.lcd_enabled = false;
    copy_text(out.lcd_title, "ESP32 Time Server");
    out.lcd_24_hour = true;
    out.lcd_pad_hour = true;
    out.lcd_show_time_zone = true;
    out.lcd_address = web_lcd_address_t::both;
    copy_text(out.time_zone, "CET-1CEST,M3.5.0,M10.5.0/3");
    out.button_enabled = DEFAULT_BUTTON_ENABLED;
    out.button_pin = DEFAULT_BUTTON_PIN;

    // ---- RGB LED ----
    out.led_type = web_led_type_t::none;
    out.led_brightness_percent = 70;
    out.led_red_pin = 6;
    out.led_green_pin = 5;
    out.led_blue_pin = 4;
    out.led_ws2812_pin = 4;
    out.led_rgb[static_cast<size_t>(web_led_state_t::startup)] = 0x0000FF;  // blue
    out.led_rgb[static_cast<size_t>(web_led_state_t::normal)] = 0x00FF00;   // green
    out.led_rgb[static_cast<size_t>(web_led_state_t::sync)] = 0xFFFFFF;     // white
    out.led_rgb[static_cast<size_t>(web_led_state_t::warning)] = 0xFFFF00;  // yellow
    out.led_rgb[static_cast<size_t>(web_led_state_t::alert)] = 0xFFA500;    // orange
    out.led_rgb[static_cast<size_t>(web_led_state_t::critical)] = 0xFF0000; // red
    out.led_rgb[static_cast<size_t>(web_led_state_t::button)] = 0xFFC0CB;   // pink

    // ---- MQTT (off; no broker, port 0 = 8883 with TLS, 1883 without) and the TF card slot of the Waveshare ESP32-P4
    // Ethernet boards ----
    out.mqtt_enabled = false;
    out.mqtt_port = 0;
    out.mqtt_tls = false;
    copy_text(out.mqtt_topic, DEFAULT_MQTT_TOPIC);
    out.mqtt_qos = 0;
    out.mqtt_retain = true;
    out.mqtt_report_period_s = 900;
    out.mqtt_tf_client_limit = 500;
    out.tf_cmd_pin = 44;
    out.tf_clk_pin = 43;
    out.tf_d0_pin = 39;
    out.tf_d1_pin = 40;
    out.tf_d2_pin = 41;
    out.tf_d3_pin = 42;

    // ---- Over The Ethernet updates and NTP authentication: off, no keys ----
    out.ote_enabled = false;
    out.ntp_auth_enabled = false;
    out.ntp_key_override = true;
    out.ntp_key_count = 0;

    // ---- PTP master: off ----
    ptp_defaults(out);
}

void web_settings_load_boot()
{
    if (s_mutex == nullptr)
        s_mutex = xSemaphoreCreateMutex();

    web_settings_defaults(s_boot);
    const char *source = "defaults";

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        static settings_blob_t blob; // static: about 5.5 kB, the caller (app_main) has a small stack
        char error[96] = "";
        bool had_ote_fields = false;
        if (read_blob(handle, NVS_KEY_SAVED, blob, &had_ote_fields))
        {
            if (had_ote_fields)
            {
                // Written back without the former OTE password (and device name), so that it does not stay in NVS.
                // read_blob() has zeroed both, brought an older set to the current version and renewed the CRC.
                esp_err_t scrub_err = nvs_set_blob(handle, NVS_KEY_SAVED, &blob, sizeof(blob));
                if (scrub_err == ESP_OK)
                    scrub_err = nvs_commit(handle);
                if (scrub_err == ESP_OK)
                    ESP_LOGI(TAG, "Saved web settings: the former OTE password and device name were erased");
                else
                    ESP_LOGW(TAG, "Saved web settings: the former OTE password could not be erased (%s)", esp_err_to_name(scrub_err));
            }
            if (web_settings_validate(blob.settings, error, sizeof(error)))
            {
                s_boot = blob.settings;
                source = "saved settings (NVS)";
            }
            else
                ESP_LOGW(TAG, "Saved web settings are not valid for this firmware (%s), the defaults are used", error);
        }

        // The one-shot set is erased before it is used: if it makes the device crash, the next boot is back on the
        // saved settings
        if (read_blob(handle, NVS_KEY_ONESHOT, blob))
        {
            const esp_err_t erase_err = nvs_erase_key(handle, NVS_KEY_ONESHOT);
            const esp_err_t commit_err = erase_err == ESP_OK ? nvs_commit(handle) : erase_err;
            if (commit_err != ESP_OK)
                ESP_LOGE(TAG, "One-shot settings could not be erased (%s), they are not used", esp_err_to_name(commit_err));
            else if (web_settings_validate(blob.settings, error, sizeof(error)))
            {
                s_boot = blob.settings;
                s_boot_oneshot = true;
                s_boot_network_confirm = (blob.flags & 1u) != 0;
                source = "one-shot settings of the web admin (valid for this boot only)";
            }
            else
                ESP_LOGW(TAG, "One-shot web settings are not valid (%s), ignored", error);
        }
        else
        {
            // remove an unusable one-shot entry, if any
            if (nvs_erase_key(handle, NVS_KEY_ONESHOT) == ESP_OK)
                nvs_commit(handle);
        }
        nvs_close(handle);
    }
    else
        ESP_LOGW(TAG, "NVS namespace %s unavailable (%s), the defaults are used", NVS_NAMESPACE, esp_err_to_name(err));

    s_running = s_boot;
    ESP_LOGI(TAG, "Settings: %s", source);
}

const web_settings_t &web_settings_boot()
{
    return s_boot;
}

bool web_settings_boot_was_oneshot(bool *network_confirm_required)
{
    if (network_confirm_required != nullptr)
        *network_confirm_required = s_boot_network_confirm;
    return s_boot_oneshot;
}

void web_settings_get_running(web_settings_t &out)
{
    if (s_mutex != nullptr)
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    out = s_running;
    if (s_mutex != nullptr)
        xSemaphoreGive(s_mutex);
}

void web_settings_set_running(const web_settings_t &in)
{
    if (s_mutex != nullptr)
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_running = in;
    if (s_mutex != nullptr)
        xSemaphoreGive(s_mutex);
}

bool web_settings_get_saved(web_settings_t &out)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;
    settings_blob_t *blob = new (std::nothrow) settings_blob_t{};
    bool ok = blob != nullptr && read_blob(handle, NVS_KEY_SAVED, *blob);
    if (ok)
        out = blob->settings;
    delete blob;
    nvs_close(handle);
    return ok;
}

esp_err_t web_settings_save(const web_settings_t &in)
{
    return write_blob(NVS_KEY_SAVED, in, 0);
}

esp_err_t web_settings_save_oneshot(const web_settings_t &in, bool network_confirm_required)
{
    return write_blob(NVS_KEY_ONESHOT, in, network_confirm_required ? 1u : 0u);
}

esp_err_t web_settings_factory_reset()
{
    esp_err_t result = ESP_OK;
    for (const char *ns : {NVS_NAMESPACE, AUTH_NVS_NAMESPACE})
    {
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            continue;
        if (err == ESP_OK)
        {
            err = nvs_erase_all(handle);
            if (err == ESP_OK)
                err = nvs_commit(handle);
            nvs_close(handle);
        }
        if (err != ESP_OK)
            result = err;
    }
    return result;
}

void web_settings_hostname(const web_settings_t &s, char *out, size_t out_size)
{
    if (out_size == 0)
        return;
    size_t length = strcspn(s.fqdn, ".");
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, s.fqdn, length);
    out[length] = '\0';
}

bool web_settings_validate(const web_settings_t &s, char *err, size_t err_size)
{
#define FAIL(...)                                \
    do                                           \
    {                                            \
        if (err != nullptr && err_size > 0)      \
            snprintf(err, err_size, __VA_ARGS__); \
        return false;                            \
    } while (0)

    // ---- FQDN: host name label plus optional domain, RFC 1123 ----
    {
        const size_t length = strnlen(s.fqdn, WEB_FQDN_SIZE);
        if (length == 0 || length >= WEB_FQDN_SIZE)
            FAIL("FQDN: 1 to 253 characters");
        if (web_parse_ipv4(s.fqdn, nullptr))
            FAIL("FQDN: must not be an IP address");
        size_t label_length = 0;
        size_t label_index = 0;
        bool all_digits = true;
        char previous = '.';
        for (size_t i = 0; i <= length; ++i)
        {
            const char c = s.fqdn[i];
            if (c == '.' || c == '\0')
            {
                if (label_length == 0 || label_length > 63 || previous == '-')
                    FAIL("FQDN: every label needs 1 to 63 characters and must not end with '-'");
                if (label_index == 0 && label_length > WEB_HOSTNAME_MAX_LEN)
                    FAIL("FQDN: the host name (first label) may have at most %u characters", static_cast<unsigned>(WEB_HOSTNAME_MAX_LEN));
                if (c == '\0' && label_index > 0 && all_digits)
                    FAIL("FQDN: the top-level domain must not be numeric");
                ++label_index;
                label_length = 0;
                all_digits = true;
            }
            else
            {
                if (!isalnum(static_cast<unsigned char>(c)) && c != '-')
                    FAIL("FQDN: only letters, digits, '-' and '.' are allowed");
                if (c == '-' && label_length == 0)
                    FAIL("FQDN: a label must not start with '-'");
                if (!isdigit(static_cast<unsigned char>(c)))
                    all_digits = false;
                ++label_length;
            }
            previous = c;
        }
    }

    if (s.ip_mode != web_ip_mode_t::dual && s.ip_mode != web_ip_mode_t::ipv4 && s.ip_mode != web_ip_mode_t::ipv6)
        FAIL("Address families: dual, ipv4 or ipv6 expected");

    // ---- IPv4 ----
    if (s.ip_mode != web_ip_mode_t::ipv6 && !s.ipv4_dhcp)
    {
        uint32_t address_be = 0;
        uint32_t gateway_be = 0;
        if (!web_parse_ipv4(s.ipv4_address, &address_be))
            FAIL("IPv4 address is not valid");
        if (s.ipv4_prefix < 8 || s.ipv4_prefix > 30)
            FAIL("IPv4 prefix length: 8 to 30");
        const uint32_t address = ntohl(address_be);
        const uint32_t mask = 0xFFFFFFFFu << (32 - s.ipv4_prefix);
        const uint8_t first = static_cast<uint8_t>(address >> 24);
        if (first == 0 || first == 127 || first >= 224 || (first == 169 && static_cast<uint8_t>(address >> 16) == 254))
            FAIL("IPv4 address: not a usable unicast address");
        if ((address & ~mask) == 0 || (address & ~mask) == ~mask)
            FAIL("IPv4 address must not be the network or broadcast address");
        if (!web_parse_ipv4(s.ipv4_gateway, &gateway_be))
            FAIL("IPv4 gateway is not valid");
        const uint32_t gateway = ntohl(gateway_be);
        if ((gateway & mask) != (address & mask))
            FAIL("IPv4 gateway must be in the subnet of the address");
        if (gateway == address || (gateway & ~mask) == 0 || (gateway & ~mask) == ~mask)
            FAIL("IPv4 gateway must be another host address of the subnet");
    }

    // ---- IPv6 static address ----
    if (s.ip_mode != web_ip_mode_t::ipv4 && s.ipv6_address[0] != '\0')
    {
        uint8_t address[16];
        if (!web_parse_ipv6(s.ipv6_address, address))
            FAIL("IPv6 address is not valid");
        static constexpr uint8_t zero[16] = {};
        static constexpr uint8_t v4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (address[0] == 0xFF || (address[0] == 0xFE && (address[1] & 0xC0) == 0x80) ||
            memcmp(address, zero, 15) == 0 || memcmp(address, v4_mapped, 12) == 0)
            FAIL("IPv6 address: a global or unique local unicast address is required");
        // lwIP decides on-link with the first 64 bits, so shorter prefixes are not supported
        if (s.ipv6_prefix < 64 || s.ipv6_prefix > 128)
            FAIL("IPv6 prefix length: 64 to 128");
        if (s.ipv6_gateway[0] != '\0')
        {
            uint8_t gateway[16];
            if (!web_parse_ipv6(s.ipv6_gateway, gateway))
                FAIL("IPv6 gateway is not valid");
            const bool link_local = gateway[0] == 0xFE && (gateway[1] & 0xC0) == 0x80;
            const bool same_64 = memcmp(gateway, address, 8) == 0;
            if (gateway[0] == 0xFF || memcmp(gateway, zero, 16) == 0 || memcmp(gateway, address, 16) == 0 || (!link_local && !same_64))
                FAIL("IPv6 gateway must be a link-local address or in the /64 of the address");
        }
    }
    else if (s.ip_mode != web_ip_mode_t::ipv4 && s.ipv6_gateway[0] != '\0')
        FAIL("IPv6 gateway requires a static IPv6 address");
    else if (s.ipv6_prefix < 64 || s.ipv6_prefix > 128)
        FAIL("IPv6 prefix length: 64 to 128");

    // ---- DNS ----
    for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
    {
        if (s.dns[i][0] == '\0')
            continue;
        uint32_t v4 = 0;
        uint8_t v6[16];
        if (web_parse_ipv4(s.dns[i], &v4))
        {
            const uint8_t first = static_cast<uint8_t>(ntohl(v4) >> 24);
            if (first == 0 || first >= 224)
                FAIL("DNS server %u: not a unicast address", static_cast<unsigned>(i + 1));
        }
        else if (web_parse_ipv6(s.dns[i], v6))
        {
            if (v6[0] == 0xFF)
                FAIL("DNS server %u: not a unicast address", static_cast<unsigned>(i + 1));
        }
        else
            FAIL("DNS server %u is not an IPv4 or IPv6 address", static_cast<unsigned>(i + 1));
    }

    // ---- MAC address ----
    if (s.mac_address[0] != '\0')
    {
        char normalised[WEB_MAC_TEXT_SIZE];
        if (!web_normalise_mac(s.mac_address, normalised) || strcmp(normalised, s.mac_address) != 0)
            FAIL("MAC address: xx:xx:xx:xx:xx:xx (hexadecimal) expected");
        const unsigned first = static_cast<unsigned>(strtoul(s.mac_address, nullptr, 16));
        if ((first & 0x01u) != 0)
            FAIL("MAC address: a unicast address is required (the lowest bit of the first byte must be 0)");
        if (strcmp(s.mac_address, "00:00:00:00:00:00") == 0)
            FAIL("MAC address: 00:00:00:00:00:00 cannot be used");
    }

    // ---- NTP clients ----
    if (s.ntp_client_rate_limit > WEB_NTP_RATE_LIMIT_MAX)
        FAIL("Rate limit per client: 0 (no limit) to %u requests per second", static_cast<unsigned>(WEB_NTP_RATE_LIMIT_MAX));

    // ---- GPIOs ----
    struct pin_use_t
    {
        int pin;
        const char *name;
    };
    pin_use_t pins[15];
    size_t pin_count = 0;
    auto add_pin = [&](int pin, const char *name) -> bool
    {
        if (web_is_reserved_gpio(pin))
            return false;
        pins[pin_count++] = {pin, name};
        return true;
    };
    if (!add_pin(s.gnss_tx_pin, "GNSS TX"))
        FAIL("GNSS TX pin: GPIO %d cannot be used", s.gnss_tx_pin);
    if (!add_pin(s.gnss_rx_pin, "GNSS RX"))
        FAIL("GNSS RX pin: GPIO %d cannot be used", s.gnss_rx_pin);
    if (!add_pin(s.gnss_pps_pin, "GNSS PPS"))
        FAIL("PPS pin: GPIO %d cannot be used", s.gnss_pps_pin);
    if (s.ptp_loopback_pin != -1 && !add_pin(s.ptp_loopback_pin, "PTP PPS loopback"))
        FAIL("PTP PPS loopback pin: GPIO %d cannot be used", s.ptp_loopback_pin);
    if (s.button_enabled && !add_pin(s.button_pin, "Button"))
        FAIL("Button pin: GPIO %d cannot be used", s.button_pin);
    // TF card: always taken (on the Waveshare boards the slot is wired to these pins)
    if (!add_pin(s.tf_cmd_pin, "TF CMD") || !add_pin(s.tf_clk_pin, "TF CLK") || !add_pin(s.tf_d0_pin, "TF D0") ||
        !add_pin(s.tf_d1_pin, "TF D1") || !add_pin(s.tf_d2_pin, "TF D2") || !add_pin(s.tf_d3_pin, "TF D3"))
        FAIL("TF card pins: one of the GPIOs cannot be used");
    switch (s.led_type)
    {
    case web_led_type_t::none:
        break;
    case web_led_type_t::ky016:
        if (!add_pin(s.led_red_pin, "LED red") || !add_pin(s.led_green_pin, "LED green") || !add_pin(s.led_blue_pin, "LED blue"))
            FAIL("KY-016 LED pins: one of the GPIOs cannot be used");
        break;
    case web_led_type_t::ws2812:
        if (!add_pin(s.led_ws2812_pin, "WS2812 data"))
            FAIL("WS2812 data pin: GPIO %d cannot be used", s.led_ws2812_pin);
        break;
    default:
        FAIL("LED type is not valid");
    }
    for (size_t i = 0; i < pin_count; ++i)
        for (size_t j = i + 1; j < pin_count; ++j)
            if (pins[i].pin == pins[j].pin)
                FAIL("GPIO %d is used twice (%s, %s)", pins[i].pin, pins[i].name, pins[j].name);

    static constexpr uint32_t bauds[] = {4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    bool baud_ok = false;
    for (uint32_t b : bauds)
        baud_ok = baud_ok || b == s.gnss_baud;
    if (!baud_ok)
        FAIL("GNSS baud rate is not one of the supported values");
    if (s.gnss_min_satellites > WEB_GNSS_MIN_SATELLITES_MAX)
        FAIL("Minimum satellites in use: 0 (off) to %u", static_cast<unsigned>(WEB_GNSS_MIN_SATELLITES_MAX));
    if (s.gnss_min_fix_age_s > WEB_GNSS_MIN_FIX_AGE_MAX_S)
        FAIL("Minimum GNSS fix age: 0 (off) to %u seconds", static_cast<unsigned>(WEB_GNSS_MIN_FIX_AGE_MAX_S));
    if (s.pps_holdover_s > WEB_PPS_HOLDOVER_MAX_S)
        FAIL("PPS holdover: 0 to %u seconds", static_cast<unsigned>(WEB_PPS_HOLDOVER_MAX_S));
    if (s.pps_lost_stratum < 2 || s.pps_lost_stratum > 16)
        FAIL("Stratum after the holdover: 2 to 16");
    if (s.pps_lost_serve_min > WEB_PPS_LOST_SERVE_MAX_MIN)
        FAIL("Serving time after the PPS loss: 0 (no limit) to %u minutes", static_cast<unsigned>(WEB_PPS_LOST_SERVE_MAX_MIN));
    if (s.pps_dispersion_ppm < 1 || s.pps_dispersion_ppm > WEB_PPS_DISPERSION_MAX_PPM)
        FAIL("Root dispersion growth: 1 to %u ppm (0.001 to 10 ms per second)", static_cast<unsigned>(WEB_PPS_DISPERSION_MAX_PPM));

    // ---- LCD Display ----
    {
        const size_t length = strnlen(s.lcd_title, WEB_LCD_TITLE_SIZE);
        if (length == 0 || length >= WEB_LCD_TITLE_SIZE || !is_printable_ascii(s.lcd_title, 0x20, 0x7D))
            FAIL("LCD title: 1 to 17 printable ASCII characters");
    }
    if (s.lcd_address > web_lcd_address_t::none)
        FAIL("LCD address line is not valid");
    if (!web_is_valid_time_zone(s.time_zone))
        FAIL("Time zone is not a valid POSIX TZ string");
    if (s.led_brightness_percent > 100)
        FAIL("LED brightness: 0 to 100 %%");
    for (size_t i = 0; i < WEB_LED_STATE_COUNT; ++i)
        if (s.led_rgb[i] > 0xFFFFFFu)
            FAIL("LED colour %u is not a 24-bit RGB value", static_cast<unsigned>(i + 1));

    // ---- MQTT ----
    {
        if (memchr(s.mqtt_broker, '\0', sizeof(s.mqtt_broker)) == nullptr || memchr(s.mqtt_ca_pem, '\0', sizeof(s.mqtt_ca_pem)) == nullptr ||
            memchr(s.mqtt_topic, '\0', sizeof(s.mqtt_topic)) == nullptr || memchr(s.mqtt_username, '\0', sizeof(s.mqtt_username)) == nullptr ||
            memchr(s.mqtt_password, '\0', sizeof(s.mqtt_password)) == nullptr)
            FAIL("MQTT: text is not terminated");
        if (s.mqtt_broker[0] != '\0' && !web_mqtt_host_valid(s.mqtt_broker))
            FAIL("MQTT broker: a host name or an IPv4 or IPv6 address expected");
        if (web_mqtt_is_mdns_name(s.mqtt_broker))
            FAIL("MQTT broker: .local names (mDNS) are not supported; use an address or a DNS name");
        if (s.mqtt_enabled && s.mqtt_broker[0] == '\0')
            FAIL("MQTT broker: required while MQTT is on");
        if (!web_mqtt_topic_valid(s.mqtt_topic))
            FAIL("MQTT topic: 1 to 100 characters without +, #, control characters, a leading $ or a trailing /");
        if (!is_printable_ascii(s.mqtt_username, 0x20, 0x7E))
            FAIL("MQTT user name: at most 64 printable ASCII characters");
        if (!is_printable_ascii(s.mqtt_password, 0x20, 0x7E))
            FAIL("MQTT password: at most 64 printable ASCII characters");
        if (s.mqtt_ca_pem[0] != '\0' && !web_pem_certificates_valid(s.mqtt_ca_pem))
            FAIL("MQTT CA certificate: PEM with one or more BEGIN CERTIFICATE / END CERTIFICATE blocks expected");
        if (s.mqtt_enabled && s.mqtt_tls && s.mqtt_ca_pem[0] == '\0')
            FAIL("MQTT: TLS needs the CA certificate of the broker (PEM)");
        if (s.mqtt_qos > 2)
            FAIL("MQTT QoS: 0, 1 or 2");
        if (s.mqtt_report_period_s < 30 || s.mqtt_report_period_s > 86400)
            FAIL("MQTT reporting period: 30 to 86400 s");
        if (s.mqtt_tf_client_limit < WEB_MQTT_TF_CLIENT_LIMIT_MIN || s.mqtt_tf_client_limit > WEB_MQTT_TF_CLIENT_LIMIT_MAX)
            FAIL("MQTT client limit with TF card: %u to %u", static_cast<unsigned>(WEB_MQTT_TF_CLIENT_LIMIT_MIN),
                 static_cast<unsigned>(WEB_MQTT_TF_CLIENT_LIMIT_MAX));
    }

    // ---- NTP authentication ----
    if (s.ntp_key_count > WEB_NTP_MAX_KEYS)
        FAIL("NTP keys: at most %u", static_cast<unsigned>(WEB_NTP_MAX_KEYS));
    if (s.ntp_auth_enabled && s.ntp_key_count == 0)
        FAIL("NTP authentication needs at least one key");
    for (size_t i = 0; i < s.ntp_key_count; ++i)
    {
        const web_ntp_key_t &key = s.ntp_keys[i];
        if (key.id < 1 || key.id > 65535)
            FAIL("NTP key %u: key ID 1 to 65535", static_cast<unsigned>(i + 1));
        for (size_t k = 0; k < i; ++k)
            if (s.ntp_keys[k].id == key.id)
                FAIL("NTP key ID %lu is used twice", static_cast<unsigned long>(key.id));
        if (strnlen(key.key, WEB_NTP_KEY_SIZE) != 32)
            FAIL("NTP key %u: exactly 32 characters", static_cast<unsigned>(i + 1));
        for (size_t c = 0; c < 32; ++c)
            if (!isalnum(static_cast<unsigned char>(key.key[c])))
                FAIL("NTP key %u: letters and digits only", static_cast<unsigned>(i + 1));
    }

    // ---- PTP master ----
    if (s.ptp_domain > WEB_PTP_DOMAIN_MAX)
        FAIL("PTP domain: 0 to %u", static_cast<unsigned>(WEB_PTP_DOMAIN_MAX));
    if (s.ptp_log_sync_interval < WEB_PTP_LOG_SYNC_MIN || s.ptp_log_sync_interval > WEB_PTP_LOG_SYNC_MAX)
        FAIL("PTP sync interval: 2^%d to 2^%d s", WEB_PTP_LOG_SYNC_MIN, WEB_PTP_LOG_SYNC_MAX);
    if (s.ptp_log_announce_interval < WEB_PTP_LOG_ANNOUNCE_MIN || s.ptp_log_announce_interval > WEB_PTP_LOG_ANNOUNCE_MAX)
        FAIL("PTP announce interval: 2^%d to 2^%d s", WEB_PTP_LOG_ANNOUNCE_MIN, WEB_PTP_LOG_ANNOUNCE_MAX);
    if (s.ptp_log_min_delay_req_interval < WEB_PTP_LOG_DELAY_REQ_MIN || s.ptp_log_min_delay_req_interval > WEB_PTP_LOG_DELAY_REQ_MAX)
        FAIL("PTP minimum delay request interval: 2^%d to 2^%d s", WEB_PTP_LOG_DELAY_REQ_MIN, WEB_PTP_LOG_DELAY_REQ_MAX);
    if ((s.ptp_clock_accuracy < 0x20 || s.ptp_clock_accuracy > 0x31) && s.ptp_clock_accuracy != 0xFE)
        FAIL("PTP clock accuracy: 0x20 to 0x31 or 0xFE");
    if (s.ptp_follower_timeout_s < WEB_PTP_FOLLOWER_TIMEOUT_MIN_S || s.ptp_follower_timeout_s > WEB_PTP_FOLLOWER_TIMEOUT_MAX_S)
        FAIL("PTP follower timeout: %u to %u s", static_cast<unsigned>(WEB_PTP_FOLLOWER_TIMEOUT_MIN_S),
             static_cast<unsigned>(WEB_PTP_FOLLOWER_TIMEOUT_MAX_S));
    if (s.ptp_delay_req_rate_limit > WEB_PTP_DELAY_REQ_RATE_MAX)
        FAIL("PTP delay requests per follower: 0 (no limit) to %u per second", static_cast<unsigned>(WEB_PTP_DELAY_REQ_RATE_MAX));
    if (s.ptp_utc_offset_s < 0 || s.ptp_utc_offset_s > WEB_PTP_UTC_OFFSET_MAX_S)
        FAIL("PTP TAI - UTC: 0 to %d s", WEB_PTP_UTC_OFFSET_MAX_S);
    if (s.ptp_ingress_latency_ns < -WEB_PTP_LATENCY_MAX_NS || s.ptp_ingress_latency_ns > WEB_PTP_LATENCY_MAX_NS ||
        s.ptp_egress_latency_ns < -WEB_PTP_LATENCY_MAX_NS || s.ptp_egress_latency_ns > WEB_PTP_LATENCY_MAX_NS)
        FAIL("PTP ingress / egress latency: -%ld to %ld ns", static_cast<long>(WEB_PTP_LATENCY_MAX_NS),
             static_cast<long>(WEB_PTP_LATENCY_MAX_NS));
    if (s.ptp_udp4_enabled && s.ip_mode == web_ip_mode_t::ipv6)
        FAIL("PTP over UDP/IPv4 needs IPv4 (address families)");
    if (s.ptp_udp6_enabled && s.ip_mode == web_ip_mode_t::ipv4)
        FAIL("PTP over UDP/IPv6 needs IPv6 (address families)");

#undef FAIL
    return true;
}

bool web_settings_network_differs(const web_settings_t &a, const web_settings_t &b)
{
    if (a.ipv4_dhcp != b.ipv4_dhcp || a.ipv6_slaac != b.ipv6_slaac || strcmp(a.ipv6_address, b.ipv6_address) != 0 ||
        strcmp(a.ipv6_gateway, b.ipv6_gateway) != 0 || (a.ipv6_address[0] != '\0' && a.ipv6_prefix != b.ipv6_prefix))
        return true;
    if (!a.ipv4_dhcp && (strcmp(a.ipv4_address, b.ipv4_address) != 0 || a.ipv4_prefix != b.ipv4_prefix ||
                         strcmp(a.ipv4_gateway, b.ipv4_gateway) != 0))
        return true;
    for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
        if (strcmp(a.dns[i], b.dns[i]) != 0)
            return true;
    // The MAC and the address families need a restart; addresses may change with them, so they have to be confirmed
    // as well
    return strcmp(a.mac_address, b.mac_address) != 0 || a.ip_mode != b.ip_mode;
}

bool web_settings_restart_fields_differ(const web_settings_t &a, const web_settings_t &b)
{
    return strcmp(a.fqdn, b.fqdn) != 0 || a.gnss_ublox != b.gnss_ublox ||
           a.gnss_tx_pin != b.gnss_tx_pin || a.gnss_rx_pin != b.gnss_rx_pin || a.gnss_pps_pin != b.gnss_pps_pin ||
           a.gnss_baud != b.gnss_baud || a.ptp_loopback_pin != b.ptp_loopback_pin ||
           a.lcd_enabled != b.lcd_enabled ||
           a.led_type != b.led_type || a.led_red_pin != b.led_red_pin || a.led_green_pin != b.led_green_pin ||
           a.led_blue_pin != b.led_blue_pin || a.led_ws2812_pin != b.led_ws2812_pin ||
           strcmp(a.mac_address, b.mac_address) != 0 || a.ip_mode != b.ip_mode ||
           a.tf_cmd_pin != b.tf_cmd_pin || a.tf_clk_pin != b.tf_clk_pin || a.tf_d0_pin != b.tf_d0_pin ||
           a.tf_d1_pin != b.tf_d1_pin || a.tf_d2_pin != b.tf_d2_pin || a.tf_d3_pin != b.tf_d3_pin ||
           a.ote_enabled != b.ote_enabled || a.button_enabled != b.button_enabled || a.button_pin != b.button_pin;
}

bool web_settings_equal(const web_settings_t &a, const web_settings_t &b)
{
    if (web_settings_network_differs(a, b) || web_settings_restart_fields_differ(a, b))
        return false;
    return strcmp(a.lcd_title, b.lcd_title) == 0 && a.lcd_24_hour == b.lcd_24_hour && a.lcd_pad_hour == b.lcd_pad_hour &&
           a.lcd_show_time_zone == b.lcd_show_time_zone && a.lcd_address == b.lcd_address && strcmp(a.time_zone, b.time_zone) == 0 &&
           a.led_brightness_percent == b.led_brightness_percent && memcmp(a.led_rgb, b.led_rgb, sizeof(a.led_rgb)) == 0 &&
           a.pps_holdover_s == b.pps_holdover_s && a.pps_lost_stratum == b.pps_lost_stratum &&
           a.pps_lost_serve == b.pps_lost_serve && a.pps_lost_serve_min == b.pps_lost_serve_min &&
           a.pps_dispersion_ppm == b.pps_dispersion_ppm && a.ntp_client_rate_limit == b.ntp_client_rate_limit &&
           a.gnss_min_fix_age_s == b.gnss_min_fix_age_s && a.gnss_min_satellites == b.gnss_min_satellites &&
           a.ntp_auth_enabled == b.ntp_auth_enabled &&
           // MQTT takes effect at once (the service task reconnects)
           a.mqtt_enabled == b.mqtt_enabled && strcmp(a.mqtt_broker, b.mqtt_broker) == 0 && a.mqtt_port == b.mqtt_port &&
           a.mqtt_tls == b.mqtt_tls && strcmp(a.mqtt_ca_pem, b.mqtt_ca_pem) == 0 && strcmp(a.mqtt_username, b.mqtt_username) == 0 &&
           web_constant_time_equal(reinterpret_cast<const uint8_t *>(a.mqtt_password), reinterpret_cast<const uint8_t *>(b.mqtt_password),
                                   WEB_MQTT_CREDENTIAL_SIZE) &&
           strcmp(a.mqtt_topic, b.mqtt_topic) == 0 && a.mqtt_qos == b.mqtt_qos && a.mqtt_retain == b.mqtt_retain &&
           a.mqtt_report_period_s == b.mqtt_report_period_s && a.mqtt_client_reporting == b.mqtt_client_reporting &&
           a.mqtt_memory_reporting == b.mqtt_memory_reporting && a.mqtt_historical_reporting == b.mqtt_historical_reporting &&
           a.mqtt_distinguish_auth == b.mqtt_distinguish_auth && a.mqtt_tf_client_limit == b.mqtt_tf_client_limit &&
           a.ntp_key_count == b.ntp_key_count &&
           web_constant_time_equal(reinterpret_cast<const uint8_t *>(a.ntp_keys), reinterpret_cast<const uint8_t *>(b.ntp_keys), sizeof(a.ntp_keys)) &&
           // PTP master: everything takes effect at once
           a.ptp_l2_enabled == b.ptp_l2_enabled && a.ptp_udp4_enabled == b.ptp_udp4_enabled &&
           a.ptp_udp6_enabled == b.ptp_udp6_enabled && a.ptp_domain == b.ptp_domain && a.ptp_priority1 == b.ptp_priority1 &&
           a.ptp_priority2 == b.ptp_priority2 && a.ptp_log_sync_interval == b.ptp_log_sync_interval &&
           a.ptp_log_announce_interval == b.ptp_log_announce_interval &&
           a.ptp_log_min_delay_req_interval == b.ptp_log_min_delay_req_interval &&
           a.ptp_master_selection == b.ptp_master_selection && a.ptp_clock_accuracy == b.ptp_clock_accuracy &&
           a.ptp_follower_timeout_s == b.ptp_follower_timeout_s && a.ptp_delay_req_rate_limit == b.ptp_delay_req_rate_limit &&
           a.ptp_utc_offset_s == b.ptp_utc_offset_s && a.ptp_ingress_latency_ns == b.ptp_ingress_latency_ns &&
           a.ptp_egress_latency_ns == b.ptp_egress_latency_ns;
}
