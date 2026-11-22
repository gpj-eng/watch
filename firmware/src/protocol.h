#ifndef WATCH_PROTOCOL_H
#define WATCH_PROTOCOL_H

#include "alarm.h"

#define WATCH_FRAME_SIZE 20u
#define WATCH_MAX_CONTACTS 3u
#define WATCH_PHONE_SIZE 17u
#define WATCH_SOS_ACK_TYPE 0x81u
#define WATCH_TIME_SYNC_TYPE 0x82u
#define WATCH_RESOLVED_ACK_TYPE 0x83u
#define WATCH_WEATHER_TYPE 0x84u
#define WATCH_RESOLVED_TYPE 0x03u
#define WATCH_RESOLVED_BY_USER 1u
#define WATCH_DEVICE_ID_SIZE 6u
#define WATCH_TIME_MIN 1735689600u   /* 2025-01-01, earliest valid RTC date. */
#define WATCH_TIME_LIMIT 4102444800u /* 2100-01-01, outside the SDK date range. */
#define WATCH_WEATHER_DAYS 3u
#define WATCH_WEATHER_MISSING_U8 UINT8_MAX
#define WATCH_WEATHER_MISSING_S8 INT8_MIN
#define WATCH_WEATHER_MISSING_S16 INT16_MIN

struct watch_sos {
    uint64_t id;
    uint8_t reason;
    uint8_t count;
    char phones[WATCH_MAX_CONTACTS][WATCH_PHONE_SIZE];
};

struct watch_weather {
    uint8_t current_wmo;
    uint8_t humidity_percent;
    uint8_t wind_kmh;
    int16_t temperature_deci_c;
    uint8_t daily_wmo[WATCH_WEATHER_DAYS];
    int8_t daily_high_c[WATCH_WEATHER_DAYS];
    int8_t daily_low_c[WATCH_WEATHER_DAYS];
    uint32_t fetched_at_utc;
};

bool watch_valid_phone(const char *phone);
void watch_frame_imu(uint8_t out[20], uint16_t seq, uint32_t ms, const struct watch_imu *imu);
bool watch_frame_sos(uint8_t out[20], const struct watch_sos *sos, uint8_t index);
bool watch_frame_resolved(uint8_t out[WATCH_FRAME_SIZE], uint64_t id,
                          const uint8_t device[WATCH_DEVICE_ID_SIZE], uint32_t ms);
bool watch_frame_ack(const uint8_t *frame, unsigned size, uint8_t *type, uint64_t *id, uint8_t *status);
bool watch_frame_time(const uint8_t *frame, unsigned size, uint32_t *local_epoch);
bool watch_frame_weather(const uint8_t *frame, unsigned size, struct watch_weather *weather);

#endif
