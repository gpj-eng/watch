#ifndef WATCH_ALARM_H
#define WATCH_ALARM_H

#include <stdbool.h>
#include <stdint.h>

#define WATCH_VIBRATION_MS 10000u
#define WATCH_CANCEL_MS 30000u

enum watch_reason { WATCH_FALL = 1, WATCH_MANUAL_SOS = 2 };
enum watch_alarm_phase { WATCH_IDLE, WATCH_COUNTDOWN, WATCH_CANCELLED, WATCH_SUBMIT };

struct watch_alarm {
    enum watch_alarm_phase phase;
    uint32_t started_ms;
    enum watch_reason reason;
    bool demo;
};

void watch_alarm_fall(struct watch_alarm *alarm, uint32_t now, bool demo);
void watch_alarm_manual(struct watch_alarm *alarm, uint32_t now);
void watch_alarm_update(struct watch_alarm *alarm, uint32_t now, bool cancel);
bool watch_alarm_vibrate(const struct watch_alarm *alarm, uint32_t now);
unsigned watch_alarm_seconds(const struct watch_alarm *alarm, uint32_t now);

struct watch_imu {
    int16_t ax, ay, az;  /* mg */
    int16_t gx, gy, gz;  /* tenths of a degree per second */
};

struct watch_detector {
    float rest[3], pre_impact[3], settled[3];
    uint32_t impact_ms, last_ms;
    uint16_t quiet_samples;
    bool initialized, candidate;
};

bool watch_detect(struct watch_detector *detector, const struct watch_imu *sample, uint32_t now);

#endif
