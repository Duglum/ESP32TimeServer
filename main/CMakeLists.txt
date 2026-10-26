# The optional features (LCD, KY-016 / WS2812 LED, MQTT, NTP authentication) are always compiled in: the *_ENABLED
# values of ESP32TimeServerSettings.h are their run-time defaults, the web admin page switches them.
set(MAIN_SOURCES
        "main.cpp"
        "custom/app_metadata.cpp"
        "custom/gnss_nmea.cpp"
        "custom/gnss_uart.cpp"
        "custom/gnss_ubx.cpp"
        "custom/ntp_auth.cpp"
        "custom/ntp_cache.c"
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
        mdns
        nvs_flash
        driver
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_mcpwm
        esp_driver_sdmmc
        esp_driver_uart
        lwip
        mbedtls
        mqtt
        fatfs
        sdmmc
        hd44780
        pcf8574
        led_strip
        cjson
)

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
)
