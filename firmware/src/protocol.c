#include "protocol.h"
#include <string.h>

static void little_endian(uint8_t *dest, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        dest[i] = (uint8_t)(value >> (8u * i));
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

bool watch_frame_ack(const uint8_t *frame, unsigned size, uint64_t *id, uint8_t *status)
{
    if (size != WATCH_FRAME_SIZE || frame[0] != 0x81 || frame[1] < 1 || frame[1] > 3)
        return false;
    uint64_t event = 0;
    for (unsigned i = 0; i < 8; ++i)
        event |= (uint64_t)frame[2 + i] << (8u * i);
    for (unsigned i = 10; i < size; ++i)
        if (frame[i])
            return false;
    if (!event)
        return false;
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

    uint32_t utc = 0;
    for (unsigned i = 0; i < 4; ++i)
        utc |= (uint32_t)frame[2 + i] << (8u * i);
    int offset_minutes = frame[6] | (frame[7] << 8);
    if (offset_minutes >= 32768)
        offset_minutes -= 65536;
    if (offset_minutes < -840 || offset_minutes > 840)
        return false;
    const int64_t local = (int64_t)utc + offset_minutes * 60;
    if (local < WATCH_TIME_MIN || local >= WATCH_TIME_LIMIT)
        return false;
    *local_epoch = (uint32_t)local;
    return true;
}
