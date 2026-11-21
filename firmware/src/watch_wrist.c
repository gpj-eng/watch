#include "watch_wrist.h"
#include <rtconfig.h>

#define WRIST_MAX_SAMPLE_GAP_MS 100u
#define WRIST_TURN_WINDOW_MS 800u
#define WRIST_FAST_SAMPLES 2u
#define WRIST_STEADY_SAMPLES 8u

static uint32_t squared(int32_t value)
{
    return (uint32_t)(value * value);
}

static uint32_t accel_squared(const struct watch_imu *sample)
{
    return squared(sample->ax) + squared(sample->ay) + squared(sample->az);
}

static uint32_t gyro_squared(const struct watch_imu *sample)
{
    return squared(sample->gx) + squared(sample->gy) + squared(sample->gz);
}

int32_t watch_wrist_face_up_mg(const struct watch_imu *sample)
{
#ifdef WATCH_DIAL_FACE_UP_NEGATIVE_Z
    return -sample->az;
#else
    return sample->az;
#endif
}

static bool within_tilt(const struct watch_imu *sample, uint32_t accel, unsigned cos_squared_milli)
{
    const int32_t up = watch_wrist_face_up_mg(sample);
    return up > 0 && (uint64_t)squared(up) * 1000u >= (uint64_t)accel * cos_squared_milli;
}

bool watch_wrist_still(const struct watch_imu *sample)
{
    const uint32_t accel = accel_squared(sample);
    return accel >= 900u * 900u && accel <= 1100u * 1100u &&
           gyro_squared(sample) <= 120u * 120u;
}

bool watch_wrist_update(struct watch_wrist *wrist, const struct watch_imu *sample, uint32_t now)
{
    if ((wrist->have_sample && now - wrist->last_sample_ms > WRIST_MAX_SAMPLE_GAP_MS) ||
        (wrist->turning && now - wrist->started_ms > WRIST_TURN_WINDOW_MS))
        *wrist = (struct watch_wrist){0};
    wrist->have_sample = true;
    wrist->last_sample_ms = now;

    const uint32_t accel = accel_squared(sample);
    if (!wrist->turning) {
        if (watch_wrist_still(sample))
            wrist->armed = !within_tilt(sample, accel, 329u); /* cos²(55°) × 1000 */
        if (!wrist->armed || squared(sample->gx) + squared(sample->gy) < 800u * 800u) {
            wrist->fast_samples = 0;
            return false;
        }
        if (++wrist->fast_samples < WRIST_FAST_SAMPLES)
            return false;
        wrist->turning = true;
        wrist->started_ms = now;
    }

    if (accel < 800u * 800u || accel > 1200u * 1200u ||
        gyro_squared(sample) > 180u * 180u || !within_tilt(sample, accel, 587u)) { /* cos²(40°) × 1000 */
        wrist->steady_samples = 0;
        return false;
    }
    if (++wrist->steady_samples < WRIST_STEADY_SAMPLES)
        return false;
    *wrist = (struct watch_wrist){0};
    return true;
}
