#ifndef WATCH_BATTERY_H
#define WATCH_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

void watch_battery_init(void);
int watch_battery_percent(uint32_t now, bool *charging);

#endif
