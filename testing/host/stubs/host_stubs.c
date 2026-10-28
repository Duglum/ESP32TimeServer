// ESP32 Time Server - host tests
// Implementation of the stand-ins of host_stubs.h

#include "host_stubs.h"

#include <stdarg.h>
#include <stdlib.h>

int64_t host_now_us = 1000000;
bool host_log_verbose = false;
void (*host_uart_tx_hook)(const uint8_t *data, size_t length) = NULL;

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

int64_t esp_timer_get_time(void)
{
    return host_now_us;
}

void host_log(const char *level, const char *tag, const char *format, ...)
{
    if (!host_log_verbose)
        return;
    va_list args;
    va_start(args, format);
    printf("%s (%s) ", level, tag);
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

struct host_semaphore
{
    int count;
};

static SemaphoreHandle_t create_semaphore(int count)
{
    SemaphoreHandle_t semaphore = (SemaphoreHandle_t)malloc(sizeof(struct host_semaphore));
    if (semaphore != NULL)
        semaphore->count = count;
    return semaphore;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return create_semaphore(0);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return create_semaphore(1);
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    free(semaphore);
}

// Nothing else runs: an empty semaphore stays empty, the wait only passes the time
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    if (semaphore->count > 0)
    {
        semaphore->count--;
        return pdTRUE;
    }
    if (ticks != portMAX_DELAY)
        host_now_us += (int64_t)ticks * 1000;
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore->count > 0)
        return pdFALSE;
    semaphore->count = 1;
    return pdTRUE;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack, void *parameter,
                                   UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    (void)function, (void)name, (void)stack, (void)parameter, (void)priority, (void)core;
    if (handle != NULL)
        *handle = NULL;
    return pdFAIL;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

void vTaskDelay(TickType_t ticks)
{
    host_now_us += (int64_t)ticks * 1000;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(host_now_us / 1000);
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    (void)queue, (void)item;
    host_now_us += (int64_t)ticks * 1000;
    return pdFALSE;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    (void)queue;
    return pdPASS;
}

esp_err_t uart_driver_install(uart_port_t port, int rx_buffer_size, int tx_buffer_size, int queue_size,
                              QueueHandle_t *queue, int flags)
{
    (void)port, (void)rx_buffer_size, (void)tx_buffer_size, (void)queue_size, (void)flags;
    if (queue != NULL)
        *queue = NULL;
    return ESP_FAIL;
}

esp_err_t uart_driver_delete(uart_port_t port)
{
    (void)port;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
    (void)port, (void)config;
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    (void)port, (void)tx, (void)rx, (void)rts, (void)cts;
    return ESP_OK;
}

esp_err_t uart_set_baudrate(uart_port_t port, uint32_t baud)
{
    (void)port, (void)baud;
    return ESP_OK;
}

esp_err_t uart_flush_input(uart_port_t port)
{
    (void)port;
    return ESP_OK;
}

esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks)
{
    (void)port, (void)ticks;
    return ESP_OK;
}

esp_err_t uart_get_buffered_data_len(uart_port_t port, size_t *size)
{
    (void)port;
    *size = 0;
    return ESP_OK;
}

int uart_read_bytes(uart_port_t port, void *buffer, uint32_t length, TickType_t ticks)
{
    (void)port, (void)buffer, (void)length, (void)ticks;
    return 0;
}

int uart_write_bytes(uart_port_t port, const void *data, size_t length)
{
    (void)port;
    if (host_uart_tx_hook != NULL)
        host_uart_tx_hook((const uint8_t *)data, length);
    return (int)length;
}
