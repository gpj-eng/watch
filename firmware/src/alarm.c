#include "alarm.h"
#include <math.h>

const alarm_cfg_t alarm_cfg_default = {
    .cancel_timeout_ms = 30000u,
    .vib_normal_on_ms = 600u,
    .vib_normal_off_ms = 900u,
    .vib_urgent_on_ms = 500u,
    .vib_urgent_off_ms = 400u,
    .sos_voice_interval_ms = 3000u,
    .sos_voice_stop_hold_ms = 2000u,
    .vib_pwm_duty = 50u,
    .sos_voice_repeat_max = 10u,
};

static uint32_t normal_phase_ms(const struct watch_alarm *alarm)
{
    return (uint32_t)((uint64_t)alarm->cfg->cancel_timeout_ms * 2u / 3u);
}

bool watch_alarm_counting(const struct watch_alarm *alarm)
{
    return alarm->phase == ALARM_STATE_COUNTDOWN_NORMAL || alarm->phase == ALARM_STATE_COUNTDOWN_URGENT;
}

void watch_alarm_fall(struct watch_alarm *alarm, uint32_t now, bool demo)
{
    if (alarm->phase != ALARM_STATE_IDLE)
        return;
    alarm->phase = ALARM_STATE_COUNTDOWN_NORMAL;
    alarm->started_ms = now;
    alarm->reason = WATCH_FALL;
    alarm->demo = demo;
}

enum watch_alarm_event watch_alarm_manual(struct watch_alarm *alarm)
{
    if (alarm->phase != ALARM_STATE_IDLE)
        return EVT_ALARM_NONE;
    alarm->reason = WATCH_MANUAL_SOS;
    alarm->demo = false;
    return EVT_SOS_MANUAL;
}

enum watch_alarm_event watch_alarm_update(struct watch_alarm *alarm, uint32_t now, bool cancel)
{
    if (!watch_alarm_counting(alarm))
        return EVT_ALARM_NONE;
    uint32_t elapsed = now - alarm->started_ms;
    if (elapsed >= alarm->cfg->cancel_timeout_ms) {
        alarm->phase = ALARM_STATE_SOS_ACTIVE;
        return EVT_SOS_TIMEOUT;
    }
    if (cancel) {
        alarm->phase = ALARM_STATE_IDLE;
        return EVT_ALARM_CANCEL;
    }
    if (alarm->phase == ALARM_STATE_COUNTDOWN_NORMAL && elapsed >= normal_phase_ms(alarm)) {
        alarm->phase = ALARM_STATE_COUNTDOWN_URGENT;
        return EVT_ALARM_URGENT;
    }
    return EVT_ALARM_NONE;
}

bool watch_alarm_finish_voice(struct watch_alarm *alarm, enum watch_alarm_event event)
{
    if (alarm->phase != ALARM_STATE_SOS_ACTIVE)
        return false;
    if (event != EVT_SOS_VOICE_DONE && event != EVT_SOS_VOICE_STOPPED && event != EVT_SOS_VOICE_FAILED)
        return false;
    alarm->phase = ALARM_STATE_IDLE;
    return true;
}

bool watch_alarm_vibrate(const struct watch_alarm *alarm, uint32_t now)
{
    if (!watch_alarm_counting(alarm))
        return false;
    uint32_t elapsed = now - alarm->started_ms;
    const alarm_cfg_t *cfg = alarm->cfg;
    if (elapsed >= cfg->cancel_timeout_ms)
        return false;
    uint32_t normal = normal_phase_ms(alarm);
    bool urgent = elapsed >= normal;
    uint32_t on = urgent ? cfg->vib_urgent_on_ms : cfg->vib_normal_on_ms;
    uint32_t off = urgent ? cfg->vib_urgent_off_ms : cfg->vib_normal_off_ms;
    uint64_t period = (uint64_t)on + off;
    uint32_t phase_elapsed = urgent ? elapsed - normal : elapsed;
    return period && phase_elapsed % period < on;
}

unsigned watch_alarm_seconds(const struct watch_alarm *alarm, uint32_t now)
{
    uint32_t elapsed = now - alarm->started_ms;
    if (!watch_alarm_counting(alarm) || elapsed >= alarm->cfg->cancel_timeout_ms)
        return 0;
    uint32_t remaining = alarm->cfg->cancel_timeout_ms - elapsed;
    return remaining / 1000u + (remaining % 1000u != 0);
}

static float magnitude(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

bool watch_detect(struct watch_detector *d, const struct watch_imu *s, uint32_t now)
{
    const float a[3] = {s->ax, s->ay, s->az};
    const float g[3] = {s->gx, s->gy, s->gz};
    const float accel = magnitude(a);
    const float gyro = magnitude(g);
    if (!d->initialized || now - d->last_ms > 200u) {
        *d = (struct watch_detector){.initialized = true};
        for (unsigned i = 0; i < 3; ++i)
            d->rest[i] = a[i];
    }
    d->last_ms = now;
    if (!d->candidate) {
        if (accel >= 2600.0f && magnitude(d->rest) >= 700.0f) {
            d->candidate = true;
            d->impact_ms = now;
            d->quiet_samples = 0;
            for (unsigned i = 0; i < 3; ++i)
                d->pre_impact[i] = d->rest[i];
        } else if (accel > 750.0f && accel < 1250.0f && gyro < 250.0f) {
            for (unsigned i = 0; i < 3; ++i)
                d->rest[i] += 0.04f * (a[i] - d->rest[i]);
        }
        return false;
    }

    if (now - d->impact_ms > 6000u) {
        d->candidate = false;
        return false;
    }
    if (accel < 800.0f || accel > 1200.0f || gyro > 180.0f) {
        d->quiet_samples = 0;
        return false;
    }
    for (unsigned i = 0; i < 3; ++i)
        d->settled[i] = d->quiet_samples ? d->settled[i] + 0.035f * (a[i] - d->settled[i]) : a[i];
    if (++d->quiet_samples < 156u)
        return false;

    float dot = 0.0f;
    for (unsigned i = 0; i < 3; ++i)
        dot += d->pre_impact[i] * d->settled[i];
    d->candidate = false;
    return dot < 0.7071068f * magnitude(d->pre_impact) * magnitude(d->settled);
}
