#include "watch_wrist.h"
#include <math.h>
#include <stdint.h>
#include <rtconfig.h>

const wrist_sleep_cfg_t wrist_defaults = {
    .alpha = 0.20f, .view_z_g = 0.70f, .view_forearm_max_g = 0.65f, .sleep_z_g = 0.35f,
    .raise_step_mg = 230, .max_gap_ms = 35, .sleep_frames = 16,
    .forearm = WRIST_FOREARM_Y,
};

static float norm_sq(float x, float y, float z)
{
    return x*x + y*y + z*z;
}

int32_t watch_wrist_face_up_mg(const struct watch_imu *s)
{
#ifdef WATCH_DIAL_FACE_UP_NEGATIVE_Z
    return -(int32_t)s->az;
#else
    return s->az;
#endif
}

bool watch_wrist_still(const struct watch_imu *s)
{
    float a2 = norm_sq(s->ax, s->ay, s->az);
    return a2 >= 900.f*900.f && a2 <= 1100.f*1100.f && norm_sq(s->gx, s->gy, s->gz) <= 120.f*120.f;
}

static void sync_screen(struct watch_wrist *w, enum wrist_screen screen)
{
    w->count = 0;
    w->viewed = false;
    w->screen = screen;
    w->raise_min_mg = INT32_MAX;
}

enum wrist_event watch_wrist_update(struct watch_wrist *w, const wrist_sleep_cfg_t *cfg,
                                    const struct watch_imu *s, uint32_t now, enum wrist_screen screen)
{
    const bool gap = !w->initialized || now - w->last_ms > cfg->max_gap_ms;
    const float g[] = {s->ax / 1000.f, s->ay / 1000.f, watch_wrist_face_up_mg(s) / 1000.f};
    for (unsigned i = 0; i < 3; ++i)
        w->gravity[i] = gap ? g[i] : w->gravity[i] + cfg->alpha * (g[i] - w->gravity[i]);
    if (gap || w->screen != screen)
        sync_screen(w, screen);
    w->initialized = true;
    w->last_ms = now;

    const float z = w->gravity[2];
    const bool view = z >= cfg->view_z_g && fabsf(w->gravity[cfg->forearm]) <= cfg->view_forearm_max_g;

    const int32_t forearm_mg = cfg->forearm == WRIST_FOREARM_X ? s->ax : s->ay;
    if (forearm_mg < w->raise_min_mg)
        w->raise_min_mg = forearm_mg;

    if (screen == RW_SCREEN_OFF) {
        /* Raise to wake, ported from InfiniTime ShouldRaiseWake() (MotionController.cpp,
           MPL-2.0): while the face is dark, wake once the wrist is in the viewing pose and
           the forearm axis has turned raise_step_mg off its hanging value. InfiniTime gates
           the pose with |accel x| <= 335 mg, which never holds on this mount (README: the
           viewing pose reads |ax| = 564 mg), so the gate is this watch's calibrated pose. */
        if (view && forearm_mg - w->raise_min_mg >= cfg->raise_step_mg) {
            sync_screen(w, RW_SCREEN_ON);
            return RW_EVT_WAKE_SCREEN;
        }
        return RW_EVT_NONE;
    }

    if (screen != RW_SCREEN_ON)
        return RW_EVT_NONE;

    /* A key can wake the face while the wrist is already down.
       Require a transition away from a viewing pose before putting it back to sleep. */
    w->viewed |= view;
    if (w->viewed && z <= cfg->sleep_z_g) {
        if (++w->count >= cfg->sleep_frames) {
            sync_screen(w, RW_SCREEN_OFF);
            return RW_EVT_SLEEP_SCREEN;
        }
    } else {
        w->count = 0;
    }
    return RW_EVT_NONE;
}
