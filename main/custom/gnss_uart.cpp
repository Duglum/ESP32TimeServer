// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "gnss_uart.h"
#include "gnss_nmea.h"
#include "gnss_ubx.h"
#include "ESP32TimeServerSettings.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace
{
    static const char *TAG = "gnss_uart";

    static constexpr int RX_BUFFER_SIZE = 2048;  // driver ring buffer (about 22 ms at 921600 baud)
    static constexpr int EVENT_QUEUE_LENGTH = 16;
    static constexpr size_t SENTENCE_SIZE = 128; // NMEA allows 82 characters
    static constexpr size_t READ_CHUNK_SIZE = 256;
    static constexpr uint16_t UBX_MAX_PAYLOAD = 512; // MON-VER of an M10 is about 220 bytes; longer frames are skipped

    gnss_uart_config_t s_config{};
    bool s_configured = false;
    QueueHandle_t s_events = nullptr;
    TaskHandle_t s_task = nullptr;
    std::atomic<bool> s_stop_requested{false};
    std::atomic<bool> s_running{false};
    std::atomic<uint32_t> s_generation{1};

    std::atomic<uint32_t> s_overflows{0};
    portMUX_TYPE s_traffic_lock = portMUX_INITIALIZER_UNLOCKED;
    gnss_uart_traffic_t s_traffic{}; // counters of valid sentences and frames, sample (s_traffic_lock)
    std::atomic<uint32_t> s_traffic_bytes{0};
    std::atomic<bool> s_sample_full{false};

    // Parser state, only used by the reader task
    enum class ubx_state_t : uint8_t
    {
        idle,
        sync2,
        cls,
        id,
        length_low,
        length_high,
        payload,
        checksum_a,
        checksum_b
    };

    char s_line[SENTENCE_SIZE];
    size_t s_line_length = 0;
    bool s_collecting = false;
    uint32_t s_line_generation = 0;

    ubx_state_t s_ubx_state = ubx_state_t::idle;
    uint8_t s_ubx_cls = 0;
    uint8_t s_ubx_id = 0;
    uint16_t s_ubx_length = 0;
    uint16_t s_ubx_received = 0;
    uint8_t s_ubx_ck_a = 0;
    uint8_t s_ubx_ck_b = 0;
    uint8_t s_ubx_rx_ck_a = 0;
    uint32_t s_ubx_generation = 0;
    uint8_t s_ubx_payload[UBX_MAX_PAYLOAD];
    uint8_t s_chunk[READ_CHUNK_SIZE];

    void reset_parsers()
    {
        s_collecting = false;
        s_ubx_state = ubx_state_t::idle;
    }

    void note_sample(uint8_t byte)
    {
        s_traffic_bytes.fetch_add(1, std::memory_order_relaxed);
        if (s_sample_full.load(std::memory_order_relaxed))
            return; // the lock only for the first bytes after a reset
        taskENTER_CRITICAL(&s_traffic_lock);
        const size_t length = strlen(s_traffic.sample);
        if (length < sizeof(s_traffic.sample) - 1)
        {
            const char ch = static_cast<char>(byte);
            s_traffic.sample[length] = (ch >= 32 && ch <= 126) ? ch : ((ch == '\r' || ch == '\n' || ch == '\t') ? ' ' : '.');
            s_traffic.sample[length + 1] = '\0';
        }
        else
            s_sample_full.store(true, std::memory_order_relaxed);
        taskEXIT_CRITICAL(&s_traffic_lock);
    }

    void ubx_add_checksum(uint8_t byte)
    {
        s_ubx_ck_a = static_cast<uint8_t>(s_ubx_ck_a + byte);
        s_ubx_ck_b = static_cast<uint8_t>(s_ubx_ck_b + s_ubx_ck_a);
    }

    // generation: flush generation read before the bytes were read from the driver. A frame counts only if all its
    // bytes have the generation of its start: a flush in between discards it.
    void process_ubx_byte(uint8_t byte, uint32_t generation)
    {
        switch (s_ubx_state)
        {
        case ubx_state_t::idle:
            break;
        case ubx_state_t::sync2:
            s_ubx_state = byte == 0x62 ? ubx_state_t::cls : ubx_state_t::idle;
            s_ubx_ck_a = 0;
            s_ubx_ck_b = 0;
            break;
        case ubx_state_t::cls:
            s_ubx_cls = byte;
            ubx_add_checksum(byte);
            s_ubx_state = ubx_state_t::id;
            break;
        case ubx_state_t::id:
            s_ubx_id = byte;
            ubx_add_checksum(byte);
            s_ubx_state = ubx_state_t::length_low;
            break;
        case ubx_state_t::length_low:
            s_ubx_length = byte;
            ubx_add_checksum(byte);
            s_ubx_state = ubx_state_t::length_high;
            break;
        case ubx_state_t::length_high:
            s_ubx_length = static_cast<uint16_t>(s_ubx_length | (static_cast<uint16_t>(byte) << 8));
            ubx_add_checksum(byte);
            s_ubx_received = 0;
            if (s_ubx_length > UBX_MAX_PAYLOAD)
                s_ubx_state = ubx_state_t::idle; // not a frame this program needs (or noise): search the next sync
            else
                s_ubx_state = s_ubx_length == 0 ? ubx_state_t::checksum_a : ubx_state_t::payload;
            break;
        case ubx_state_t::payload:
            s_ubx_payload[s_ubx_received++] = byte;
            ubx_add_checksum(byte);
            if (s_ubx_received >= s_ubx_length)
                s_ubx_state = ubx_state_t::checksum_a;
            break;
        case ubx_state_t::checksum_a:
            s_ubx_rx_ck_a = byte;
            s_ubx_state = ubx_state_t::checksum_b;
            break;
        case ubx_state_t::checksum_b:
            s_ubx_state = ubx_state_t::idle;
            if (s_ubx_rx_ck_a == s_ubx_ck_a && byte == s_ubx_ck_b && generation == s_ubx_generation)
            {
                taskENTER_CRITICAL(&s_traffic_lock);
                s_traffic.ubx_frames++;
                taskEXIT_CRITICAL(&s_traffic_lock);
                gnss_ubx_process_frame(s_ubx_cls, s_ubx_id, s_ubx_payload, s_ubx_length, s_ubx_generation);
            }
            break;
        }
    }

    void process_byte(uint8_t byte, uint32_t generation)
    {
        note_sample(byte);

        // A flush since the start of the frame or sentence: it is dropped, this byte may start the next one (a cut
        // frame would otherwise take the following bytes as its payload)
        if (s_ubx_state != ubx_state_t::idle && generation != s_ubx_generation)
            s_ubx_state = ubx_state_t::idle;
        if (s_collecting && generation != s_line_generation)
            s_collecting = false;

        if (s_ubx_state != ubx_state_t::idle)
        {
            process_ubx_byte(byte, generation);
            return;
        }

        const char ch = static_cast<char>(byte);
        if (byte == 0xB5)
        {
            // UBX sync 1 (not a printable character: a sentence being collected is dropped)
            s_collecting = false;
            s_ubx_state = ubx_state_t::sync2;
            s_ubx_generation = generation;
            return;
        }
        if (ch == '$')
        {
            s_collecting = true;
            s_line_length = 0;
            s_line[s_line_length++] = ch;
            s_line_generation = generation;
            return;
        }
        if (!s_collecting)
            return;

        if (ch == '\r' || ch == '\n')
        {
            s_line[s_line_length] = '\0';
            s_collecting = false;
            if (generation == s_line_generation && gnss_nmea_process_sentence(s_line, s_line_generation))
            {
                taskENTER_CRITICAL(&s_traffic_lock);
                s_traffic.nmea_sentences++;
                taskEXIT_CRITICAL(&s_traffic_lock);
            }
            return;
        }

        // Other bytes than printable characters (noise, binary data): the sentence is dropped
        if (byte < 32 || byte > 126 || s_line_length >= SENTENCE_SIZE - 1)
        {
            s_collecting = false;
            return;
        }
        s_line[s_line_length++] = ch;
    }

    void reader_task(void *)
    {
        const uart_port_t port = static_cast<uart_port_t>(s_config.uart_num);
        int64_t last_log_us = esp_timer_get_time();
        (void)last_log_us; // only read by the debug log

        while (!s_stop_requested.load(std::memory_order_acquire))
        {
            uart_event_t event{};
            if (xQueueReceive(s_events, &event, pdMS_TO_TICKS(100)) == pdTRUE &&
                (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL))
            {
                // Data was lost: start again with an empty buffer
                s_overflows.fetch_add(1, std::memory_order_relaxed);
                uart_flush_input(port);
                xQueueReset(s_events);
                reset_parsers();
                continue;
            }

            for (;;)
            {
                size_t buffered = 0;
                if (uart_get_buffered_data_len(port, &buffered) != ESP_OK || buffered == 0)
                    break;
                // The generation before the read: bytes read now were received after the flushes up to it
                const uint32_t generation = s_generation.load(std::memory_order_acquire);
                const int length = uart_read_bytes(port, s_chunk, std::min(buffered, sizeof(s_chunk)), 0);
                if (length <= 0)
                    break;
                for (int i = 0; i < length; ++i)
                    process_byte(s_chunk[i], generation);
            }

#if DEBUG_ENABLED
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_log_us >= 60000000LL)
            {
                last_log_us = now_us;
                gnss_nmea_log_status();
                gnss_ubx_log_status();
            }
#endif
            if (s_config.loop_hook != nullptr)
                s_config.loop_hook();
        }

        uart_driver_delete(port);
        s_events = nullptr;
        s_running.store(false, std::memory_order_release);
        s_task = nullptr;
        vTaskDelete(nullptr);
    }
}

bool gnss_uart_start(const gnss_uart_config_t &config)
{
    gnss_uart_stop();

    s_config = config;
    s_configured = true;
    const uart_port_t port = static_cast<uart_port_t>(config.uart_num);
    uart_config_t uart_config{};
    uart_config.baud_rate = static_cast<int>(config.baud);
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    QueueHandle_t events = nullptr;
    esp_err_t err = uart_driver_install(port, RX_BUFFER_SIZE, 0, EVENT_QUEUE_LENGTH, &events, 0);
    if (err == ESP_OK)
        err = uart_param_config(port, &uart_config);
    if (err == ESP_OK)
        err = uart_set_pin(port, config.tx_pin, config.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART%d at %lu baud (RX GPIO %d) could not be set up: %s", config.uart_num,
                 static_cast<unsigned long>(config.baud), config.rx_pin, esp_err_to_name(err));
        uart_driver_delete(port);
        return false;
    }

    s_events = events;
    reset_parsers();
    gnss_uart_reset_traffic();
    s_stop_requested.store(false, std::memory_order_relaxed);
    s_running.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(reader_task, "gnss_uart", config.task_stack, nullptr, config.task_priority, &s_task,
                                config.task_core) != pdPASS)
    {
        ESP_LOGE(TAG, "Reader task could not be started");
        s_running.store(false, std::memory_order_release);
        s_task = nullptr;
        uart_driver_delete(port);
        s_events = nullptr;
        return false;
    }
    return true;
}

void gnss_uart_stop(void)
{
    if (!s_running.load(std::memory_order_acquire))
        return;
    s_stop_requested.store(true, std::memory_order_release);
    // The reader leaves its loop within 100 ms, deletes the driver and itself
    for (int i = 0; i < 50 && s_running.load(std::memory_order_acquire); ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
}

bool gnss_uart_running(void)
{
    return s_running.load(std::memory_order_acquire);
}

bool gnss_uart_set_baud(uint32_t baud)
{
    if (!s_configured)
        return false;
    if (!s_running.load(std::memory_order_acquire))
    {
        gnss_uart_config_t config = s_config;
        config.baud = baud;
        return gnss_uart_start(config);
    }
    const uart_port_t port = static_cast<uart_port_t>(s_config.uart_num);
    (void)uart_wait_tx_done(port, pdMS_TO_TICKS(100)); // a command still being sent goes out at the old rate
    if (uart_set_baudrate(port, baud) != ESP_OK)
        return false;
    s_config.baud = baud;
    (void)gnss_uart_flush();
    return true;
}

uint32_t gnss_uart_flush(void)
{
    // First the driver (ring buffer and FIFO), then the generation: bytes the reader reads afterwards carry the new
    // generation only if they were received after the flush
    if (s_running.load(std::memory_order_acquire))
        uart_flush_input(static_cast<uart_port_t>(s_config.uart_num));
    return s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool gnss_uart_write(const uint8_t *data, size_t length)
{
    if (data == nullptr || !s_running.load(std::memory_order_acquire))
        return false;
    return uart_write_bytes(static_cast<uart_port_t>(s_config.uart_num), data, length) == static_cast<int>(length);
}

void gnss_uart_reset_traffic(void)
{
    taskENTER_CRITICAL(&s_traffic_lock);
    s_traffic = gnss_uart_traffic_t{};
    s_traffic_bytes.store(0, std::memory_order_relaxed);
    s_sample_full.store(false, std::memory_order_relaxed);
    taskEXIT_CRITICAL(&s_traffic_lock);
}

void gnss_uart_get_traffic(gnss_uart_traffic_t *traffic)
{
    if (traffic == nullptr)
        return;
    taskENTER_CRITICAL(&s_traffic_lock);
    *traffic = s_traffic;
    traffic->bytes = s_traffic_bytes.load(std::memory_order_relaxed);
    taskEXIT_CRITICAL(&s_traffic_lock);
}

uint32_t gnss_uart_overflows(void)
{
    return s_overflows.load(std::memory_order_relaxed);
}
