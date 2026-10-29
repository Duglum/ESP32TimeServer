// ESP32 Time Server - host tests
// The UART reader of main/custom/gnss_uart.cpp with access to its byte parser (it is in an unnamed namespace there):
// the tests feed the bytes that the reader task would read from the driver.

#include "gnss_uart.cpp"

#include "host_hooks.h"

void host_gnss_uart_reset(bool running)
{
    reset_parsers();
    s_configured = running;
    s_running.store(running, std::memory_order_release);
    gnss_uart_reset_traffic();
}

void host_gnss_uart_console_capture(bool capture)
{
    // As the reader task does at the start of a pass (the console has to be open for entries to be kept)
    s_console_capture = capture;
    s_console_line_length = 0;
}

void host_gnss_uart_feed(const uint8_t *data, size_t length, uint32_t generation)
{
    for (size_t i = 0; i < length; ++i)
        process_byte(data[i], generation);
}
