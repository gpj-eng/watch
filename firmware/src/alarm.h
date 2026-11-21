#ifndef WATCH_ALARM_H
#define WATCH_ALARM_H

#include <stdbool.h>
#include <stdint.h>

enum watch_reason { WATCH_FALL = 1, WATCH_MANUAL_SOS = 2 };
enum watch_alarm_phase {
    ALARM_STATE_IDLE,
    ALARM_STATE_COUNTDOWN_NORMAL,
    ALARM_STATE_COUNTDOWN_URGENT,
    ALARM_STATE_SOS_ACTIVE,
};

enum watch_alarm_event {
    EVT_ALARM_NONE,
    EVT_ALARM_URGENT,
    EVT_ALARM_CANCEL,
    EVT_SOS_TIMEOUT,
    EVT_SOS_MANUAL,
    EVT_SOS_VOICE_DONE,
    EVT_SOS_VOICE_FAILED = 7,
    EVT_SOS_RESOLVED = 8,
};

typedef struct {
    uint32_t cancel_timeout_ms;
    uint32_t vib_normal_on_ms, vib_normal_off_ms;
    uint32_t vib_urgent_on_ms, vib_urgent_off_ms;
    uint32_t sos_vib_on_ms, sos_vib_off_ms;
    uint32_t cancel_hold_ms;
    uint32_t sos_audio_max_ms;
    uint8_t vib_pwm_duty;
    uint8_t sos_audio_volume;
} alarm_cfg_t;

extern const alarm_cfg_t alarm_cfg_default;

struct watch_alarm {
    const alarm_cfg_t *cfg;
    enum watch_alarm_phase phase;
    uint32_t started_ms;
    uint32_t sos_started_ms;
    uint32_t hold_started_ms;
    enum watch_reason reason;
    bool demo, holding;
};

void watch_alarm_fall(struct watch_alarm *alarm, uint32_t now, bool demo);
enum watch_alarm_event watch_alarm_manual(struct watch_alarm *alarm);
enum watch_alarm_event watch_alarm_update(struct watch_alarm *alarm, uint32_t now, bool cancel);
bool watch_alarm_counting(const struct watch_alarm *alarm);
enum watch_alarm_event watch_alarm_hold(struct watch_alarm *alarm, bool pressed, bool new_press, uint32_t now);
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
