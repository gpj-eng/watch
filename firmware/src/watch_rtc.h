#ifndef WATCH_RTC_H
#define WATCH_RTC_H

#include <stdbool.h>
#include <stdint.h>

#define WATCH_POWER_KEY_PIN 34

struct watch_rtc_sample {
    uint32_t tr, dr, isr;
};

struct watch_rtc_sample watch_rtc_read(void);
struct watch_rtc_sample watch_rtc_at_reset(void);
bool watch_rtc_valid(struct watch_rtc_sample sample, uint32_t *epoch);
bool watch_rtc_domain_retained(void);
void watch_rtc_log_reset(void);
void watch_rtc_hibernate(bool usb);

#endif
