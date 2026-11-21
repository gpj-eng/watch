#ifndef WATCH_WRIST_H
#define WATCH_WRIST_H

#include "alarm.h"

struct watch_wrist {
    uint32_t last_sample_ms, started_ms;
    uint8_t fast_samples, steady_samples;
    bool have_sample, armed, turning;
};

bool watch_wrist_still(const struct watch_imu *sample);
int32_t watch_wrist_face_up_mg(const struct watch_imu *sample);
bool watch_wrist_update(struct watch_wrist *wrist, const struct watch_imu *sample, uint32_t now);

#endif
