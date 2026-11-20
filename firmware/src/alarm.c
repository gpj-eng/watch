#include "alarm.h"
#include <math.h>

void watch_alarm_fall(struct watch_alarm *alarm, uint32_t now, bool demo)
{
    if (alarm->phase == WATCH_COUNTDOWN || alarm->phase == WATCH_SUBMIT)
        return;
    *alarm = (struct watch_alarm){WATCH_COUNTDOWN, now, WATCH_FALL, demo};
}

void watch_alarm_manual(struct watch_alarm *alarm, uint32_t now)
{
    *alarm = (struct watch_alarm){WATCH_SUBMIT, now, WATCH_MANUAL_SOS, false};
}

void watch_alarm_update(struct watch_alarm *alarm, uint32_t now, bool cancel)
{
    if (alarm->phase != WATCH_COUNTDOWN)
        return;
    if (now - alarm->started_ms >= WATCH_CANCEL_MS)
        alarm->phase = WATCH_SUBMIT;
    else if (cancel)
        alarm->phase = WATCH_CANCELLED;
}

bool watch_alarm_vibrate(const struct watch_alarm *alarm, uint32_t now)
{
    if (alarm->phase != WATCH_COUNTDOWN)
        return false;
    return now - alarm->started_ms < WATCH_VIBRATION_MS;
}

unsigned watch_alarm_seconds(const struct watch_alarm *alarm, uint32_t now)
{
    uint32_t elapsed = now - alarm->started_ms;
    return elapsed < WATCH_CANCEL_MS ? (WATCH_CANCEL_MS - elapsed + 999u) / 1000u : 0u;
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
