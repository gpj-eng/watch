#include "protocol.h"
#include <string.h>

static void little_endian(uint8_t *dest, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        dest[i] = (uint8_t)(value >> (8u * i));
}

static uint64_t little_endian_read(const uint8_t *src, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= (uint64_t)src[i] << (8u * i);
    return value;
}

static int32_t little_endian_read_i16(const uint8_t *src)
{
    uint16_t raw = (uint16_t)little_endian_read(src, 2);
    return raw >= 0x8000u ? (int32_t)raw - 0x10000 : (int32_t)raw;
}

bool watch_valid_phone(const char *phone)
{
    if (phone[0] != '+' || phone[1] < '1' || phone[1] > '9')
        return false;
    for (unsigned i = 1; i < WATCH_PHONE_SIZE; ++i) {
        if (!phone[i])
            return true;
        if (phone[i] < '0' || phone[i] > '9')
            return false;
    }
    return false;
}

void watch_frame_imu(uint8_t out[20], uint16_t seq, uint32_t ms, const struct watch_imu *s)
{
    const int16_t axes[] = {s->ax, s->ay, s->az, s->gx, s->gy, s->gz};
    out[0] = 1;
    out[1] = 1;
    little_endian(out + 2, seq, 2);
    little_endian(out + 4, ms, 4);
    for (unsigned i = 0; i < 6; ++i)
        little_endian(out + 8 + 2 * i, (uint16_t)axes[i], 2);
}

bool watch_frame_sos(uint8_t out[20], const struct watch_sos *sos, uint8_t index)
{
    if (!sos->id || sos->count > WATCH_MAX_CONTACTS || index >= sos->count ||
        !watch_valid_phone(sos->phones[index]))
        return false;
    out[0] = 2;
    out[1] = sos->reason;
    little_endian(out + 2, sos->id, 8);
    out[10] = index;
    out[11] = sos->count;
    memset(out + 12, 0xff, 8);
    for (unsigned i = 0; sos->phones[index][i + 1]; ++i) {
        uint8_t digit = (uint8_t)(sos->phones[index][i + 1] - '0');
        uint8_t *byte = out + 12 + i / 2;
        *byte = i % 2 ? (uint8_t)((*byte & 0xf0) | digit) : (uint8_t)((digit << 4) | 0x0f);
    }
    return true;
}

bool watch_frame_resolved(uint8_t out[WATCH_FRAME_SIZE], uint64_t id,
                          const uint8_t device[WATCH_DEVICE_ID_SIZE], uint32_t ms)
{
    static const uint8_t empty[WATCH_DEVICE_ID_SIZE];
    if (!id || !memcmp(device, empty, sizeof(empty)))
        return false;
    out[0] = WATCH_RESOLVED_TYPE;
    out[1] = WATCH_RESOLVED_BY_USER;
    little_endian(out + 2, id, 8);
    memcpy(out + 10, device, WATCH_DEVICE_ID_SIZE);
    little_endian(out + 16, ms, 4);
    return true;
}

bool watch_frame_ack(const uint8_t *frame, unsigned size, uint8_t *type, uint64_t *id, uint8_t *status)
{
    if (size != WATCH_FRAME_SIZE ||
        (frame[0] != WATCH_SOS_ACK_TYPE && frame[0] != WATCH_RESOLVED_ACK_TYPE) ||
        frame[1] < 1 || frame[1] > 3)
        return false;
    uint64_t event = little_endian_read(frame + 2, 8);
    for (unsigned i = 10; i < size; ++i)
        if (frame[i])
            return false;
    if (!event)
        return false;
    *type = frame[0];
    *id = event;
    *status = frame[1];
    return true;
}

bool watch_frame_time(const uint8_t *frame, unsigned size, uint32_t *local_epoch)
{
    if (size != WATCH_FRAME_SIZE || frame[0] != WATCH_TIME_SYNC_TYPE || frame[1] != 1)
        return false;
    for (unsigned i = 8; i < size; ++i)
        if (frame[i])
            return false;

    uint32_t utc = (uint32_t)little_endian_read(frame + 2, 4);
    int offset_minutes = (int)little_endian_read_i16(frame + 6);
    if (offset_minutes < -840 || offset_minutes > 840)
        return false;
    const int64_t local = (int64_t)utc + offset_minutes * 60;
    if (local < WATCH_TIME_MIN || local >= WATCH_TIME_LIMIT)
        return false;
    *local_epoch = (uint32_t)local;
    return true;
}

static bool valid_wmo(uint8_t code)
{
    return code < 100u || code == WATCH_WEATHER_MISSING_U8;
}

static int8_t signed_weather_byte(uint8_t byte)
{
    return (int8_t)(byte <= INT8_MAX ? (int)byte : (int)byte - 256);
}

bool watch_frame_weather(const uint8_t *frame, unsigned size, struct watch_weather *weather)
{
    if (!frame || !weather || size != WATCH_FRAME_SIZE || frame[0] != WATCH_WEATHER_TYPE || frame[1] != 1 ||
        !valid_wmo(frame[2]) || (frame[3] > 100u && frame[3] != WATCH_WEATHER_MISSING_U8))
        return false;

    int16_t temperature = (int16_t)little_endian_read_i16(frame + 5);
    struct watch_weather next = {
        .current_wmo = frame[2],
        .humidity_percent = frame[3],
        .wind_kmh = frame[4],
        .temperature_deci_c = (int16_t)temperature,
    };
    for (unsigned i = 0; i < WATCH_WEATHER_DAYS; ++i) {
        if (!valid_wmo(frame[7 + i]))
            return false;
        next.daily_wmo[i] = frame[7 + i];
        next.daily_high_c[i] = signed_weather_byte(frame[10 + i]);
        next.daily_low_c[i] = signed_weather_byte(frame[13 + i]);
    }
    next.fetched_at_utc = (uint32_t)little_endian_read(frame + 16, 4);
    if (next.fetched_at_utc < WATCH_TIME_MIN || next.fetched_at_utc >= WATCH_TIME_LIMIT)
        return false;

    *weather = next;
    return true;
}
