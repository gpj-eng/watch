#include "watch_battery.h"
#include <stdbool.h>
#include <rtdevice.h>
#include <rtthread.h>

#define BATTERY_ADC_CHANNEL 7u
#define CHARGER_REFRESH_MS 2000u
#define BATTERY_REFRESH_MS 15000u

#ifdef CHARGE_USING_AW32001
/* The chip's raw status read does not change the charging configuration. */
extern bool aw32001_get_charge_status(uint8_t *status);
#endif

static struct rt_semaphore sample_requested;
static volatile int8_t percent = -1;
static volatile bool charger_active;
static bool sampler_ready;
static uint32_t requested_at;

static int estimate_percent(uint32_t mv)
{
    /* Approximate single-cell state of charge from terminal voltage. */
    static const struct { uint16_t mv; uint8_t percent; } curve[] = {
        {3300, 0}, {3500, 5}, {3650, 15}, {3750, 35}, {3820, 55},
        {3920, 75}, {4050, 90}, {4200, 100},
    };
    if (mv < 2800u || mv > 4300u)
        return -1;
    if (mv <= curve[0].mv)
        return 0;
    for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (mv < curve[i].mv) {
            uint32_t delta = mv - curve[i - 1].mv;
            uint32_t range = curve[i].mv - curve[i - 1].mv;
            return curve[i - 1].percent + delta * (curve[i].percent - curve[i - 1].percent) / range;
        }
    }
    return 100;
}

static void battery_sampler(void *arg)
{
    rt_adc_device_t adc = arg;
    rt_tick_t sampled_at = rt_tick_get() - rt_tick_from_millisecond(BATTERY_REFRESH_MS);
    for (;;) {
        if (rt_sem_take(&sample_requested, RT_WAITING_FOREVER) != RT_EOK)
            continue;
        bool charging = false;
#ifdef CHARGE_USING_AW32001
        uint8_t status;
        if (rt_device_find("charge") && aw32001_get_charge_status(&status))
            charging = status >= 1u && status <= 3u;
#endif
        rt_tick_t now = rt_tick_get();
        if (charging == charger_active && now - sampled_at < rt_tick_from_millisecond(BATTERY_REFRESH_MS))
            continue;
        charger_active = charging;
        sampled_at = now;
        int estimate = -1;
        if (rt_adc_enable(adc, BATTERY_ADC_CHANNEL) == RT_EOK) {
            /* The SF32LB52 driver applies the VBAT divider and reports 0.1 mV. */
            uint32_t reading = rt_adc_read(adc, BATTERY_ADC_CHANNEL);
            rt_adc_disable(adc, BATTERY_ADC_CHANNEL);
            estimate = estimate_percent(reading / 10u);
        }
        percent = (int8_t)estimate;
    }
}

void watch_battery_init(void)
{
    rt_adc_device_t adc = (rt_adc_device_t)rt_device_find("bat1");
    if (!adc)
        return;
    rt_sem_init(&sample_requested, "ui_bat", 0, RT_IPC_FLAG_FIFO);
    rt_thread_t thread = rt_thread_create("ui_bat", battery_sampler, adc, 2048,
                                          RT_THREAD_PRIORITY_MAX * 3 / 4, 10);
    if (!thread)
        return;
    requested_at = 0u - CHARGER_REFRESH_MS;
    sampler_ready = rt_thread_startup(thread) == RT_EOK;
    if (!sampler_ready)
        rt_thread_delete(thread);
}

int watch_battery_percent(uint32_t now, bool *charging)
{
    if (sampler_ready && now - requested_at >= CHARGER_REFRESH_MS) {
        requested_at = now;
        rt_sem_release(&sample_requested);
    }
    *charging = charger_active;
    return percent;
}
