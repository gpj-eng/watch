#ifndef WATCH_TIME_H
#define WATCH_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <rtthread.h>
#include <flashdb.h>

void watch_time_init(fdb_kvdb_t database, struct rt_mutex *store_lock, uint32_t now);
void watch_time_poll(uint32_t now);
void watch_time_sync(uint32_t epoch, uint32_t now);
bool watch_time_calendar(uint32_t now, struct tm *local);
bool watch_time_ready(void);

#endif
