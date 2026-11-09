# All settings are made on the web admin page (there is no settings file). The optional features (LCD, KY-016 / WS2812
# LED, MQTT, NTP authentication, button) are always compiled in; the web admin page switches them.
set(MAIN_SOURCES
        "main.cpp"
        "auth/auth_config.cpp"
        "auth/radius_client.cpp"
        "auth/radius_proto.cpp"
        "auth/radius_task.cpp"
        "auth/totp.cpp"
        "custom/app_metadata.cpp"
        "custom/crash_dump.cpp"
        "custom/gnss_console.cpp"
        "custom/gnss_nmea.cpp"
        "custom/gnss_uart.cpp"
        "custom/gnss_ubx.cpp"
        "custom/ntp_auth.cpp"
        "custom/ptp_auth.cpp"
        "custom/ntp_cache.c"
        "custom/time_diag.cpp"
        "net/ip_text.cpp"
        "third_party/qrcodegen/qrcodegen.c"
        "web/web_auth.cpp"
        "web/web_net.cpp"
        "web/web_ote.cpp"
        "web/web_server.cpp"
        "web/web_settings.cpp"
        "web/web_tls.cpp"
)

set(MAIN_REQUIRES
        esp_eth
        esp_event
        esp_netif
        esp_hw_support
        esp_timer
        esp_http_server
        esp-tls
        app_update
        bootloader_support
        espcoredump
        esp_partition
        spi_flash
        mdns
        nvs_flash
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_mcpwm
        esp_driver_uart
        lwip
        mbedtls
        mqtt
        fatfs
        sdmmc
        led_strip
        cjson
)
# TF card and LCD driver per target: the P4 has the TF slot on SDMMC and uses esp-idf-lib's HD44780 / PCF8574 driver;
# the S31 board has an external TF reader on SPI, and esp-idf-lib does not declare the S31 (upstream 4.0 uses
# jef-sure/hd44780 there)
if(IDF_TARGET STREQUAL "esp32s31")
    list(APPEND MAIN_REQUIRES esp_driver_sdspi esp_driver_spi jef-sure__hd44780)
else()
    list(APPEND MAIN_REQUIRES esp_driver_sdmmc hd44780 pcf8574)
endif()

idf_component_register(
    SRCS ${MAIN_SOURCES}
    INCLUDE_DIRS
        "."
    REQUIRES ${MAIN_REQUIRES}
    # Web admin page: separate files, served as they are (no inline script or style, see the CSP)
    EMBED_TXTFILES
        "web/www/index.html"
        "web/www/app.js"
        "web/www/i18n.js"
        "web/www/app.css"
    # Favicon: self-drawn alarm clock (U+23F0), 16 to 64 px PNG images in an ICO container
    EMBED_FILES
        "web/www/favicon.ico"
)

# qrcodegen (THIRD_PARTY_NOTICES) is taken unchanged: its warnings are not errors
set_source_files_properties("third_party/qrcodegen/qrcodegen.c" PROPERTIES COMPILE_OPTIONS "-Wno-error")

# ---------------------------------------------------------------------------
# Embed custom metadata into the binary
# ---------------------------------------------------------------------------

# Build date/time
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")

# Git SHA (short)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Metadata definitions injected into the main component
target_compile_definitions(${COMPONENT_TARGET} PRIVATE
    APP_PROJECT_NAME="${CMAKE_PROJECT_NAME}"
    APP_VERSION="${PROJECT_VER}"
    APP_COPYRIGHT="Copyright Rob Latour, 2026"
    APP_LICENSE="MIT"
    APP_HOMEPAGE="https://github.com/roblatour/ESP32TimeServer"
    APP_BUILD_DATE="${BUILD_DATE}"
    APP_BUILD_TIME="${BUILD_TIME}"
    APP_GIT_SHA="${GIT_SHA}"
    # Debug log lines on the console (UART0 at CONFIG_ESP_CONSOLE_UART_BAUDRATE): 0 = off, 1 = on. 1 degrades the
    # accuracy and the performance during periods of high volume NTP requests.
    DEBUG_ENABLED=0
)
