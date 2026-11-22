#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_epoch(uint8_t frame[WATCH_FRAME_SIZE], uint32_t epoch)
{
    for (unsigned i = 0; i < 4; ++i)
        frame[16 + i] = (uint8_t)(epoch >> (8u * i));
}

int main(void)
{
    uint8_t frame[WATCH_FRAME_SIZE] = {
        WATCH_WEATHER_TYPE, 1, 71, 93, 18, 0x85, 0xff,
        0, 71, 255, 8, 0xfc, 0x80, 0xfd, 0xf9, 0x80,
    };
    const uint32_t fetched_at_utc = WATCH_TIME_MIN + 1234567u;
    set_epoch(frame, fetched_at_utc);
    struct watch_weather weather;
    assert(watch_frame_weather(frame, sizeof(frame), &weather));
    assert(weather.current_wmo == 71 && weather.humidity_percent == 93 && weather.wind_kmh == 18);
    assert(weather.temperature_deci_c == -123 && weather.fetched_at_utc == fetched_at_utc);
    assert(weather.daily_wmo[0] == 0 && weather.daily_wmo[1] == 71);
    assert(weather.daily_wmo[2] == WATCH_WEATHER_MISSING_U8);
    assert(weather.daily_high_c[0] == 8 && weather.daily_high_c[1] == -4);
    assert(weather.daily_low_c[0] == -3 && weather.daily_low_c[1] == -7);
    assert(weather.daily_high_c[2] == WATCH_WEATHER_MISSING_S8);
    assert(weather.daily_low_c[2] == WATCH_WEATHER_MISSING_S8);

    frame[2] = WATCH_WEATHER_MISSING_U8;
    frame[3] = WATCH_WEATHER_MISSING_U8;
    frame[4] = WATCH_WEATHER_MISSING_U8;
    frame[5] = 0;
    frame[6] = 0x80;
    assert(watch_frame_weather(frame, sizeof(frame), &weather));
    assert(weather.current_wmo == WATCH_WEATHER_MISSING_U8);
    assert(weather.humidity_percent == WATCH_WEATHER_MISSING_U8);
    assert(weather.wind_kmh == WATCH_WEATHER_MISSING_U8);
    assert(weather.temperature_deci_c == WATCH_WEATHER_MISSING_S16);

    unsigned char before[sizeof(weather)];
    memcpy(before, &weather, sizeof(weather));
    assert(!watch_frame_weather(NULL, sizeof(frame), &weather));
    assert(!watch_frame_weather(frame, sizeof(frame), NULL));
    assert(!watch_frame_weather(frame, sizeof(frame) - 1, &weather));
    frame[0] = WATCH_SOS_ACK_TYPE;
    assert(!watch_frame_weather(frame, sizeof(frame), &weather));
    frame[0] = WATCH_WEATHER_TYPE;
    frame[1] = 2;
    assert(!watch_frame_weather(frame, sizeof(frame), &weather));
    frame[1] = 1;
    frame[2] = 100;
    assert(!watch_frame_weather(frame, sizeof(frame), &weather));
    frame[2] = 99;
    frame[3] = 101;
    assert(!watch_frame_weather(frame, sizeof(frame), &weather));
    frame[3] = 100;
    frame[9] = 254;
    assert(!watch_frame_weather(frame, sizeof(frame), &weather));
    frame[9] = 95;
    set_epoch(frame, WATCH_TIME_MIN - 1);
    assert(!watch_frame_weather(frame, sizeof(frame), &weather));
    set_epoch(frame, WATCH_TIME_LIMIT);
    assert(!watch_frame_weather(frame, sizeof(frame), &weather));
    assert(memcmp(before, &weather, sizeof(weather)) == 0);

    set_epoch(frame, WATCH_TIME_MIN);
    assert(watch_frame_weather(frame, sizeof(frame), &weather));
    set_epoch(frame, WATCH_TIME_LIMIT - 1);
    assert(watch_frame_weather(frame, sizeof(frame), &weather));
    puts("PASS weather decoding, missing measurements and invalid frames");
}
