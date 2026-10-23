// ESP32 Time Server v3.0.5-WebIf
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

// Password of the web admin page (https://<address>/), used until a new one is set on the page (it is then
// stored in NVS as a salted PBKDF2 hash). While the factory value "password" is in use the page shows a warning.
// The factory reset (page or button) returns to this password.
// Firmware uploads with tools/ote_upload.py authenticate with this password as well (HTTP Basic authentication
// inside the HTTPS connection); they are refused while it is the factory value. To update many devices with one
// script run, give them the same password here.
static constexpr char WebAdminPassword[] = "password";

// Credentials for the ESP32 Time Server as an MQTT client
static constexpr char MQTTUsername[] = "";
static constexpr char MQTTPassword[] = "";

// Symmetric keys for NTPv4 packet authentication
// Please see misc/symmetric_key_authentication.md for instructions on generating and managing symmetric keys.
static constexpr symmetric_key_t symmetric_keys[] = {
    {1, "abcdefghijklmnopqrstuvwxyz123456"},
};