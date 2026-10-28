// ESP32 Time Server - host tests
// Minimal stand-ins for the ESP-IDF and FreeRTOS functions that the tested sources use. Single-threaded: critical
// sections do nothing, a semaphore is a counter, and waiting moves the simulated esp_timer clock on.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // ---- esp_err ----
    typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
    const char *esp_err_to_name(esp_err_t err);

    // ---- esp_timer: simulated clock (us), set and moved on by the tests ----
    extern int64_t host_now_us;
    int64_t esp_timer_get_time(void);

    // ---- esp_log: printed only with host_log_verbose ----
    extern bool host_log_verbose;
    void host_log(const char *level, const char *tag, const char *format, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, format, ...) host_log("E", tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) host_log("W", tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) host_log("I", tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) host_log("D", tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) host_log("V", tag, format, ##__VA_ARGS__)

    // ---- FreeRTOS types and critical sections ----
    typedef uint32_t TickType_t;
    typedef int BaseType_t;
    typedef unsigned int UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
    typedef struct
    {
        int unused;
    } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))

    // ---- Semaphores (binary semaphores and mutexes) ----
    typedef struct host_semaphore *SemaphoreHandle_t;
    SemaphoreHandle_t xSemaphoreCreateBinary(void);
    SemaphoreHandle_t xSemaphoreCreateMutex(void);
    void vSemaphoreDelete(SemaphoreHandle_t semaphore);
    BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
    BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);

    // ---- Tasks and queues (no tasks are started in the tests) ----
    typedef struct host_task *TaskHandle_t;
    typedef void (*TaskFunction_t)(void *);
    BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack, void *parameter,
                                       UBaseType_t priority, TaskHandle_t *handle, BaseType_t core);
    void vTaskDelete(TaskHandle_t task);
    void vTaskDelay(TickType_t ticks);
    TickType_t xTaskGetTickCount(void);
    typedef struct host_queue *QueueHandle_t;
    BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
    BaseType_t xQueueReset(QueueHandle_t queue);

    // ---- UART driver: what is written goes to host_uart_tx_hook (a test can answer through the reader) ----
    typedef int uart_port_t;
    typedef enum
    {
        UART_DATA,
        UART_BREAK,
        UART_BUFFER_FULL,
        UART_FIFO_OVF,
        UART_FRAME_ERR,
        UART_PARITY_ERR,
    } uart_event_type_t;
    typedef struct
    {
        uart_event_type_t type;
        size_t size;
    } uart_event_t;
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE (-1)
    typedef struct
    {
        int baud_rate;
        int data_bits;
        int parity;
        int stop_bits;
        int flow_ctrl;
        int source_clk;
    } uart_config_t;
    esp_err_t uart_driver_install(uart_port_t port, int rx_buffer_size, int tx_buffer_size, int queue_size,
                                  QueueHandle_t *queue, int flags);
    esp_err_t uart_driver_delete(uart_port_t port);
    esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config);
    esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
    esp_err_t uart_set_baudrate(uart_port_t port, uint32_t baud);
    esp_err_t uart_flush_input(uart_port_t port);
    esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks);
    esp_err_t uart_get_buffered_data_len(uart_port_t port, size_t *size);
    int uart_read_bytes(uart_port_t port, void *buffer, uint32_t length, TickType_t ticks);
    int uart_write_bytes(uart_port_t port, const void *data, size_t length);
    extern void (*host_uart_tx_hook)(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
