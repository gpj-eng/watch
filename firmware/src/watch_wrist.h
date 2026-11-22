#ifndef WATCH_WRIST_H
#define WATCH_WRIST_H

#include "alarm.h"

enum wrist_screen { RW_SCREEN_OFF, RW_SCREEN_ON, RW_SCREEN_PROTECTED };
enum wrist_event { RW_EVT_NONE, RW_EVT_SLEEP_SCREEN };
enum forearm_axis { WRIST_FOREARM_X, WRIST_FOREARM_Y };

typedef struct {
    float alpha, view_z_g, view_forearm_max_g, sleep_z_g;
    uint16_t max_gap_ms;
    uint8_t sleep_frames;
    enum forearm_axis forearm;
} wrist_sleep_cfg_t;

struct watch_wrist {
    float gravity[3]; /* g, positive Z toward the face of the display */
    uint32_t last_ms;
    enum wrist_screen screen;
    uint8_t count;
    bool initialized, viewed;
};

extern const wrist_sleep_cfg_t wrist_defaults;
bool watch_wrist_still(const struct watch_imu *sample);
int32_t watch_wrist_face_up_mg(const struct watch_imu *sample);
enum wrist_event watch_wrist_update(struct watch_wrist *w, const wrist_sleep_cfg_t *cfg,
                                    const struct watch_imu *s, uint32_t now, enum wrist_screen screen);

/* Serial diagnostics consume copies of the same samples as the detector. */
void wrist_console_init(void);
bool wrist_console_take_cfg(wrist_sleep_cfg_t *cfg);
void wrist_console_record(const struct watch_imu *s, const struct watch_wrist *w,
                          enum wrist_event event, uint32_t now);

#endif
