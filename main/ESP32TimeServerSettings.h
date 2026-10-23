// ESP32 Time Server v3.0.5-WebIf
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Note: some additional settings are defined in the ESP32TimeServerSettingsSecrets.h file
//
// Web admin page (https://<address>/, TCP 443, see WEB_ADMIN_ENABLED below): the network settings, the GNSS protocol
// (UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED), the GNSS and PPS pins, the GNSS baud rate, the PTP PPS loopback pin, the
// LCD, time zone, RGB LED (including the LED_* colours), MQTT, NTP authentication and Over The Ethernet update
// settings below (OTE password in ESP32TimeServerSettingsSecrets.h) can also be
// changed on the page. Settings saved there (NVS) take precedence over the values in this file; the values here
// are the defaults and are used again after a factory reset. The features LCD, RGB LED, MQTT, NTP
// authentication and OTE updates are always compiled in: their *_ENABLED values below only decide whether they are on
// by default.
// DeviceName is the default host name (FQDN) of the page and the default mDNS host name (<name>.local).
//

#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include "custom/setting_types.c"
#include "ESP32TimeServerSettingsSecrets.h"

// (optional) debug support
// NOTE: setting DEBUG_ENABLED to 1 (Enabled) will degrade accuracy and performance during periods of high volume ntp requests
#define DEBUG_ENABLED 0 // 0 = Disabled; 1 = Enabled
static constexpr int serialMonitorSpeed = 115200;

// (optional) web admin page: HTTPS settings page on TCP 443 (password in ESP32TimeServerSettingsSecrets.h)
// 0 = no web server at all (TCP 443 stays closed, no TLS key is created) and only the settings of this file are
//     used: settings saved on the page are ignored (they stay in NVS and are used again when this is set back to 1)
// 1 = settings page available; settings saved there take precedence over the values in this file
#define WEB_ADMIN_ENABLED 1 // 0 = Disabled; 1 = Enabled

// (optional) RGB LED module support; enable support for one module type below at most
#define RBG_LED_KY_016_MODULE_ENABLED 0 // 0 = Disabled; 1 = Enabled
#define RBG_LED_WS2812_MODULE_ENABLED 0 // 0 = Disabled; 1 = Enabled

static constexpr int RGB_LED_Brightness_Percent = 70;

// The LED colours below may be changed to suit your preferences, but the descriptors should remain unchanged
// For example: you can change LED_normal to blue but leave the descriptor 'LED_normal' unchanged
static constexpr RGB_LED_Colour LED_startup = RGB_LED_Colour::blue;
static constexpr RGB_LED_Colour LED_normal = RGB_LED_Colour::green;
static constexpr RGB_LED_Colour LED_sync = RGB_LED_Colour::white;
static constexpr RGB_LED_Colour LED_warning = RGB_LED_Colour::yellow;
static constexpr RGB_LED_Colour LED_alert = RGB_LED_Colour::orange;
static constexpr RGB_LED_Colour LED_critical = RGB_LED_Colour::red;
static constexpr RGB_LED_Colour LED_button_push = RGB_LED_Colour::pink;

// GPIO pin definitions for use with a KY-016 RGB LED module
static constexpr int LEDBluePin = 4;
static constexpr int LEDGreenPin = 5;
static constexpr int LEDRedPin = 6;

// GPIO pin definitions for a WS2812 RGB LED module
static constexpr int WS2812DataPin = 4;

// (optional) attached LCD support
#define LIQUID_CRYSTAL_DISPLAY_ENABLED 0 // 0 = Disabled; 1 = Enabled
static constexpr int lcdColumns = 20;
static constexpr int lcdRows = 4;
static constexpr char lcdTopLineTitle[] = "ESP32 Time Server"; // text on the LCD's top line (max 17 characters; the last 3 columns are reserved for status indicators)
static constexpr bool display24HourFormat = true;             // set to true for 24-hour format, false for 12-hour format
static constexpr bool displayHoursWithPaddedZero = true;      // set to true to pad the hour with a leading zero when less than 10, false not to pad
static constexpr bool displayTimeZone = true;
static constexpr int lcdI2CAddressPrimary = 0x27;
static constexpr int lcdI2CAddressSecondary = 0x3F;

// (optional) uptime / reset momentary button support
#define UPTIME_RESTART_BUTTON_ENABLED 0 // 0 = Disabled; 1 = Enabled
static constexpr unsigned long holdUpTimeRestartButtonForThisManySecondsToTriggerAReset = 10UL;
static constexpr int upTimeDisplayWillStayActiveForThisManySeconds = 10;
// GPIO pin definitions for the uptime / reset momentary button
static constexpr int upTimeRestartPin = 3;

// (optional) Over The Ethernet updates support
// The firmware image is uploaded over the web admin server (HTTPS, TCP 443, needs WEB_ADMIN_ENABLED 1): on the page
// (System, Firmware update) or with tools/ote_upload.py, see misc/UsefullPowerShellCommands.md.
// Please also see ESP32TimeServerSettingsSecretes.h for the configuration of the OTE password (used by the script).
#define OTE_UPDATES_ENABLED 0 // 0 = Disabled; 1 = Enabled
static constexpr char DeviceName[] = "ESP32TimeServer";

// (optional) NTPv4 symmetric-key packet authentication.
// When enabled, only authenticated NTPv4 requests with a valid configured key receive signed replies.
// However, non-authenticated NTPv4 requests are still processed as normal requests without authentication.
// Please also see ESP32TimeServerSettingsSecretes.h for the configuration of symmetric keys.
#define SYMMETRIC_KEY_AUTHENTICATION_ENABLED 0 // 0 = Disabled; 1 = Enabled

// (optional) MQTT reporting support
// MQTT_ENABLED provides for current system status and activity reporting on information such as:
// uptime, number of active satellites, ntp request counts, etc.
// Additional detailed related to client activity, memory usage, and historical information can be optionally included
// When symmetric-key authentication is enabled, MQTT reporting can distinguish between authenticated and unauthenticated requests as well
// Please also see ESP32TimeServerSettingsSecretes.h for the configuration of the MQTT credentials.
#define MQTT_ENABLED 0                                     // 0 = Disabled; 1 = Enabled
#define MQTT_CLIENT_REPORTING_ENABLED 0                    // 0 = Disabled; 1 = Enabled
#define MQTT_MEMORY_REPORTING_ENABLED 0                    // 0 = Disabled; 1 = Enabled
#define MQTT_HISTORICAL_REPORTING_ENABLED 0                // 0 = Disabled; 1 = Enabled
#define MQTT_DISTINGUISH_AUTHENTICATED_REPORTING_ENABLED 0 // 0 = Disabled; 1 = Enabled

static constexpr char MQTTServerIPAddress[] = ""; // For example 192.168.1.15
static constexpr uint16_t MQTTPort = 1883;
static constexpr char MQTTTopic[] = "ESP32TimeServer";
static constexpr uint16_t MQTTBrokerRetain = 1;      // 0 = tell the broker not to retain the most current message; 1 = tell the broker to retain the most current message
static constexpr uint32_t MQTTReportingPeriod = 900; // in seconds

// QoS 0 delivers at most once and does not retain reports while disconnected
// QoS 1 delivers at least once and queues reports while disconnected
// QoS 2 delivers exactly once and queues reports while disconnected
static constexpr int MQTT_QOS = 0;

// For MQTT_QOS > 0
//   If a TF card, formatted using FAT32, is not present in the TF reader, or if read/write operations to it fail then only
//   limited MQTT report queuing is provided (up to 4 prior reporting periods, each detailing the usage of a maximum of 50 unique clients)
//   with queued reports being managed on a FIFO basis.
//   Additionally, in these cases queued reports are held in volatile RAM and will be lost if power is lost or there is a unexpected system crash;
//   flash ram is not used so as to not degrade it over time.
//
//   However, if TF is present and supports read/write operations then report queuing will be done using the TF card
//   (an empty 16GB TF card can potentially hold more than 36,000 queued messages) with the number of unique clients set
//   by the value MQTT_TF_Client_Limit below.  Regardless, if queued reports do exceed the TF storage capacity they will
//   be managed on a FIFO basis.
//
//   Note: Adjust MQTT_TF_Client_Limit based on the expected number of unique clients and available TF card storage.
//         The report buffer is sized from it at the start (1166 bytes plus 76 bytes per client, RAM), so no other
//         value has to be recalculated. Allowed: 50 to 1000 (also on the web admin page).
static constexpr size_t MQTT_TF_Client_Limit = 500;

// (optional) setting of the ESP32-P4's MAC address support
// set the value below to "" to use the ESP32-P4's default MAC address, or
// set the value below to a desired MAC address, such as "80:f1:b2:d1:d9:18"
// (also accepted: "80-f1-b2-d1-d9-18", "80f1.b2d1.d918" or "80f1b2d1d918", any case)
static constexpr char MACAddress[] = "";

// (optional) setting a static IP address for the ESP32-P4 support
// set StaticIPAddress to "" to use DHCP (the default behaviour), which allows the router to
// auto-assign an IP address, or set it to a desired static IP address such as "192.168.1.100".
// when a static IP address is set, Gateway and SubnetMask must also be provided.
// PrimaryDNS and SecondaryDNS are optional; leave them as "" to omit them.
static constexpr char StaticIPAddress[] = ""; // optional - leave as "" to omit, or for example: 192.168.1.100
static constexpr char Gateway[] = "";         // optional - leave as "" to omit, or for example: : 192.168.1.1
static constexpr char SubnetMask[] = "";      // optional - leave as "" to omit, or for example: 255.255.255.0
static constexpr char PrimaryDNS[] = "";      // optional - leave as "" to omit, or for example: 8.8.8.8
static constexpr char SecondaryDNS[] = "";    // optional - leave as "" to omit, or for example: 1.1.1.1

// Preference for network connection
static constexpr int PreferIPvX = 0; // 0 - dual-stack (uses both IPv4 and IPv6), LCD shows both IPv4 and IPv6 addresses
                                     // 1 - use IPv4 only, LCD shows IPv4 address only
                                     // 2 - use IPv6 only, LCD shows IPv6 address only
                                     // 4 - dual-stack, but LCD shows IPv4 address only
                                     // 6 - dual-stack, but LCD shows IPv6 address only
                                     // 8 - dual-stack, but LCD shows IPv4 and IPv6 address status only ("IPv4: OK/N/A  IPv6: GUA/ULA/LLA/N/A")
                                     // 9 - dual-stack, but leave the LCD bottom line blank for privacy/security reasons
                                     //
                                     // Notes: 
                                     // - With prefer IPv4: if an IPv6 address is offered it will be temporarily accepted,
                                     //   until an IPv4 offer comes in at which time the program will automatically switch over
                                     //   to an IPv4 address.
                                     // - The same is true, in reverse, for prefer IPv6.
                                     // - With a value of 0, 4, 6, 8, or 9 NTP requests from both IPv4 and IPv6 clients are accepted.
                                     // - With values 1 and 2, NTP requests are accepted only on the selected IP version.
                                     // - For IPv6 addresses, the LCD will display the appropriate status based on highest scope address type 
                                     //   (Global-Unicast address (GUA), Unique-Local address (ULA), or Link-Local address (LLA)) assigned:
                                     //   i.e. if a GUA is assigned it will be shown, otherwise if a ULA is assigned it will be shown,
                                     //   otherwise if a LLA is assigned it will be shown.

// (required) Time zone setting for your region - for more information see https://gist.github.com/alwynallan/24d96091655391107939
static constexpr const char *timeZoneSpec = "CET-1CEST,M3.5.0,M10.5.0/3";

// Unless you know what you are doing, the options below should be left as is

// PPS holdover (default of the web admin setting "PPS holdover", whole seconds, 0 to 600; 0 = no holdover).
// The PPS counts as lost after 3.5 s without a pulse. For this long after that the server keeps answering at stratum 1,
// coasting on the learned oscillator rate, while the advertised root dispersion grows with the time since the last
// pulse. After it the server answers at stratum 16 (leap indicator "not synchronised") until the PPS and GNSS are back.
static constexpr int64_t ntpHoldoverDurationUs = 2LL * 1000000LL; // Holdover duration in microseconds (2 seconds)
// Conservative uncertainty growth while coasting on the learned oscillator rate (default of the web admin setting
// "Root dispersion growth", 1 to 10000 ppm; 100 ppm = 0.1 ms per second since the last pulse).
static constexpr uint32_t ntpHoldoverDispersionPpm = 100;
// When the PPS is lost for longer than the holdover (defaults of the web admin settings):
// - ntpServeAfterHoldover: true = keep answering NTP requests (for ntpServeAfterPpsLostMinutes); false = no answer at
//   all, so the clients switch to other servers. Answers again when the PPS and GNSS are back.
// - ntpServeAfterPpsLostMinutes: with ntpServeAfterHoldover, the answers stop this long after the PPS was lost
//   (counted from the loss, the holdover included; 0 = no limit, at most 1440)
// - ntpStratumAfterHoldover: stratum of those answers. 16 = "not synchronised" (leap indicator alarm): the clients
//   stop using the server. 2 to 15: the answers stay "synchronised" at that stratum, coasting on the learned rate with
//   the root dispersion still growing, for at most one hour after the last pulse (then 16).
// Relying on time that is no longer synchronised to the GNSS is probably not a good idea: its error grows by an unknown
// amount. Use a stratum below 16 and a long serving time only when the clients absolutely need a time source.
static constexpr bool ntpServeAfterHoldover = true;
static constexpr uint32_t ntpServeAfterPpsLostMinutes = 60;
static constexpr uint8_t ntpStratumAfterHoldover = 16;

// This project was designed and tested to work with a SparkFun GNSS Receiver Breakout board which uses a u-blox - MAX-M10S module.
// ( https://www.sparkfun.com/sparkfun-gnss-receiver-breakout-max-m10s-qwiic.html )
// However, the code has fallback logic for non/cloned/older u-blox gnss modules and has been tested with one such device as well.
//
// The setting below determines if the code should provide processing for other (than the MAX-M10S) gnss module - even if they are less capable/potentially less accurate.

// Note: even if fallback processing is set to true below, accuracy should still be fine as long as PPS is disciplined.
// For more information here are some detailed timing accuracy notes:
// - This firmware timestamps NTP responses using gettimeofday() system call, which is backed by the ESP32-P4's high-resolution timer
//   driven by the built-in 40 MHz external XTAL via the APB clock (~10 ppm). This is the most accurate clock available
//   on this chip; no external 32.768 kHz RTC crystal is needed or beneficial for NTP timestamping purposes.
// - The PPS discipline task applies continuous sub-second corrections on every GNSS PPS pulse.
//   This reduces inter-sync error to well under 1 ms, limited mainly by interrupt latency (~10-100 us).
// - Preferred GNSS (MAX-M10S) connects at 921600 baud, minimizing serial latency and enabling faster, more precise time
//   message processing. Fallback modules may be limited to 9600 baud, introducing additional parsing delay and reducing
//   the accuracy of the time set at each GNSS resync.

static constexpr bool allowFallbackProcessing = true;

static constexpr uint32_t periodicGNSSRefreshEveryThisNumberOfMinutes = 5UL; // resync with GNSS every 5 minutes (recommended)

// a reboot during setup may be required to facilitate initial setup of non/clone/older u-blox gnss modules;
// recommend leaving it set to true unless code continually restarts when this setting is used
static constexpr bool rebootIfGNSSBaudChangeCommandSucceedsButImmediateReconnectFails = true;

static constexpr time_t safeguardThresholdInSeconds = 1; // When a new GNSS reading is taken the difference between it and the last reading
                                                         // should be sub-second, if not a sanity check safeguard flag is tripped

static constexpr bool rebootIfSanityCheckFails = true; // Further to the above,
                                                       // if the sanity check fails, the system will either:
                                                       // - automatically reboot if this setting is set to true, or
                                                       // - display double asterisks'**' on the LCD screen to indicate an issue with the GNSS
                                                       //   and then will providing the esp32's time, unsynced by the GNSS, moving forward.

// (required) GNSS support; do not change these unless you know what you are doing

// IMPORTANT NOTES: ***************************************************************************************
// Some GNSS receivers and breakout boards provide a 3.3v PPS (Pulse-Per-Second) output pin               *
// while others provide a 5v PPS output pin.                                                              *
// 1. Only connect a 3.3v PPS pin to an ESP32 GPIO pin, never connect a 5v PPS pin to an ESP32 GPIO pin   *
// 2. For the wire between the ESP32-P4's GPIO pin and the GNSS Receiver's PPS pin                        *
//    use as short and as well connect wire as possible; a standard copper Dupont wire                    *
//    will be fine assuming its gauge is between 24 AWG and 28AWG                                         *
// 3. The GNSS GND must be connected to the ESP32 GND                                                     *
// ********************************************************************************************************

#define UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED 0 // 0 = Disabled; 1 = Enabled
// The following only needs to be set if your receiver does not use a U-Blox or U-Block clone GNSS chip
static constexpr uint32_t baudRateForUbloxNonCompliantGNSSReceiver = 921600;

// (required) GPIO pin definitions for the GNSS module
static constexpr int TXPin = 22;  // note: prior to release 2.4 pin 16 was used for TX
static constexpr int RXPin = 21;  // note: prior to release 2.4 pin 17 was used for RX
static constexpr int PPSPin = 20; // note: prior to release 2.4 pin 18 was used for PPS

// (optional) GPIO pin for the PTP PPS loopback, -1 = not used
// The Ethernet MAC outputs one pulse per second of its PTP clock on this pin, and the pin is read back by the same
// MCPWM capture timer that captures the GNSS PPS. This compares the PTP clock with the GNSS PPS in hardware
// (12.5 ns resolution) instead of by software readings, which removes the read delay error of a few hundred ns.
// Nothing has to be wired: the pulse is looped back inside the pad of the chip. The pin is driven as an output, so
// it must be free and connected to nothing (no header pin that is in use, no on-board function).
// Requires PTP_FREE_RUNNING 1 in main.cpp and an ESP32-P4 of revision v3.0 or newer.
static constexpr int PTPPPSLoopbackPin = 23;

// (required) GPIO pin definitions for the Waveshare:
// ESP32-P4-ETH, ESP32-P4-POE-ETH, ESP32-P4-WIFI-ETH, ESP32-P4-WIFI-POE-ETH
// if your using this board do not change these
static constexpr int TFCardCommandPin = 44;
static constexpr int TFCardClockPin = 43;
static constexpr int TFCardData3Pin = 42;
static constexpr int TFCardData2Pin = 41;
static constexpr int TFCardData1Pin = 40;
static constexpr int TFCardData0Pin = 39;
