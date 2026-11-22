#include "watch_wrist.h"
#include "watch_board.h"
#include <finsh.h>
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

struct wrist_row {
    uint32_t ms;
    struct watch_imu imu;
    int16_t filtered_mg[3];
    uint8_t count, screen, event;
};

static struct rt_messagequeue rows;
static rt_ubase_t row_pool[256];
static volatile bool recording, cfg_changed;
static bool ready;
static volatile unsigned overruns;
static wrist_sleep_cfg_t settings;

void wrist_console_init(void)
{
    settings = wrist_defaults;
    ready = rt_mq_init(&rows, "wristlog", row_pool, sizeof(struct wrist_row),
                       sizeof(row_pool), RT_IPC_FLAG_FIFO) == RT_EOK;
}

bool wrist_console_take_cfg(wrist_sleep_cfg_t *cfg)
{
    if (!cfg_changed)
        return false;
    rt_enter_critical();
    *cfg = settings;
    cfg_changed = false;
    rt_exit_critical();
    return true;
}

void wrist_console_record(const struct watch_imu *s, const struct watch_wrist *w,
                          enum wrist_event event, uint32_t now)
{
    if (!recording)
        return;
    struct wrist_row row = {.ms = now, .imu = *s, .count = w->count,
                            .screen = w->screen, .event = event};
    for (unsigned i = 0; i < 3; ++i)
        row.filtered_mg[i] = (int16_t)(1000.f * w->gravity[i]);
    if (rt_mq_send(&rows, &row, sizeof(row)) != RT_EOK)
        ++overruns;
}

static unsigned positive(const char *text, unsigned max)
{
    char *end;
    long v = strtol(text, &end, 10);
    return end != text && !*end && v > 0 && v <= (long)max ? (unsigned)v : 0;
}

static void format_g(char out[12], int mg)
{
    int magnitude = abs(mg);
    rt_snprintf(out, 12, "%s%d.%03d", mg < 0 ? "-" : "", magnitude / 1000, magnitude % 1000);
}

/* The shell thread consumes the queue. The IMU thread never waits for serial output. */
static void capture(unsigned seconds, const char *hand, const char *pose)
{
    int sum[3] = {0}, min[3] = {0}, max[3] = {0};
    unsigned n = 0, moving = 0, gaps = 0;
    uint32_t last = 0;
    rt_mq_control(&rows, RT_IPC_CMD_RESET, RT_NULL);
    overruns = 0;
    uint32_t start = watch_now_ms();
    recording = true;
    while (watch_now_ms() - start < seconds * 1000u && (!hand || n < 52u)) {
        struct wrist_row row;
        if (rt_mq_recv(&rows, &row, sizeof(row), rt_tick_from_millisecond(250)) != RT_EOK)
            continue;
        const struct watch_imu *s = &row.imu;
        if (n && row.ms - last > settings.max_gap_ms)
            ++gaps;
        last = row.ms;
        if (hand) {
            const int a[] = {s->ax, s->ay, s->az};
            for (unsigned i = 0; i < 3; ++i) {
                sum[i] += a[i];
                if (!n || a[i] < min[i]) min[i] = a[i];
                if (!n || a[i] > max[i]) max[i] = a[i];
            }
            moving += !watch_wrist_still(s);
            char ax[12], ay[12], az[12];
            format_g(ax, s->ax);
            format_g(ay, s->ay);
            format_g(az, s->az);
            rt_kprintf("RW_RAW ms=%u ax_g=%s ay_g=%s az_g=%s\n", (unsigned)row.ms, ax, ay, az);
        } else {
            rt_kprintf("RW ms=%u a=%d,%d,%d w=%d,%d,%d g=%d,%d,%d %s n=%u screen=%u evt=%s\n",
                       (unsigned)row.ms, s->ax, s->ay, s->az, s->gx, s->gy, s->gz,
                       row.filtered_mg[0], row.filtered_mg[1], row.filtered_mg[2],
                       row.screen == RW_SCREEN_OFF ? "IDLE" : "AWAKE", row.count, row.screen,
                       row.event == RW_EVT_SLEEP_SCREEN ? "SLEEP" : "-");
        }
        ++n;
    }
    recording = false;
    if (hand && n) {
        char ax[12], ay[12], az[12];
        format_g(ax, sum[0] / (int)n);
        format_g(ay, sum[1] / (int)n);
        format_g(az, sum[2] / (int)n);
        bool stable = n == 52 && !moving && !overruns && !gaps;
        for (unsigned i = 0; i < 3; ++i)
            stable &= max[i] - min[i] <= 100;
        rt_kprintf("RW_CAL hand=%s pose=%s mean_g=%s,%s,%s stable=%u\n", hand, pose, ax, ay, az, stable);
    }
    rt_kprintf("RW_END frames=%u drops=%u gaps=%u\n", n, overruns, gaps);
}

static void watch_rw(int argc, char **argv)
{
    if (!ready) {
        rt_kprintf("RW serial capture unavailable\n");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "cal") &&
        (!strcmp(argv[2], "left") || !strcmp(argv[2], "right")) &&
        (!strcmp(argv[3], "down") || !strcmp(argv[3], "view") || !strcmp(argv[3], "flat"))) {
        capture(3, argv[2], argv[3]);
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "trace") && positive(argv[2], 120)) {
        capture(positive(argv[2], 120), RT_NULL, RT_NULL);
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "axis") && (!strcmp(argv[2], "x") || !strcmp(argv[2], "y"))) {
        rt_enter_critical();
        settings.forearm = argv[2][0] == 'x' ? WRIST_FOREARM_X : WRIST_FOREARM_Y;
        cfg_changed = true;
        rt_exit_critical();
    } else if (argc != 1) {
        rt_kprintf("watch_rw cal left|right flat|down|view / trace 1..120 / axis x|y\n");
        return;
    }
    rt_kprintf("RW_CFG forearm=%s\n", settings.forearm == WRIST_FOREARM_X ? "x" : "y");
}
MSH_CMD_EXPORT(watch_rw, Calibrate and trace wrist-down screen sleep);
