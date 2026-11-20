#ifndef WATCH_PROTOCOL_H
#define WATCH_PROTOCOL_H

#include "alarm.h"

#define WATCH_FRAME_SIZE 20u
#define WATCH_MAX_CONTACTS 3u
#define WATCH_PHONE_SIZE 17u

struct watch_sos {
    uint64_t id;
    uint8_t reason;
    uint8_t count;
    char phones[WATCH_MAX_CONTACTS][WATCH_PHONE_SIZE];
};

bool watch_valid_phone(const char *phone);
void watch_frame_imu(uint8_t out[20], uint16_t seq, uint32_t ms, const struct watch_imu *imu);
bool watch_frame_sos(uint8_t out[20], const struct watch_sos *sos, uint8_t index);
bool watch_frame_ack(const uint8_t *frame, unsigned size, uint64_t *id, uint8_t *status);

#endif
