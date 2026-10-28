// ESP32 Time Server  
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

typedef struct
{
    const char *project_name;
    const char *version;
    const char *copyright;
    const char *license;
    const char *homepage;
    const char *build_date;
    const char *build_time;
    const char *git_sha;
} app_metadata_t;

const app_metadata_t *get_app_metadata();

#ifdef __cplusplus
// Year of the build (APP_BUILD_DATE "YYYY-MM-DD", main component only). A GNSS date before it is not plausible: a
// receiver without almanac or after a GPS week number rollover reports an old date. One threshold for the NMEA and
// the UBX path (review Z-23).
static_assert(sizeof(APP_BUILD_DATE) == sizeof("YYYY-MM-DD"), "APP_BUILD_DATE format");
constexpr int app_build_year()
{
    return (APP_BUILD_DATE[0] - '0') * 1000 + (APP_BUILD_DATE[1] - '0') * 100 + (APP_BUILD_DATE[2] - '0') * 10 +
           (APP_BUILD_DATE[3] - '0');
}
#endif
