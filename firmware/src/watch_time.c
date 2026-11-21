#include "watch_time.h"
#include "protocol.h"

#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>

#define TIME_KEY "clock"
#define TIME_RECORD_VERSION 1u
#define RTC_READ_INTERVAL_MS 1000u
#define FLASH_MIN_INTERVAL_MS 60000u
#define FLASH_CHECKPOINT_SECONDS (6u * 60u * 60u)
#define SIGNIFICANT_CORRECTION_SECONDS 60u

struct time_record {
    uint32_t version;
    uint32_t epoch;
};

static fdb_kvdb_t db;
static struct rt_mutex *db_lock;
static rt_device_t rtc;
static uint32_t clock_epoch, clock_ms, last_rtc_read;
static uint32_t saved_epoch, last_flash_attempt;
static bool synced, flash_attempted, flash_dirty, rtc_needs_set;

static bool valid_epoch(uint32_t epoch)
{
    return epoch >= WATCH_TIME_MIN && epoch < WATCH_TIME_LIMIT;
}

static uint32_t current_epoch(uint32_t now)
{
    return clock_epoch + (now - clock_ms) / 1000u;
}

static void set_clock(uint32_t epoch, uint32_t now)
{
    clock_epoch = epoch;
    clock_ms = now;
}

static uint32_t build_epoch(void)
{
    const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    const char *time = __TIME__;
    struct tm local = {
        .tm_year = atoi(date + 7) - 1900,
        .tm_mday = atoi(date + 4),
        .tm_hour = atoi(time),
        .tm_min = atoi(time + 3),
        .tm_sec = atoi(time + 6),
        .tm_isdst = -1,
    };
    while (local.tm_mon < 12 && strncmp(months + 3 * local.tm_mon, date, 3) != 0)
        ++local.tm_mon;
    time_t stamp = mktime(&local);
    return stamp >= 0 && (uint64_t)stamp <= UINT32_MAX && valid_epoch((uint32_t)stamp)
           ? (uint32_t)stamp : WATCH_TIME_MIN;
}

static bool write_rtc(uint32_t epoch)
{
    uint32_t readback = 0;
    return rtc && rt_device_control(rtc, RT_DEVICE_CTRL_RTC_SET_TIME, &epoch) == RT_EOK &&
           rt_device_control(rtc, RT_DEVICE_CTRL_RTC_GET_TIME, &readback) == RT_EOK &&
           readback >= epoch && readback - epoch <= 2u;
}

static const char *persist_clock(uint32_t now)
{
    if (!flash_dirty)
        return "unchanged";
    if (!db)
        return "unavailable";
    if (flash_attempted && now - last_flash_attempt < FLASH_MIN_INTERVAL_MS)
        return "deferred";

    const struct time_record record = {TIME_RECORD_VERSION, current_epoch(now)};
    struct fdb_blob blob;
    flash_attempted = true;
    last_flash_attempt = now;
    if (rt_mutex_take(db_lock, 0) != RT_EOK)
        return "deferred";
    bool saved = fdb_kv_set_blob(db, TIME_KEY, fdb_blob_make(&blob, &record, sizeof(record))) == FDB_NO_ERR;
    rt_mutex_release(db_lock);
    if (saved) {
        saved_epoch = record.epoch;
        flash_dirty = false;
    }
    return saved ? "saved" : "error";
}

void watch_time_init(fdb_kvdb_t database, struct rt_mutex *store_lock, uint32_t now)
{
    db = database;
    db_lock = store_lock;
    rtc = rt_device_find("rtc");
    if (db) {
        struct time_record record = {0};
        struct fdb_blob blob;
        rt_mutex_take(db_lock, RT_WAITING_FOREVER);
        size_t length = fdb_kv_get_blob(db, TIME_KEY, fdb_blob_make(&blob, &record, sizeof(record)));
        rt_mutex_release(db_lock);
        if (length == sizeof(record) && record.version == TIME_RECORD_VERSION && valid_epoch(record.epoch))
            saved_epoch = record.epoch;
    }
    synced = saved_epoch != 0;

    uint32_t epoch = 0;
    const char *source = "rtc";
    if (!rtc || rt_device_control(rtc, RT_DEVICE_CTRL_RTC_GET_TIME, &epoch) != RT_EOK || !valid_epoch(epoch)) {
        source = saved_epoch ? "flash" : "build";
        epoch = saved_epoch ? saved_epoch : build_epoch();
        rtc_needs_set = !write_rtc(epoch);
    }
    set_clock(epoch, now);
    last_rtc_read = now;
    struct tm local;
    char text[20] = "";
    if (watch_time_calendar(now, &local))
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    rt_kprintf("WATCH_TIME_BOOT source=%s time=%s synced=%d rtc=%s\n",
               source, text, synced, rtc_needs_set ? "error" : "ok");
}

void watch_time_sync(uint32_t epoch, uint32_t now)
{
    if (!valid_epoch(epoch))
        return;

    uint32_t previous = current_epoch(now);
    uint32_t correction = previous > epoch ? previous - epoch : epoch - previous;
    uint32_t checkpoint_age = saved_epoch > epoch ? saved_epoch - epoch : epoch - saved_epoch;
    flash_dirty |= !saved_epoch || correction >= SIGNIFICANT_CORRECTION_SECONDS ||
                   checkpoint_age >= FLASH_CHECKPOINT_SECONDS;
    synced = true;
    set_clock(epoch, now);
    rtc_needs_set = !write_rtc(epoch);
    last_rtc_read = now;
    const char *flash = persist_clock(now);
    rt_kprintf("WATCH_TIME_SYNC source=ble rtc=%s flash=%s epoch=%u\n",
               rtc_needs_set ? "error" : "ok", flash, (unsigned)epoch);
}

void watch_time_poll(uint32_t now)
{
    if (now - last_rtc_read < RTC_READ_INTERVAL_MS)
        return;
    last_rtc_read = now;
    uint32_t elapsed = (now - clock_ms) / 1000u;
    clock_epoch += elapsed;
    clock_ms += elapsed * 1000u;
    if (rtc_needs_set) {
        if (!rtc)
            rtc = rt_device_find("rtc");
        rtc_needs_set = !write_rtc(clock_epoch);
    } else {
        uint32_t epoch;
        if (rtc && rt_device_control(rtc, RT_DEVICE_CTRL_RTC_GET_TIME, &epoch) == RT_EOK && valid_epoch(epoch))
            set_clock(epoch, now);
    }
    if (synced && clock_epoch >= saved_epoch && clock_epoch - saved_epoch >= FLASH_CHECKPOINT_SECONDS)
        flash_dirty = true;
    if (flash_dirty && db && (!flash_attempted || now - last_flash_attempt >= FLASH_MIN_INTERVAL_MS))
        rt_kprintf("WATCH_TIME_CHECKPOINT flash=%s\n", persist_clock(now));
}

bool watch_time_calendar(uint32_t now, struct tm *local)
{
    time_t stamp = current_epoch(now);
    return localtime_r(&stamp, local) != NULL;
}

bool watch_time_is_synced(void)
{
    return synced;
}
