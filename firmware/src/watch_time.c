#include "watch_time.h"
#include "watch_rtc.h"
#include "protocol.h"

#include <rtdevice.h>
#include <finsh.h>

#define TIME_KEY "clock"
/* Earlier versions could leave a valid-looking estimate in the RTC. */
#define TIME_RECORD_VERSION 3u
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
static bool clock_available, trusted_checkpoint, retry_retained_rtc;
static bool flash_attempted, flash_dirty, ble_write_pending;
static bool using_rtc;
static const char *boot_source;
static uint32_t boot_epoch;
static bool boot_show_clock;
static struct watch_rtc_sample boot_read;

static bool valid_epoch(uint32_t epoch)
{
    return epoch >= WATCH_TIME_MIN && epoch < WATCH_TIME_LIMIT;
}

static uint32_t current_epoch(uint32_t now)
{
    return clock_epoch ? clock_epoch + (now - clock_ms) / 1000u : 0u;
}

static void set_clock(uint32_t epoch, uint32_t now)
{
    clock_epoch = epoch;
    clock_ms = now;
}

static bool write_rtc(uint32_t epoch)
{
    uint32_t readback = 0;
    if (!rtc)
        rtc = rt_device_find("rtc");
    return rtc && rt_device_control(rtc, RT_DEVICE_CTRL_RTC_SET_TIME, &epoch) == RT_EOK &&
           watch_rtc_valid(watch_rtc_read(), &readback) &&
           readback >= epoch && readback - epoch <= 2u;
}

static const char *persist_clock(uint32_t now)
{
    if (!flash_dirty)
        return "unchanged";
    if (!db)
        return "unavailable";
    if (ble_write_pending)
        return "pending-rtc";
    if (flash_attempted && now - last_flash_attempt < FLASH_MIN_INTERVAL_MS)
        return "deferred";

    struct time_record record = {TIME_RECORD_VERSION, current_epoch(now)};
    struct fdb_blob blob;
    if (rt_mutex_take(db_lock, 0) != RT_EOK)
        return "deferred";
    flash_attempted = true;
    last_flash_attempt = now;
    bool saved = fdb_kv_set_blob(db, TIME_KEY, fdb_blob_make(&blob, &record, sizeof(record))) == FDB_NO_ERR;
    rt_mutex_release(db_lock);
    if (saved) {
        saved_epoch = record.epoch;
        trusted_checkpoint = true;
        flash_dirty = false;
    }
    return saved ? "saved" : "error";
}

static void clock_status(void)
{
    char text[24] = "awaiting-rtc";
    struct tm local;
    time_t stamp = boot_epoch;
    if (boot_epoch && localtime_r(&stamp, &local))
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    watch_rtc_log_reset();
    rt_kprintf("WATCH_TIME_BOOT display=%s rtc_source=%s overwritten=0 time=%s epoch=%u tr=%08x dr=%08x\n",
               boot_show_clock ? "rtc" : "wait_ble", boot_source, text, boot_epoch, boot_read.tr, boot_read.dr);
}

/* Replay the actual boot snapshot after reconnecting USB on battery tests. */
static int watch_clock(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!boot_source)
        return 0;
    clock_status();
    struct watch_rtc_sample s = watch_rtc_read();
    uint32_t epoch = 0;
    bool valid = watch_rtc_valid(s, &epoch);
    rt_kprintf("WATCH_RTC_NOW valid=%d epoch=%u tr=%08x dr=%08x\n", valid, epoch, s.tr, s.dr);
    return 0;
}
MSH_CMD_EXPORT(watch_clock, Show reset evidence and current hardware RTC);

void watch_time_init(fdb_kvdb_t database, struct rt_mutex *store_lock, uint32_t now)
{
    boot_read = watch_rtc_read();
    uint32_t epoch = 0;
    const char *source = "unavailable";
    if (!watch_rtc_valid(boot_read, &epoch))
        boot_read = watch_rtc_read();
    if (watch_rtc_valid(boot_read, &epoch))
        source = "rtc";
    else if (watch_rtc_valid(watch_rtc_at_reset(), &epoch))
        source = "rtc_at_reset";
    db = database;
    db_lock = store_lock;
    rtc = rt_device_find("rtc");
    if (db) {
        struct time_record record = {0};
        struct fdb_blob blob;
        rt_mutex_take(db_lock, RT_WAITING_FOREVER);
        size_t length = fdb_kv_get_blob(db, TIME_KEY, fdb_blob_make(&blob, &record, sizeof(record)));
        rt_mutex_release(db_lock);
        if (length == sizeof(record) && record.version == TIME_RECORD_VERSION && valid_epoch(record.epoch)) {
            saved_epoch = record.epoch;
            trusted_checkpoint = true;
        }
    }
    const bool retained = trusted_checkpoint && watch_rtc_domain_retained();
    clock_available = retained && valid_epoch(epoch);
    retry_retained_rtc = retained && !clock_available;
    if (clock_available)
        set_clock(epoch, now);
    using_rtc = clock_available;
    last_rtc_read = now;
    boot_epoch = epoch;
    boot_source = source;
    boot_show_clock = clock_available;
    clock_status();
}

void watch_time_sync(uint32_t epoch, uint32_t now)
{
    if (!valid_epoch(epoch))
        return;

    uint32_t previous = clock_available ? current_epoch(now) : boot_epoch;
    uint32_t correction = previous > epoch ? previous - epoch : epoch - previous;
    uint32_t checkpoint_age = saved_epoch > epoch ? saved_epoch - epoch : epoch - saved_epoch;
    flash_dirty |= !trusted_checkpoint || !clock_available || correction >= SIGNIFICANT_CORRECTION_SECONDS ||
                   checkpoint_age >= FLASH_CHECKPOINT_SECONDS;
    clock_available = true;
    retry_retained_rtc = false;
    set_clock(epoch, now);
    ble_write_pending = !write_rtc(epoch);
    using_rtc = !ble_write_pending;
    last_rtc_read = now;
    const char *flash = persist_clock(now);
    rt_kprintf("WATCH_TIME_SYNC source=ble rtc=%s flash=%s epoch=%u overwritten=%s\n",
               ble_write_pending ? "retry" : "ok", flash, (unsigned)epoch,
               ble_write_pending ? "unconfirmed" : "1");
}

void watch_time_poll(uint32_t now)
{
    if ((!clock_available && !retry_retained_rtc) || now - last_rtc_read < RTC_READ_INTERVAL_MS)
        return;
    last_rtc_read = now;
    if (clock_epoch) {
        uint32_t elapsed = (now - clock_ms) / 1000u;
        clock_epoch += elapsed;
        clock_ms += elapsed * 1000u;
    }
    if (ble_write_pending) {
        ble_write_pending = !write_rtc(clock_epoch);
        using_rtc = !ble_write_pending;
        if (using_rtc)
            rt_kprintf("WATCH_TIME_RECOVER source=ble overwritten=1 epoch=%u\n", clock_epoch);
    } else {
        uint32_t epoch;
        if (watch_rtc_valid(watch_rtc_read(), &epoch)) {
            if (!using_rtc) {
                rt_kprintf("WATCH_TIME_RECOVER source=rtc overwritten=0 epoch=%u\n", epoch);
                using_rtc = true;
                clock_available = true;
                retry_retained_rtc = false;
            }
            set_clock(epoch, now);
        }
    }
    if (clock_available && clock_epoch >= saved_epoch && clock_epoch - saved_epoch >= FLASH_CHECKPOINT_SECONDS)
        flash_dirty = true;
    if (flash_dirty && db && !ble_write_pending &&
            (!flash_attempted || now - last_flash_attempt >= FLASH_MIN_INTERVAL_MS))
        rt_kprintf("WATCH_TIME_CHECKPOINT flash=%s\n", persist_clock(now));
}

bool watch_time_calendar(uint32_t now, struct tm *local)
{
    time_t stamp = current_epoch(now);
    return clock_available && localtime_r(&stamp, local) != NULL;
}

bool watch_time_ready(void)
{
    return clock_available;
}
