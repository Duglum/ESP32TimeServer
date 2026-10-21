// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Over The Ethernet update over the web admin server (POST /api/ote, HTTPS): the request body is a raw application
// image (the ESP32TimeServer.bin of the build folder, not a merged flash image). It is written into the inactive
// OTA partition and selected for the next boot. Before the first flash write the image header is checked: image
// magic, the project name of this firmware, the chip and the chip revision range of the build (an image of the
// pre-v3 build is refused on a v3 chip and the other way round). esp_ota_end() verifies the whole image.

#pragma once

#include "esp_http_server.h"

struct web_ote_result_t
{
    bool accepted;       // the image is selected for the next boot; the device restarts a few seconds later
    bool body_consumed;  // false: the rest of the body was not read (the connection has to be closed)
    const char *status;  // HTTP status line of the reply
    const char *message; // text of the reply
};

// Receives the body of req (authentication and Content-Type are checked by the caller). Runs in the web server task.
// Progress, failure and success are reported to main.cpp (app_ote_progress(), app_ote_failed(), app_ote_accepted()).
web_ote_result_t web_ote_receive(httpd_req_t *req);
