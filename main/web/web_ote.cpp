// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Over The Ethernet update: firmware upload over the web admin server (see web_ote.h). The image checks follow the
// native OTE updater of release 3.0.5 (main/custom/native_ote.cpp of roblatour/ESP32TimeServer); the transport is
// the TLS connection of the web admin server instead of a separate plain HTTP server.

#include "web_ote.h"
#include "web_server.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C"
{
#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
}

namespace
{
    const char *TAG = "web_ote";

    // Image header, first segment header and application description: everything the checks need
    constexpr size_t IMAGE_PREFIX_SIZE = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    constexpr size_t BUFFER_SIZE = 4096;
    static_assert(IMAGE_PREFIX_SIZE <= BUFFER_SIZE, "the image prefix must fit the receive buffer");
    constexpr int MAX_IDLE_TIMEOUTS = 3;                       // 3 x the 5 s receive timeout of the server: 15 s without data
    constexpr int64_t MAX_UPLOAD_TIME_US = 300LL * 1000000LL;  // whole upload

    bool s_accepted = false; // an image was selected for the next boot (web server task only, never cleared)

    // Receives exactly length bytes. ESP_ERR_TIMEOUT: no data for 15 s or the upload deadline passed.
    esp_err_t receive(httpd_req_t *req, char *buffer, size_t length, int64_t deadline_us)
    {
        size_t received = 0;
        int idle_timeouts = 0;
        while (received < length)
        {
            if (esp_timer_get_time() >= deadline_us)
                return ESP_ERR_TIMEOUT;
            const int ret = httpd_req_recv(req, buffer + received, length - received);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                if (++idle_timeouts >= MAX_IDLE_TIMEOUTS)
                    return ESP_ERR_TIMEOUT;
                continue;
            }
            if (ret <= 0)
                return ESP_FAIL;
            idle_timeouts = 0;
            received += static_cast<size_t>(ret);
        }
        return ESP_OK;
    }

    // Rejected before any data was read: nothing is shown on the LCD
    web_ote_result_t refuse(const char *status, const char *message)
    {
        ESP_LOGW(TAG, "Firmware upload refused: %s", message);
        return {false, false, status, message};
    }

    // Failed after the upload started: shown on the LCD (lcd_reason: at most 20 characters)
    web_ote_result_t fail(const char *status, const char *message, const char *lcd_reason, esp_err_t err)
    {
        ESP_LOGE(TAG, "Firmware upload failed: %s (%s)", message, esp_err_to_name(err));
        app_ote_failed(lcd_reason);
        return {false, false, status, message};
    }

    web_ote_result_t receive_failure(esp_err_t err)
    {
        return err == ESP_ERR_TIMEOUT ? fail("408 Request Timeout", "Upload timeout", "Upload timeout", err)
                                      : fail("400 Bad Request", "Upload interrupted or truncated", "Upload interrupted", err);
    }

    // Checks the image header, the first segment header and the application description. false: *failure is the reply.
    bool check_prefix(const char *buffer, size_t content_length, web_ote_result_t *failure)
    {
        esp_image_header_t image_header{};
        esp_image_segment_header_t segment_header{};
        esp_app_desc_t description{};
        memcpy(&image_header, buffer, sizeof(image_header));
        memcpy(&segment_header, buffer + sizeof(image_header), sizeof(segment_header));
        memcpy(&description, buffer + sizeof(image_header) + sizeof(segment_header), sizeof(description));
        const esp_app_desc_t *running = esp_app_get_description();
        if (image_header.magic != ESP_IMAGE_HEADER_MAGIC || image_header.segment_count == 0 ||
            image_header.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
            segment_header.data_len < sizeof(esp_app_desc_t) ||
            segment_header.data_len > content_length - sizeof(image_header) - sizeof(segment_header) ||
            description.magic_word != ESP_APP_DESC_MAGIC_WORD)
        {
            *failure = fail("400 Bad Request", "Not an application image (use the .bin of the build folder, not a merged flash image)",
                            "Not an app image", ESP_ERR_INVALID_ARG);
            return false;
        }
        if (strncmp(description.project_name, running->project_name, sizeof(description.project_name)) != 0)
        {
            *failure = fail("400 Bad Request", "The image belongs to another project", "Wrong project", ESP_ERR_INVALID_ARG);
            return false;
        }
        const esp_err_t err = bootloader_common_check_chip_validity(&image_header, ESP_IMAGE_APPLICATION);
        if (err != ESP_OK)
        {
            *failure = fail("400 Bad Request", "The image was built for another chip or chip revision (see the build profiles in README.md)",
                            "Wrong chip revision", err);
            return false;
        }
        ESP_LOGI(TAG, "Firmware upload: %.32s %.32s (running: %.32s)", description.project_name, description.version, running->version);
        return true;
    }

    web_ote_result_t write_image(httpd_req_t *req, const esp_partition_t *partition, char *buffer)
    {
        const size_t total = req->content_len;
        const int64_t deadline_us = esp_timer_get_time() + MAX_UPLOAD_TIME_US;
        app_ote_progress(0, total);

        esp_err_t err = receive(req, buffer, IMAGE_PREFIX_SIZE, deadline_us);
        if (err != ESP_OK)
            return receive_failure(err);
        web_ote_result_t failure{};
        if (!check_prefix(buffer, total, &failure))
            return failure;

        esp_ota_handle_t handle = 0;
        err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
        if (err != ESP_OK)
            return fail("500 Internal Server Error", "The OTA partition could not be prepared", "OTA begin failed", err);

        size_t written = 0;
        size_t count = IMAGE_PREFIX_SIZE;
        for (;;)
        {
            err = esp_ota_write(handle, buffer, count);
            if (err != ESP_OK)
            {
                esp_ota_abort(handle);
                return fail("500 Internal Server Error", "Writing the OTA partition failed", "Flash write failed", err);
            }
            written += count;
            app_ote_progress(written, total);
            if (written == total)
                break;
            count = std::min(BUFFER_SIZE, total - written);
            err = receive(req, buffer, count, deadline_us);
            if (err != ESP_OK)
            {
                esp_ota_abort(handle);
                return receive_failure(err);
            }
        }

        // esp_ota_end() verifies the image and releases the handle, also on failure
        err = esp_ota_end(handle);
        if (err != ESP_OK)
            return fail("400 Bad Request", "The image did not pass the verification", "Image invalid", err);
        err = esp_ota_set_boot_partition(partition);
        if (err != ESP_OK)
            return fail("500 Internal Server Error", "The new image could not be selected for the next boot", "Boot select failed", err);

        s_accepted = true;
        ESP_LOGI(TAG, "Firmware upload: %u bytes written to %s and selected for the next boot",
                 static_cast<unsigned int>(total), partition->label);
        app_ote_accepted();
        return {true, true, "200 OK", "Firmware accepted. The device restarts in 5 seconds."};
    }
} // namespace

web_ote_result_t web_ote_receive(httpd_req_t *req)
{
    if (s_accepted)
        return refuse("409 Conflict", "A firmware image was already accepted; wait for the restart");
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") != 0 || req->content_len < IMAGE_PREFIX_SIZE)
        return refuse("411 Length Required", "Send the application image with its Content-Length");

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (running == nullptr || partition == nullptr || partition->address == running->address)
        return refuse("500 Internal Server Error", "No inactive OTA partition");
    if (req->content_len > partition->size)
        return refuse("413 Content Too Large", "The image is larger than the OTA partition");

    char *buffer = static_cast<char *>(malloc(BUFFER_SIZE));
    if (buffer == nullptr)
        return refuse("503 Service Unavailable", "Out of memory");
    const web_ote_result_t result = write_image(req, partition, buffer);
    free(buffer);
    return result;
}
