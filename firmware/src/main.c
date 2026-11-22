#include "watch_board.h"
#include "watch_time.h"
#include "watch_rtc.h"
#include "watch_wrist.h"
#include "watch_wallpapers.h"
#include "watch_menu.h"
#include "watch_settings_audio.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <finsh.h>
#include <flashdb.h>
#include <fal.h>
#include <string.h>
#include <littlevgl2rtt.h>
#include <lvgl.h>
LV_FONT_DECLARE(watch_cjk);
LV_FONT_DECLARE(watch_dial_date);
LV_FONT_DECLARE(watch_dial_digits);
LV_FONT_DECLARE(watch_wallpaper_font);

#define KEY1 WATCH_POWER_KEY_PIN
#define KEY2 43
#define VBUS_DET 44
#define KEY1_DEBOUNCE_MS 30u
#define KEY1_SHORT_MS 500u
#define KEY1_PROGRESS_MS 1000u
#define KEY1_LONG_MS 3000u
#define KEY1_FULL_FRAME_MS 120u
#define KEY1_POWER_GUARD_MS 300u
#define NOTICE_MS 12000u
#define KEY2_EDGE_EVENT (1u << 0)
#define KEY2_DEBOUNCE_MS 30u
#define KEY2_MANUAL_HOLD_MS 1200u
#define WATCH_POLL_MS 4u
#define IMU_ACTIVITY_GAP_MS 100u
#define WATCH_PANEL_BRIGHTNESS 50u
#define MAX_PENDING 4
#define MAX_RESOLUTIONS 8
#define RESOLVED_NOTICE_MS 2000u
#define HOLD_DRAW_MS 25u
#define STORE_VERSION 0x48530201u
#define WALLPAPER_KEY "wallpaper_idx"
#define BRIGHTNESS_KEY "ui_brightness"
#define VOLUME_KEY "ui_volume"
#define MIN_BRIGHTNESS 10u
#define DEFAULT_VOLUME 60u
#define SWIPE_AXIS_PIXELS 12
#define SWIPE_PAGE_PIXELS 30

struct persisted {
    uint32_t version;
    uint64_t sequence;
    char contacts[WATCH_MAX_CONTACTS][WATCH_PHONE_SIZE];
    struct watch_sos pending[MAX_PENDING];
};

struct resolution {
    uint64_t original_id;
    uint32_t at_ms;
};

static struct persisted stored = {.version = STORE_VERSION};
static struct resolution resolutions[MAX_RESOLUTIONS];
static bool resolutions_ok, resolutions_dirty;
static uint32_t resolution_save_at;
static uint64_t active_sos_id;
static bool resolved_notice;
static uint32_t resolved_at;
static struct fdb_kvdb database;
static struct rt_mutex store_mutex;
static bool store_ok, database_ready;
static struct watch_alarm alarm = {.cfg = &alarm_cfg_default};
static struct watch_detector detector;
static struct rt_event key_wake;
static volatile uint32_t key_edge_ms;
static volatile uint8_t demo_requested;
static volatile bool pose_requested;
static volatile uint32_t samples_read;
static bool bench_alarm;
static bool imu_ok, manual_requested, cancel_requested;
static uint32_t sensor_seen;
enum watch_page { PAGE_NONE, PAGE_DIAL, PAGE_FUNCTION, PAGE_MENU, PAGE_SLEEP, PAGE_ALARM, PAGE_POWER_HOLD };
enum key1_power_state { KEY1_POWER_IDLE, KEY1_POWER_HOLDING, KEY1_POWER_CONFIRMED };
enum swipe_axis { SWIPE_UNDECIDED, SWIPE_HORIZONTAL, SWIPE_VERTICAL, SWIPE_CONTROL };
static enum key1_power_state key1_power;
static uint32_t key1_power_progress, key1_power_confirmed_at;
static bool key1_released;
static enum watch_page visible_page;
static lv_obj_t *screen, *wallpaper, *title, *reading, *caption, *action, *action_label;
static lv_obj_t *wallpaper_image, *wallpaper_button, *wallpaper_status;
static lv_obj_t *help_back;
static lv_obj_t *dial_date, *dial_date_shadow, *clock_row, *dial_digits[5], *digit_shadows[5];
static lv_obj_t *clock_placeholder;
static uint8_t wallpaper_idx;
static bool wallpaper_requested;
static uint8_t panel_brightness = WATCH_PANEL_BRIGHTNESS, saved_brightness = WATCH_PANEL_BRIGHTNESS;
static uint8_t normal_volume = DEFAULT_VOLUME, saved_volume = DEFAULT_VOLUME;
static enum watch_menu_page menu_page, requested_menu;
static enum watch_page requested_page;
static lv_indev_read_cb_t original_touch_read;
static lv_point_t touch_origin;
static enum watch_page touch_page;
static enum swipe_axis touch_axis;
static bool touch_down, touch_moved;
static int swipe_direction;
static lv_obj_t *resolve_arc, *resolve_percent, *resolve_text;
static rt_device_t lcd;
static const char *notice;
static uint32_t notice_started;

uint32_t watch_now_ms(void)
{
    /* Keep elapsed arithmetic valid even when the RTOS tick wraps. */
    return (uint32_t)((uint64_t)rt_tick_get() * 1000u / RT_TICK_PER_SECOND);
}

static void key2_edge(void *argument)
{
    (void)argument;
    key_edge_ms = watch_now_ms();
    rt_event_send(&key_wake, KEY2_EDGE_EVENT);
}

static bool save(const struct persisted *next)
{
    struct fdb_blob blob;
    return store_ok && fdb_kv_set_blob(&database, "state", fdb_blob_make(&blob, next, sizeof(*next))) == FDB_NO_ERR;
}

static void load_store(void)
{
    struct persisted loaded = {0};
    struct fdb_blob blob;
    rt_mutex_init(&store_mutex, "sosdb", RT_IPC_FLAG_PRIO);
    database_ready = fal_init() >= 0 &&
                     fdb_kvdb_init(&database, "watch", "watch_sos", NULL, NULL) == FDB_NO_ERR;
    store_ok = database_ready;
    if (store_ok) {
        size_t length = fdb_kv_get_blob(&database, "state", fdb_blob_make(&blob, &loaded, sizeof(loaded)));
        if (length == sizeof(loaded) && loaded.version == STORE_VERSION)
            stored = loaded;
        else if (length != 0)
            store_ok = false;
    }
    if (database_ready) {
        struct resolution loaded_resolutions[MAX_RESOLUTIONS] = {0};
        size_t length = fdb_kv_get_blob(&database, "resolved",
                                       fdb_blob_make(&blob, loaded_resolutions, sizeof(loaded_resolutions)));
        resolutions_ok = length == 0 || length == sizeof(loaded_resolutions);
        if (length == sizeof(loaded_resolutions))
            memcpy(resolutions, loaded_resolutions, sizeof(resolutions));
        if (!resolutions_ok)
            rt_kprintf("WATCH_RESOLVED storage=invalid\n");
    }
}

static bool read_ui_byte(const char *key, uint8_t *value)
{
    struct fdb_blob blob;
    return database_ready &&
           fdb_kv_get_blob(&database, key, fdb_blob_make(&blob, value, sizeof(*value))) == sizeof(*value) &&
           blob.saved.len == sizeof(*value);
}

static bool save_ui_byte(const char *key, uint8_t value)
{
    struct fdb_blob blob;
    rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
    bool saved = database_ready &&
                 fdb_kv_set_blob(&database, key, fdb_blob_make(&blob, &value, sizeof(value))) == FDB_NO_ERR;
    rt_mutex_release(&store_mutex);
    return saved;
}

static void load_ui_settings(void)
{
    uint8_t index = 0;
    if (read_ui_byte(WALLPAPER_KEY, &index) && index < g_wallpaper_count) {
        wallpaper_idx = index;
        rt_kprintf("WATCH_WALLPAPER load index=%u count=%u saved=1\n", (unsigned)index, g_wallpaper_count);
    } else {
        rt_kprintf("WATCH_WALLPAPER load index=0 count=%u saved=0\n", g_wallpaper_count);
    }
    uint8_t value;
    if (read_ui_byte(BRIGHTNESS_KEY, &value) && value >= MIN_BRIGHTNESS && value <= 100u)
        panel_brightness = saved_brightness = value;
    if (read_ui_byte(VOLUME_KEY, &value) && value <= 100u)
        normal_volume = saved_volume = value;
}

static void watch_contacts(int argc, char **argv)
{
    if (argc < 2 || argc > 1 + WATCH_MAX_CONTACTS) {
        rt_kprintf("watch_contacts +E164 [+E164 ...] | clear\n");
        return;
    }
    int count = argc == 2 && strcmp(argv[1], "clear") == 0 ? 0 : argc - 1;
    for (int i = 1; i <= count; ++i) {
        if (!watch_valid_phone(argv[i])) {
            rt_kprintf("Invalid E.164 number\n");
            return;
        }
        for (int j = 1; j < i; ++j)
            if (strcmp(argv[i], argv[j]) == 0) {
                rt_kprintf("Duplicate contact\n");
                return;
            }
    }
    rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
    struct persisted next = stored;
    memset(next.contacts, 0, sizeof(next.contacts));
    for (int i = 0; i < count; ++i)
        memcpy(next.contacts[i], argv[i + 1], strlen(argv[i + 1]) + 1);
    bool ok = save(&next);
    if (ok)
        stored = next;
    rt_mutex_release(&store_mutex);
    rt_kprintf("WATCH_CONTACTS %s count=%d\n", ok ? "saved" : "write-error", count);
}
MSH_CMD_EXPORT(watch_contacts, Save emergency contacts to watch flash);

static void watch_demo(void)
{
    demo_requested = 1;
}
MSH_CMD_EXPORT(watch_demo, Run local alarm demo without sending SOS);

static void watch_pose(void)
{
    pose_requested = true;
}
MSH_CMD_EXPORT(watch_pose, Print the next IMU sample and signed face-up acceleration);

static void watch_bench(void)
{
    demo_requested = 2;
}
MSH_CMD_EXPORT(watch_bench, Run an SOS exercise with reserved contacts only);

static void watch_status(void)
{
    rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
    unsigned contacts = 0, pending = 0;
    for (unsigned i = 0; i < WATCH_MAX_CONTACTS; ++i)
        contacts += stored.contacts[i][0] != 0;
    for (unsigned i = 0; i < MAX_PENDING; ++i)
        pending += stored.pending[i].id != 0;
    rt_mutex_release(&store_mutex);
    rt_kprintf("WATCH_STATUS imu=%d samples=%u flash=%d display=%d ble=%d contacts=%u pending=%u\n",
               imu_ok, (unsigned)samples_read, store_ok, title != NULL, watch_ble_ready(), contacts, pending);
}
MSH_CMD_EXPORT(watch_status, Show watch hardware and queue status);

static void watch_pair(void)
{
    watch_ble_pair();
    rt_kprintf("WATCH_PAIR open for 60 seconds; passkey appears on watch\n");
}
MSH_CMD_EXPORT(watch_pair, Open BLE pairing for 60 seconds);

static void show_notice(const char *message, uint32_t now)
{
    notice = message;
    notice_started = now;
}

static uint64_t submit(uint32_t now)
{
    if (alarm.demo)
        return 0;
    uint64_t committed = 0;
    rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
    struct persisted next = stored;
    unsigned slot = 0;
    while (slot < MAX_PENDING && next.pending[slot].id)
        ++slot;
    if (bench_alarm) {
        for (unsigned i = 0; i < WATCH_MAX_CONTACTS && next.contacts[i][0]; ++i) {
            const char *p = next.contacts[i];
            if (strlen(p) != 12 || memcmp(p, "+1202555010", 11) != 0 || p[11] < '0' || p[11] > '2') {
                rt_kprintf("WATCH_BENCH requires reserved test contacts\n");
                rt_mutex_release(&store_mutex);
                return 0;
            }
        }
    }
    if (!next.contacts[0][0]) {
        show_notice("请设置联系人", now);
    } else if (slot == MAX_PENDING) {
        show_notice("求助队列已满", now);
    } else {
        struct watch_sos *item = &next.pending[slot];
        *item = (struct watch_sos){.id = ++next.sequence, .reason = alarm.reason};
        for (unsigned i = 0; i < WATCH_MAX_CONTACTS && next.contacts[i][0]; ++i) {
            memcpy(item->phones[i], next.contacts[i], WATCH_PHONE_SIZE);
            ++item->count;
        }
        if (save(&next)) {
            stored = next;
            committed = item->id;
            show_notice("等待接收端", now);
            rt_kprintf("WATCH_SOS queued reason=%u contacts=%u id=%08x%08x\n", item->reason, item->count,
                       (unsigned)(committed >> 32), (unsigned)committed);
        } else {
            show_notice("闪存存储失败", now);
        }
    }
    rt_mutex_release(&store_mutex);
    return committed;
}

static void queue_resolution(uint64_t id, uint32_t now)
{
    if (!id)
        return;
    if (!resolutions_ok) {
        rt_kprintf("WATCH_RESOLVED storage=unavailable\n");
        return;
    }
    for (unsigned i = 0; i < MAX_RESOLUTIONS; ++i) {
        if (!resolutions[i].original_id) {
            resolutions[i] = (struct resolution){.original_id = id, .at_ms = now};
            resolutions_dirty = true;
            resolution_save_at = now;
            return;
        }
    }
    rt_kprintf("WATCH_RESOLVED storage=queue_full\n");
}

static bool save_resolutions(struct resolution entries[MAX_RESOLUTIONS])
{
    struct fdb_blob blob;
    return fdb_kv_set_blob(&database, "resolved",
                          fdb_blob_make(&blob, entries, sizeof(resolutions))) == FDB_NO_ERR;
}

#ifdef WATCH_SOS_RESOLVED_UPLINK
static void acknowledge_resolution(uint64_t id, uint8_t status)
{
    for (unsigned i = 0; i < MAX_RESOLUTIONS; ++i) {
        if (resolutions[i].original_id != id)
            continue;
        /* A durable Jetson ACK hands delivery ownership to the gateway. */
        struct resolution entry = resolutions[i];
        resolutions[i] = (struct resolution){0};
        if (save_resolutions(resolutions)) {
            resolutions_dirty = false;
            rt_kprintf("WATCH_RESOLVED_ACK id=%08x%08x status=%u\n",
                       (unsigned)(id >> 32), (unsigned)id, status);
        } else {
            resolutions[i] = entry;
            rt_kprintf("WATCH_RESOLVED storage=ack_write_failed\n");
        }
        return;
    }
}

static bool send_resolution(uint32_t now)
{
    static uint32_t retry;
    static bool connected;
    static uint64_t last_sent;
    static uint8_t device[WATCH_DEVICE_ID_SIZE];
    static bool have_device;

    if (!watch_ble_ready()) {
        connected = false;
        last_sent = 0;
        return false;
    }
    if (!connected) {
        connected = true;
        retry = now;
    }
    if (resolutions_dirty || (int32_t)(now - retry) < 0)
        return false;
    const struct resolution *first = NULL, *next = NULL;
    for (unsigned i = 0; i < MAX_RESOLUTIONS; ++i) {
        const struct resolution *item = &resolutions[i];
        if (!item->original_id)
            continue;
        if (!first || item->original_id < first->original_id)
            first = item;
        if (item->original_id > last_sent && (!next || item->original_id < next->original_id))
            next = item;
    }
    const struct resolution *item = next ? next : first;
    if (!item) {
        last_sent = 0;
        return false;
    }
    uint8_t frame[WATCH_FRAME_SIZE];
    have_device = have_device || watch_ble_device_id(device);
    if (!have_device || !watch_frame_resolved(frame, item->original_id, device, item->at_ms)) {
        retry = now + 2000u;
        rt_kprintf("WATCH_RESOLVED device_id=unavailable\n");
        return false;
    }
    if (watch_ble_send(frame)) {
        last_sent = item->original_id;
        retry = now + 1000u;
    } else {
        retry = now + 100u;
    }
    return true;
}
#endif

static void start_countdown(uint32_t now, bool demo)
{
    watch_alarm_fall(&alarm, now, demo);
    watch_voice_set(WATCH_VOICE_TONE);
    rt_kprintf("WATCH_ALARM_COUNTDOWN ms=%u phase=NORMAL total_ms=%u\n",
               (unsigned)now, (unsigned)alarm.cfg->cancel_timeout_ms);
}

static void handle_alarm_event(enum watch_alarm_event event, uint32_t now)
{
    switch (event) {
    case EVT_ALARM_URGENT:
        rt_kprintf("WATCH_ALARM_PHASE ms=%u from=NORMAL to=URGENT\n", (unsigned)now);
        break;
    case EVT_ALARM_CANCEL:
        watch_motor(false, alarm.cfg->vib_pwm_duty);
        watch_voice_set(WATCH_VOICE_OFF);
        show_notice("已取消", now);
        rt_kprintf("WATCH_CANCELLED ms=%u event=EVT_ALARM_CANCEL\n", (unsigned)now);
        bench_alarm = false;
        detector = (struct watch_detector){0};
        break;
    case EVT_SOS_TIMEOUT:
        watch_motor(watch_alarm_vibrate(&alarm, now), alarm.cfg->vib_pwm_duty);
        rt_kprintf("WATCH_SOS_TIMEOUT ms=%u event=EVT_SOS_TIMEOUT\n", (unsigned)now);
        watch_voice_set(WATCH_VOICE_HELP);
        active_sos_id = submit(now);
        bench_alarm = false;
        break;
    case EVT_SOS_RESOLVED:
        watch_motor(false, alarm.cfg->vib_pwm_duty);
        watch_voice_set(WATCH_VOICE_OFF);
        resolved_notice = true;
        resolved_at = now;
        detector = (struct watch_detector){0};
        queue_resolution(active_sos_id, now);
        rt_kprintf("WATCH_RESOLVED ms=%u id=%08x%08x\n", (unsigned)now,
                   (unsigned)(active_sos_id >> 32), (unsigned)active_sos_id);
        active_sos_id = 0;
        break;
    case EVT_SOS_MANUAL:
        bench_alarm = false;
        submit(now);
        detector = (struct watch_detector){0};
        break;
    default:
        break;
    }
}

static bool outbox(uint32_t now)
{
    static uint32_t retry;
    static uint8_t recipient;
    static uint64_t sending_id, last_completed;
    uint64_t id;
    uint8_t type, status;
    if (resolutions_dirty && (int32_t)(now - resolution_save_at) >= 0) {
        resolutions_dirty = !save_resolutions(resolutions);
        if (resolutions_dirty) {
            resolution_save_at = now + 2000u;
            rt_kprintf("WATCH_RESOLVED storage=write_failed\n");
        }
    }
    for (unsigned n = 0; n < 8 && watch_ble_ack(&type, &id, &status); ++n) {
        if (type == WATCH_RESOLVED_ACK_TYPE) {
#ifdef WATCH_SOS_RESOLVED_UPLINK
            acknowledge_resolution(id, status);
#endif
            continue;
        }
        for (unsigned i = 0; i < MAX_PENDING; ++i) {
            if (stored.pending[i].id != id)
                continue;
            if (status == 1) {
                show_notice("接收端已收", now);
                break;
            }
            struct persisted next = stored;
            memset(&next.pending[i], 0, sizeof(next.pending[i]));
            if (save(&next)) {
                stored = next;
                show_notice(status == 2 ? "短信服务已受理" : "短信发送失败", now);
            } else {
                show_notice("闪存存储失败", now);
            }
            break;
        }
    }
    if (!watch_ble_ready()) {
        sending_id = 0;
        last_completed = 0;
        retry = now;
        return false;
    }
    const struct watch_sos *head = NULL;
    const struct watch_sos *first = NULL;
    for (unsigned i = 0; i < MAX_PENDING; ++i) {
        const struct watch_sos *item = &stored.pending[i];
        if (!item->id)
            continue;
        if (!first || item->id < first->id)
            first = item;
        if (item->id > last_completed && (!head || item->id < head->id))
            head = item;
    }
    if (!first)
        return false;
    if (!head) {
        if ((int32_t)(now - retry) < 0)
            return false;
        last_completed = 0;
        head = first;
    }
    if (sending_id != head->id) {
        sending_id = head->id;
        recipient = 0;
        retry = now;
    }
    if ((int32_t)(now - retry) < 0)
        return false;
    uint8_t frame[20];
    if (watch_frame_sos(frame, head, recipient) && watch_ble_send(frame)) {
        if (++recipient == head->count) {
            recipient = 0;
            last_completed = head->id;
            retry = now + 8000u;
        } else {
            retry = now + 40u;
        }
    } else {
        retry = now + 100u;
    }
    return true;
}

static void action_pressed(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (visible_page == PAGE_ALARM && watch_alarm_counting(&alarm) && code == LV_EVENT_SHORT_CLICKED) {
        cancel_requested = true;
        rt_kprintf("WATCH_TOUCH cancel\n");
    }
    if (visible_page == PAGE_FUNCTION && alarm.phase == ALARM_STATE_IDLE && code == LV_EVENT_LONG_PRESSED &&
        (!touch_down || !touch_moved)) {
        manual_requested = true;
        rt_kprintf("WATCH_TOUCH manual\n");
    }
}

static lv_obj_t *label(lv_obj_t *parent, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_width(obj, 330);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    return obj;
}

static void show_wallpaper_selection(void)
{
    const char *name = g_wallpaper_names[wallpaper_idx];
    if (name && *name)
        lv_label_set_text_fmt(wallpaper_status, "%u/%u · %s", (unsigned)wallpaper_idx + 1, g_wallpaper_count, name);
    else
        lv_label_set_text_fmt(wallpaper_status, "%u/%u", (unsigned)wallpaper_idx + 1, g_wallpaper_count);
}

static bool apply_wallpaper(unsigned index)
{
    if (index >= g_wallpaper_count)
        return false;
    if (index == wallpaper_idx)
        return true;

    if (!save_ui_byte(WALLPAPER_KEY, (uint8_t)index)) {
        rt_kprintf("WATCH_WALLPAPER save_error index=%u\n", index);
        return false;
    }

    wallpaper_idx = (uint8_t)index;
    lv_img_set_src(wallpaper_image, g_wallpapers[index]);
    show_wallpaper_selection();
    rt_kprintf("WATCH_WALLPAPER apply index=%u count=%u\n", index, g_wallpaper_count);
    return true;
}

static void request_wallpaper(lv_event_t *event)
{
    (void)event;
    wallpaper_requested = visible_page == PAGE_FUNCTION;
}

static bool set_panel_brightness(uint8_t value)
{
    return lcd && rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &value) == RT_EOK;
}

static unsigned menu_get_level(enum watch_menu_page page)
{
    return page == MENU_BRIGHTNESS ? panel_brightness : normal_volume;
}

static bool menu_set_level(enum watch_menu_page page, unsigned percent, bool commit)
{
    const bool brightness = page == MENU_BRIGHTNESS;
    if (brightness && percent < MIN_BRIGHTNESS)
        percent = MIN_BRIGHTNESS;
    if (percent > 100u)
        percent = 100u;
    uint8_t *level = brightness ? &panel_brightness : &normal_volume;
    uint8_t *saved = brightness ? &saved_brightness : &saved_volume;
    if (*level != percent) {
        if (brightness && !set_panel_brightness((uint8_t)percent))
            return false;
        *level = (uint8_t)percent;
    }
    if (!commit || *level == *saved)
        return true;
    if (!save_ui_byte(brightness ? BRIGHTNESS_KEY : VOLUME_KEY, *level)) {
        if (!brightness || set_panel_brightness(*saved))
            *level = *saved;
        return false;
    }
    *saved = *level;
    rt_kprintf("WATCH_UI %s=%u\n", brightness ? "brightness" : "volume", (unsigned)*level);
    return true;
}

static bool menu_play_sample(void)
{
    return alarm.phase == ALARM_STATE_IDLE && watch_settings_play_sample(normal_volume);
}

static enum watch_menu_contact_result menu_add_contact(const char *phone)
{
    if (!watch_valid_phone(phone))
        return MENU_CONTACT_INVALID;

    rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
    enum watch_menu_contact_result result = MENU_CONTACT_FULL;
    struct persisted next = stored;
    unsigned free_slot = WATCH_MAX_CONTACTS;
    for (unsigned i = 0; i < WATCH_MAX_CONTACTS; ++i) {
        if (strcmp(next.contacts[i], phone) == 0) {
            result = MENU_CONTACT_DUPLICATE;
            break;
        }
        if (!next.contacts[i][0] && free_slot == WATCH_MAX_CONTACTS)
            free_slot = i;
    }
    if (result == MENU_CONTACT_FULL && free_slot < WATCH_MAX_CONTACTS) {
        memcpy(next.contacts[free_slot], phone, strlen(phone) + 1);
        if (save(&next)) {
            stored = next;
            result = MENU_CONTACT_SAVED;
        } else {
            result = MENU_CONTACT_SAVE_FAILED;
        }
    }
    rt_mutex_release(&store_mutex);
    return result;
}

static unsigned menu_wallpaper_count(void) { return g_wallpaper_count; }
static unsigned menu_wallpaper_index(void) { return wallpaper_idx; }
static const char *menu_wallpaper_name(unsigned index) { return g_wallpaper_names[index]; }
static const lv_img_dsc_t *menu_wallpaper_image(unsigned index) { return g_wallpapers[index]; }

static void navigate_menu(enum watch_menu_page page)
{
    requested_menu = page;
    requested_page = page == MENU_NONE ? PAGE_FUNCTION : PAGE_MENU;
}

static void go_back_from_help(lv_event_t *event)
{
    (void)event;
    requested_page = PAGE_DIAL;
}

static void finish_swipe(void)
{
    if (touch_page != visible_page || !swipe_direction)
        return;
    if (touch_page == PAGE_FUNCTION) {
        if (swipe_direction < 0)
            navigate_menu(MENU_FUNCTIONS);
        else
            requested_page = PAGE_DIAL;
    } else if (touch_page == PAGE_MENU && swipe_direction > 0) {
        navigate_menu(watch_menu_parent(menu_page));
    }
}

static void read_touch(lv_indev_t *input, lv_indev_data_t *data)
{
    original_touch_read(input, data);
    if (data->state == LV_INDEV_STATE_RELEASED) {
        if (touch_down)
            finish_swipe();
        touch_down = false;
        return;
    }
    if (!touch_down) {
        touch_down = true;
        touch_page = visible_page;
        touch_origin = data->point;
        swipe_direction = 0;
        touch_moved = false;
        touch_axis = SWIPE_UNDECIDED;
        if (touch_page == PAGE_MENU) {
            lv_point_t point = data->point;
            if (watch_menu_blocks_swipe(lv_indev_search_obj(screen, &point)))
                touch_axis = SWIPE_CONTROL;
        }
        return;
    }
    if (touch_page != visible_page || (touch_page != PAGE_FUNCTION && touch_page != PAGE_MENU) ||
        touch_axis == SWIPE_CONTROL || swipe_direction)
        return;

    int delta_x = data->point.x - touch_origin.x;
    int delta_y = data->point.y - touch_origin.y;
    int distance_x = LV_ABS(delta_x);
    int distance_y = LV_ABS(delta_y);
    if (distance_x >= SWIPE_AXIS_PIXELS || distance_y >= SWIPE_AXIS_PIXELS)
        touch_moved = true;
    if (touch_axis == SWIPE_UNDECIDED) {
        if (distance_x >= SWIPE_AXIS_PIXELS && distance_x * 4 >= distance_y * 5)
            touch_axis = SWIPE_HORIZONTAL;
        else if (distance_y >= SWIPE_AXIS_PIXELS && distance_y * 4 >= distance_x * 5)
            touch_axis = SWIPE_VERTICAL;
    }
    if (touch_axis == SWIPE_HORIZONTAL && distance_x >= SWIPE_PAGE_PIXELS) {
        swipe_direction = delta_x > 0 ? 1 : -1;
        lv_indev_wait_release(input);
    }
}

static void suppress_drag_click(lv_event_t *event)
{
    if (touch_page == visible_page && (touch_page == PAGE_FUNCTION || touch_page == PAGE_MENU) &&
        touch_moved && touch_axis != SWIPE_CONTROL)
        lv_indev_stop_processing(lv_event_get_current_target(event));
}

static lv_obj_t *dial_shade(int top, int height)
{
    lv_obj_t *shade = lv_obj_create(wallpaper);
    lv_obj_remove_style_all(shade);
    lv_obj_remove_flag(shade, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(shade, lv_pct(100), height);
    lv_obj_align(shade, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
    return shade;
}

static void init_ui(void)
{
    if (littlevgl2rtt_init("lcd") != RT_EOK)
        return;
    lcd = rt_device_find("lcd");
    if (panel_brightness != WATCH_PANEL_BRIGHTNESS && !set_panel_brightness(panel_brightness))
        rt_kprintf("WATCH_DISPLAY brightness=%u error\n", (unsigned)panel_brightness);
    screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    wallpaper = lv_obj_create(screen);
    lv_obj_remove_style_all(wallpaper);
    lv_obj_set_size(wallpaper, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(wallpaper, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    wallpaper_image = lv_img_create(wallpaper);
    lv_img_set_src(wallpaper_image, g_wallpapers[wallpaper_idx]);
    lv_obj_center(wallpaper_image);
    lv_obj_set_style_bg_opa(dial_shade(0, 205), LV_OPA_60, 0);
    lv_obj_t *fade = dial_shade(205, 78);
    lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(fade, LV_OPA_60, 0);
    lv_obj_set_style_bg_grad_opa(fade, LV_OPA_TRANSP, 0);
    dial_date_shadow = label(wallpaper, 55, &watch_dial_date, 0x000000);
    lv_obj_set_style_text_opa(dial_date_shadow, LV_OPA_60, 0);
    dial_date = label(wallpaper, 53, &watch_dial_date, 0xffffff);
    clock_row = lv_obj_create(wallpaper);
    lv_obj_add_flag(clock_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dial_date, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dial_date_shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_style_all(clock_row);
    lv_obj_remove_flag(clock_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(clock_row, 368, 118);
    lv_obj_align(clock_row, LV_ALIGN_TOP_MID, 0, 91);
    int x = 0;
    for (unsigned i = 0; i < 5; ++i) {
        const int width = i == 2 ? 32 : 83;
        digit_shadows[i] = label(clock_row, 0, &watch_dial_digits, 0x000000);
        lv_obj_set_style_text_opa(digit_shadows[i], LV_OPA_60, 0);
        lv_obj_set_width(digit_shadows[i], width);
        lv_obj_align(digit_shadows[i], LV_ALIGN_LEFT_MID, x + 2, 3);
        dial_digits[i] = label(clock_row, 0, &watch_dial_digits, 0xffffff);
        lv_obj_set_width(dial_digits[i], width);
        lv_obj_align(dial_digits[i], LV_ALIGN_LEFT_MID, x, 0);
        x += width;
    }
    const lv_font_t *font = &watch_cjk;
    title = label(screen, 63, font, 0xf3f8f4);
    reading = label(screen, 126, &lv_font_montserrat_48, 0xffffff);
    caption = label(screen, 212, font, 0xc7d6d0);
    action = lv_button_create(screen);
    lv_obj_set_size(action, 300, 82);
    lv_obj_align(action, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_set_style_radius(action, 20, 0);
    lv_obj_add_event_cb(action, action_pressed, LV_EVENT_ALL, NULL);
    action_label = label(action, 0, font, 0xffffff);
    lv_obj_set_width(action_label, 270);
    lv_obj_center(action_label);
    clock_placeholder = label(wallpaper, 123, &lv_font_montserrat_48, 0xffffff);
    lv_label_set_text(clock_placeholder, "--:--");
    wallpaper_button = lv_button_create(screen);
    lv_obj_set_size(wallpaper_button, 340, 56);
    lv_obj_align(wallpaper_button, LV_ALIGN_TOP_MID, 0, 383);
    lv_obj_set_style_radius(wallpaper_button, 18, 0);
    lv_obj_set_style_bg_color(wallpaper_button, lv_color_hex(0x263e5a), 0);
    lv_obj_add_event_cb(wallpaper_button, request_wallpaper, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *wallpaper_label = label(wallpaper_button, 0, &watch_wallpaper_font, 0xffffff);
    lv_obj_set_width(wallpaper_label, LV_SIZE_CONTENT);
    lv_obj_align(wallpaper_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(wallpaper_label, "切换壁纸");
    wallpaper_status = label(wallpaper_button, 0, &watch_wallpaper_font, 0xe0ebff);
    lv_obj_set_width(wallpaper_status, 180);
    lv_obj_set_style_text_align(wallpaper_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(wallpaper_status, LV_LABEL_LONG_DOT);
    lv_obj_align(wallpaper_status, LV_ALIGN_RIGHT_MID, 0, 0);
    show_wallpaper_selection();
    lv_obj_add_flag(wallpaper_button, LV_OBJ_FLAG_HIDDEN);
    resolve_arc = lv_arc_create(screen);
    lv_obj_set_size(resolve_arc, 210, 210);
    lv_obj_center(resolve_arc);
    lv_arc_set_rotation(resolve_arc, 270);
    lv_arc_set_bg_angles(resolve_arc, 0, 360);
    lv_arc_set_range(resolve_arc, 0, 1000);
    lv_arc_set_value(resolve_arc, 0);
    lv_obj_set_style_arc_width(resolve_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(resolve_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(resolve_arc, lv_color_hex(0x695356), LV_PART_MAIN);
    lv_obj_set_style_arc_color(resolve_arc, lv_color_hex(0xffcb91), LV_PART_INDICATOR);
    lv_obj_remove_style(resolve_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(resolve_arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(resolve_arc, LV_OBJ_FLAG_HIDDEN);
    resolve_percent = label(resolve_arc, 0, &lv_font_montserrat_48, 0xffffff);
    lv_obj_set_width(resolve_percent, 160);
    lv_obj_center(resolve_percent);
    resolve_text = label(screen, 355, font, 0xffffff);
    lv_obj_add_flag(resolve_text, LV_OBJ_FLAG_HIDDEN);
    help_back = lv_button_create(screen);
    lv_obj_set_size(help_back, 64, 48);
    lv_obj_align(help_back, LV_ALIGN_TOP_LEFT, 22, 11);
    lv_obj_set_style_bg_color(help_back, lv_color_hex(0x253332), 0);
    lv_obj_set_style_radius(help_back, 15, 0);
    lv_obj_add_event_cb(help_back, go_back_from_help, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(help_back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_center(back_icon);
    lv_obj_add_flag(help_back, LV_OBJ_FLAG_HIDDEN);
    static const struct watch_menu_ops operations = {
        .navigate = navigate_menu,
        .add_contact = menu_add_contact,
        .get_level = menu_get_level,
        .set_level = menu_set_level,
        .play_sample = menu_play_sample,
        .wallpaper_count = menu_wallpaper_count,
        .get_wallpaper = menu_wallpaper_index,
        .wallpaper_name = menu_wallpaper_name,
        .wallpaper_thumbnail = menu_wallpaper_image,
        .apply_wallpaper = apply_wallpaper,
    };
    watch_menu_init(screen, &operations);
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) {
            original_touch_read = lv_indev_get_read_cb(input);
            lv_indev_set_read_cb(input, read_touch);
            lv_indev_add_event_cb(input, suppress_drag_click, LV_EVENT_SHORT_CLICKED, NULL);
            lv_indev_add_event_cb(input, suppress_drag_click, LV_EVENT_CLICKED, NULL);
            break;
        }
    }
}

static void update_label(lv_obj_t *object, const char *text)
{
    if (strcmp(lv_label_get_text(object), text) != 0)
        lv_label_set_text(object, text);
}

static void set_visible(lv_obj_t *object, bool visible)
{
    if (visible && lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else if (!visible && !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static bool update_key1(enum watch_page *page, bool alarm_active, uint32_t now)
{
    static bool initialized, raw, stable, consumed, vbus;
    static bool woke_screen;
    static uint32_t changed, down_since, power_changed;
    const bool pressed = rt_pin_read(KEY1) == PIN_HIGH;
    const bool usb = rt_pin_read(VBUS_DET) == PIN_HIGH;
    if (!initialized) {
        initialized = true;
        raw = stable = consumed = pressed;
        vbus = usb;
        changed = down_since = power_changed = now;
        rt_kprintf("WATCH_POWER vbus=%d battery=%s\n", usb, usb ? "unknown" : "only");
        return false;
    }

    const bool power_changed_now = usb != vbus;
    if (power_changed_now) {
        vbus = usb;
        power_changed = now;
        rt_kprintf("WATCH_POWER vbus=%d battery=%s\n", usb, usb ? "unknown" : "only");
    }
    if (raw != pressed) {
        raw = pressed;
        changed = now;
    }
    if (alarm_active || power_changed_now) {
        if (key1_power != KEY1_POWER_IDLE || (!consumed && (stable || raw)))
            rt_kprintf("WATCH_KEY1 power=cancel reason=%s\n", alarm_active ? "alarm" : "supply");
        key1_power = KEY1_POWER_IDLE;
        consumed |= raw || stable;
    }
    bool activity = false;
    if (stable != raw && now - changed >= KEY1_DEBOUNCE_MS) {
        stable = raw;
        if (stable) {
            down_since = changed;
            consumed |= now - power_changed < KEY1_POWER_GUARD_MS;
            activity = !consumed;
            woke_screen = !consumed && *page == PAGE_SLEEP;
            if (woke_screen)
                *page = PAGE_DIAL;
        } else {
            const uint32_t held = changed - down_since;
            if (!consumed && held >= KEY1_LONG_MS) {
                key1_power = KEY1_POWER_CONFIRMED;
                key1_power_progress = 1000u;
                key1_power_confirmed_at = now;
                rt_kprintf("WATCH_KEY1 power=confirmed ms=%u\n", (unsigned)held);
            } else if (key1_power == KEY1_POWER_HOLDING) {
                key1_power = KEY1_POWER_IDLE;
                activity = true;
                rt_kprintf("WATCH_KEY1 power=cancel reason=release ms=%u\n", (unsigned)held);
            } else if (key1_power == KEY1_POWER_IDLE && !consumed && !woke_screen && held < KEY1_SHORT_MS) {
                *page = *page == PAGE_DIAL ? PAGE_FUNCTION : PAGE_DIAL;
                rt_kprintf("WATCH_KEY1 short ms=%u\n", (unsigned)held);
            }
            consumed = false;
        }
    } else if (!stable && !raw && now - changed >= KEY1_DEBOUNCE_MS) {
        consumed = false;
    }
    if (stable && raw && !consumed && key1_power != KEY1_POWER_CONFIRMED) {
        const uint32_t held = now - down_since;
        if (held >= KEY1_LONG_MS) {
            key1_power = KEY1_POWER_CONFIRMED;
            key1_power_progress = 1000u;
            key1_power_confirmed_at = now;
            consumed = true;
            rt_kprintf("WATCH_KEY1 power=confirmed ms=%u\n", (unsigned)held);
        } else if (held >= KEY1_PROGRESS_MS) {
            if (key1_power == KEY1_POWER_IDLE)
                rt_kprintf("WATCH_KEY1 power=hold ms=%u\n", (unsigned)held);
            key1_power = KEY1_POWER_HOLDING;
            key1_power_progress = (held - KEY1_PROGRESS_MS) * 1000u / (KEY1_LONG_MS - KEY1_PROGRESS_MS);
        }
    }
    key1_released = !raw && !stable;
    return activity || key1_power != KEY1_POWER_IDLE;
}

static void draw_hold_progress(uint32_t progress, const char *message)
{
    char value[8];
    lv_arc_set_value(resolve_arc, (int32_t)progress);
    rt_snprintf(value, sizeof(value), "%u%%", (unsigned)(progress / 10u));
    update_label(resolve_percent, value);
    update_label(resolve_text, message);
}

static void render(uint32_t now, enum watch_page next, unsigned pin, const char *power_reason)
{
    if (!title)
        return;
    const bool alarm_page = next == PAGE_ALARM;
    const bool power_page = next == PAGE_POWER_HOLD;
    const bool counting = alarm_page && watch_alarm_counting(&alarm);
    watch_menu_show(next == PAGE_MENU ? menu_page : MENU_NONE);
    if (visible_page != next) {
        static const char *const names[] = {
            [PAGE_DIAL] = "dial", [PAGE_FUNCTION] = "function", [PAGE_MENU] = "menu",
            [PAGE_SLEEP] = "sleep", [PAGE_ALARM] = "alarm", [PAGE_POWER_HOLD] = "power_hold",
        };
        if (visible_page == PAGE_SLEEP || next == PAGE_SLEEP) {
            /* Board-level LCD power-off also cuts the touchscreen's shared 3.3 V rail. */
            uint8_t brightness = next == PAGE_SLEEP ? 0 : panel_brightness;
            if (!set_panel_brightness(brightness)) {
                rt_kprintf("WATCH_DISPLAY brightness=%u error\n", (unsigned)brightness);
                return;
            }
            if (next == PAGE_SLEEP) {
                lv_display_enable_invalidation(NULL, false);
                lv_timer_pause(lv_display_get_refr_timer(NULL));
                set_visible(action, false);
                set_visible(wallpaper_button, false);
                set_visible(help_back, false);
            } else {
                lv_display_enable_invalidation(NULL, true);
                lv_obj_invalidate(screen);
            }
            rt_kprintf("WATCH_DISPLAY %s reason=%s\n", next == PAGE_SLEEP ? "sleep" : "wake", power_reason);
        }
        if (alarm_page) {
            /* A touch that began before the alarm must be released before it can cancel it. */
            lv_indev_wait_release(lv_indev_get_next(NULL));
        }
        visible_page = next;
        rt_kprintf("WATCH_PAGE %s\n", names[next]);
        if (next == PAGE_SLEEP)
            return;
        lv_obj_set_style_bg_color(screen, lv_color_hex(alarm_page ? 0x381b1c : 0x111919), 0);
        lv_obj_set_style_text_color(reading, lv_color_hex(alarm_page ? 0xffcb91 : 0xffffff), 0);
        lv_obj_set_style_bg_color(action, lv_color_hex(alarm_page ? 0x286354 : 0xa73732), 0);
        set_visible(wallpaper, next == PAGE_DIAL);
        set_visible(wallpaper_button, next == PAGE_FUNCTION);
        set_visible(help_back, next == PAGE_FUNCTION);
        if (next == PAGE_FUNCTION)
            show_wallpaper_selection();
        set_visible(title, next == PAGE_FUNCTION || alarm_page);
        set_visible(reading, next == PAGE_FUNCTION || alarm_page);
        set_visible(caption, next == PAGE_FUNCTION || alarm_page);
    } else if (next == PAGE_SLEEP) {
        return;
    }

    const bool showing_hold = alarm_page && (alarm.holding || resolved_notice);
    set_visible(resolve_arc, showing_hold || power_page);
    set_visible(resolve_text, showing_hold || power_page);
    if (alarm_page) {
        set_visible(reading, !showing_hold);
        set_visible(caption, !showing_hold);
    }
    if (!showing_hold && !power_page)
        lv_arc_set_value(resolve_arc, 0);
    set_visible(action, next == PAGE_FUNCTION || counting);
    char value[8];
    const bool notifying = notice && now - notice_started < NOTICE_MS;
    if (power_page) {
        draw_hold_progress(key1_power_progress, "长按关机");
    } else if (showing_hold) {
        uint32_t progress = resolved_notice ? 1000u :
            (uint32_t)((uint64_t)(now - alarm.hold_started_ms) * 1000u / alarm.cfg->cancel_hold_ms);
        if (progress > 1000u)
            progress = 1000u;
        draw_hold_progress(progress, resolved_notice ? "已解除警报" : "长按解除警报");
        update_label(title, alarm.demo ? "演示提醒" : "求助");
    } else if (counting) {
        rt_snprintf(value, sizeof(value), "%02u", watch_alarm_seconds(&alarm, now));
        update_label(title, alarm.demo || bench_alarm ? "演示提醒" : "疑似跌倒");
        update_label(reading, value);
        update_label(caption, alarm.demo ? "演示不会发送短信" : "短按取消");
        update_label(action_label, "我没事");
    } else if (alarm_page) {
        update_label(title, alarm.demo ? "演示提醒" : "求助");
        update_label(reading, "SOS");
        update_label(caption, alarm.phase == ALARM_STATE_SOS_ACTIVE ? "长按解除警报" :
                             alarm.demo ? "演示不会发送短信" : notifying ? notice : "");
    } else if (next == PAGE_FUNCTION) {
        update_label(action_label, "长按求助");
        if (pin < 1000000u) {
            rt_snprintf(value, sizeof(value), "%06u", pin);
            update_label(title, "蓝牙配对");
            update_label(reading, value);
            update_label(caption, "请在接收端输入");
        } else {
            update_label(title, "安全手表");
            update_label(reading, "");
            update_label(caption, !imu_ok || now - sensor_seen > 500u ? "传感器异常" :
                                  notifying ? notice : "");
        }
    } else if (next == PAGE_MENU) {
        struct tm local;
        char time[6] = "--:--";
        if (watch_time_calendar(now, &local))
            rt_snprintf(time, sizeof(time), "%02d:%02d", local.tm_hour, local.tm_min);
        watch_menu_set_time(time);
    } else {
        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        struct tm local;
        char date[32], time[6];
        bool show_time = watch_time_calendar(now, &local);
        set_visible(clock_placeholder, !show_time);
        set_visible(dial_date, show_time);
        set_visible(dial_date_shadow, show_time);
        set_visible(clock_row, show_time);
        if (show_time) {
            rt_snprintf(date, sizeof(date), "%d月%d日 周%s", local.tm_mon + 1,
                        local.tm_mday, weekdays[local.tm_wday]);
            rt_snprintf(time, sizeof(time), "%02d:%02d", local.tm_hour, local.tm_min);
            update_label(dial_date, date);
            update_label(dial_date_shadow, date);
            for (unsigned i = 0; i < 5; ++i) {
                const char digit[] = {time[i], '\0'};
                update_label(dial_digits[i], digit);
                update_label(digit_shadows[i], digit);
            }
        }
    }
}

int main(void)
{
    load_store();
    load_ui_settings();
    watch_time_init(database_ready ? &database : NULL, &store_mutex, watch_now_ms());
    rt_pin_mode(KEY1, PIN_MODE_INPUT_PULLDOWN);
    rt_pin_mode(VBUS_DET, PIN_MODE_INPUT);
    rt_pin_mode(KEY2, PIN_MODE_INPUT_PULLDOWN);
    rt_event_init(&key_wake, "key2", RT_IPC_FLAG_PRIO);
    key_edge_ms = watch_now_ms();
    bool key_irq_ready = rt_pin_attach_irq(KEY2, PIN_IRQ_MODE_RISING_FALLING, key2_edge, NULL) == RT_EOK &&
                         rt_pin_irq_enable(KEY2, PIN_IRQ_ENABLE) == RT_EOK;
    if (!key_irq_ready)
        rt_kprintf("WATCH_KEY2 ms=%u irq_unavailable polling\n", (unsigned)watch_now_ms());
    imu_ok = watch_imu_init() == 0;
    watch_voice_init(alarm.cfg);
    watch_ble_init();
    init_ui();
    rt_kprintf("WATCH_READY imu=%d storage=%d display=%d\n", imu_ok, store_ok, title != NULL);
    uint32_t last_draw = 0, down_since = 0;
    uint16_t seq = 0;
    bool key_raw = false, key_stable = false, handled = false;
    uint32_t key_changed = 0;
    enum watch_page page = PAGE_DIAL;
    bool manual_page = false;
    uint32_t manual_since = 0;
    unsigned last_pin = 1000000u;
    struct watch_wrist wrist = {0};
    wrist_sleep_cfg_t wrist_cfg = wrist_defaults;
    wrist_console_init();
    uint32_t last_activity = watch_now_ms();
    const char *power_reason = "startup";
    while (true) {
        watch_ble_poll();
        rt_base_t level = rt_hw_interrupt_disable();
        bool pressed = rt_pin_read(KEY2) == PIN_HIGH;
        uint32_t edge_ms = key_edge_ms;
        rt_hw_interrupt_enable(level);
        uint32_t now = watch_now_ms();

        enum watch_alarm_event voice_event = watch_voice_take_event();
        if (voice_event != EVT_ALARM_NONE)
            rt_kprintf("WATCH_VOICE_EXIT ms=%u event=%s\n", (unsigned)now,
                       voice_event == EVT_SOS_VOICE_DONE ? "audio_limit" : "audio_failed");
        if (resolved_notice && now - resolved_at >= RESOLVED_NOTICE_MS)
            resolved_notice = false;

        if (pressed != key_raw || (key_irq_ready && edge_ms != key_changed)) {
            key_raw = pressed;
            key_changed = key_irq_ready ? edge_ms : now;
        }
        bool key2_activity = false;
        if (key_stable != key_raw && now - key_changed >= KEY2_DEBOUNCE_MS) {
            key_stable = key_raw;
            if (key_stable) {
                key2_activity = true;
                down_since = key_changed + KEY2_DEBOUNCE_MS;
                bool counting = watch_alarm_counting(&alarm);
                handled = counting || alarm.phase == ALARM_STATE_SOS_ACTIVE || resolved_notice;
                cancel_requested |= counting;
            }
        }
        bool was_holding = alarm.holding;
        enum watch_alarm_event hold_event = watch_alarm_hold(&alarm, key_raw && key_stable, key2_activity, now);
        if (was_holding != alarm.holding)
            rt_kprintf("WATCH_KEY2_HOLD ms=%u state=%s\n", (unsigned)now,
                       alarm.holding ? "start" : hold_event == EVT_SOS_RESOLVED ? "complete" : "reset");
        handle_alarm_event(hold_event, now);
        if (key_raw && key_stable && !handled && !resolved_notice && alarm.phase == ALARM_STATE_IDLE &&
                now - down_since >= KEY2_MANUAL_HOLD_MS) {
            handled = true;
            manual_requested = true;
        }
        if (demo_requested) {
            uint8_t mode = demo_requested;
            demo_requested = 0;
            if (alarm.phase == ALARM_STATE_IDLE && !resolved_notice) {
                bench_alarm = mode == 2;
                start_countdown(now, mode == 1);
                rt_kprintf("WATCH_DEMO_START mode=%u\n", mode);
            }
        }
        if (manual_requested) {
            manual_requested = false;
            enum watch_alarm_event manual = watch_alarm_manual(&alarm);
            handle_alarm_event(manual, now);
            if (manual == EVT_SOS_MANUAL) {
                manual_page = true;
                manual_since = now;
            }
        }
        enum watch_alarm_event alarm_event = watch_alarm_update(&alarm, now, cancel_requested);
        cancel_requested = false;
        handle_alarm_event(alarm_event, now);

        struct watch_imu sample;
        bool sample_ready = imu_ok && watch_imu_read(&sample);
        const bool recent_sample = samples_read != 0 && now - sensor_seen <= IMU_ACTIVITY_GAP_MS;
        if (sample_ready) {
            ++samples_read;
            sensor_seen = now;
            if (pose_requested) {
                pose_requested = false;
                rt_kprintf("WATCH_POSE ax=%d ay=%d az=%d face_up=%d mg\n",
                           sample.ax, sample.ay, sample.az, (int)watch_wrist_face_up_mg(&sample));
            }
            if (alarm.phase == ALARM_STATE_IDLE && !resolved_notice && watch_detect(&detector, &sample, now)) {
                start_countdown(now, false);
                rt_kprintf("WATCH_FALL candidate\n");
            }
        }
        if (key_stable && watch_alarm_counting(&alarm))
            handled = true;
        watch_motor(watch_alarm_vibrate(&alarm, now), alarm.cfg->vib_pwm_duty);
        uint32_t epoch;
        bool time_changed = watch_ble_time(&epoch);
        const bool first_time_sync = time_changed && !watch_time_ready();
        if (time_changed)
            watch_time_sync(epoch, now);
        watch_time_poll(now);

        if (manual_page && now - manual_since >= NOTICE_MS)
            manual_page = false;
        const bool alarm_active = alarm.phase != ALARM_STATE_IDLE || manual_page || resolved_notice;
        if (!alarm_active && visible_page == PAGE_ALARM) {
            page = PAGE_DIAL;
            last_activity = now;
        }
        const enum watch_page from = page;
        const bool key1_activity = update_key1(&page, alarm_active || visible_page == PAGE_ALARM, now);
        if (requested_page != PAGE_NONE) {
            if (!alarm_active && page == from && page == visible_page &&
                (page == PAGE_FUNCTION || page == PAGE_MENU) && key1_power == KEY1_POWER_IDLE) {
                page = requested_page;
                if (page == PAGE_MENU) {
                    menu_page = requested_menu;
                    rt_kprintf("WATCH_MENU page=%u\n", (unsigned)menu_page);
                }
                last_activity = now;
                last_draw = now - 150u;
            }
            requested_page = PAGE_NONE;
        }
        const bool wrist_protected = alarm_active || visible_page == PAGE_ALARM;
        enum wrist_event wrist_event = RW_EVT_NONE;
        if (sample_ready) {
            if (wrist_console_take_cfg(&wrist_cfg))
                wrist = (struct watch_wrist){0};
            enum wrist_screen mode = wrist_protected ? RW_SCREEN_PROTECTED :
                                     page == PAGE_SLEEP ? RW_SCREEN_OFF : RW_SCREEN_ON;
            wrist_event = watch_wrist_update(&wrist, &wrist_cfg, &sample, now, mode);
            wrist_console_record(&sample, &wrist, wrist_event, now);
        }
        if (alarm_active) {
            last_activity = now;
            power_reason = "alarm";
        } else if (from == PAGE_SLEEP && page == PAGE_DIAL) {
            power_reason = "key1";
            last_activity = now;
        } else if (page == PAGE_SLEEP && key2_activity) {
            page = PAGE_DIAL;
            power_reason = "key2";
            last_activity = now;
        } else if (from != PAGE_SLEEP && page == PAGE_SLEEP) {
            power_reason = "key1";
        } else if (!wrist_protected && !key1_activity && !key2_activity && wrist_event == RW_EVT_SLEEP_SCREEN) {
            page = PAGE_SLEEP;
            power_reason = "wrist_down";
        }

        if (page != PAGE_SLEEP) {
            if (key1_activity || key2_activity || (sample_ready && (!recent_sample || !watch_wrist_still(&sample))))
                last_activity = now;
        }
        unsigned pin = watch_ble_pin();
        if (pin != last_pin && pin < 1000000u && !alarm_active && page != PAGE_SLEEP) {
            page = PAGE_FUNCTION;
            last_activity = now;
        }
        last_pin = pin;
        if (first_time_sync && !wrist_protected) {
            page = PAGE_DIAL;
            last_activity = now;
            power_reason = "ble_time";
            rt_kprintf("WATCH_TIME_DISPLAY ready\n");
        }
        if (title && page != PAGE_SLEEP && !alarm_active && sample_ready && now - last_activity >= WATCH_SCREEN_TIMEOUT_MS &&
                lv_display_get_inactive_time(NULL) >= WATCH_SCREEN_TIMEOUT_MS) {
            page = PAGE_SLEEP;
            power_reason = "idle";
        }
        const bool power_blank = key1_power == KEY1_POWER_CONFIRMED &&
                                 now - key1_power_confirmed_at >= KEY1_FULL_FRAME_MS;
        enum watch_page next = alarm_active ? PAGE_ALARM : page;
        if (!alarm_active && key1_power != KEY1_POWER_IDLE) {
            next = power_blank ? PAGE_SLEEP : PAGE_POWER_HOLD;
            power_reason = "key1_power";
        }
        if (time_changed || next != visible_page || was_holding != alarm.holding ||
                now - last_draw >= (alarm.holding || key1_power != KEY1_POWER_IDLE ? HOLD_DRAW_MS : 150u)) {
            last_draw = now;
            render(now, next, pin, power_reason);
        }
        if (title)
            lv_timer_handler();
        if (wallpaper_requested) {
            wallpaper_requested = false;
            if (next == PAGE_FUNCTION && !manual_requested &&
                !apply_wallpaper((wallpaper_idx + 1u) % g_wallpaper_count))
                lv_label_set_text(wallpaper_status, "保存失败");
        }
        if (power_blank && key1_released) {
            /* The screen blanks at full progress. Start the high-level wake
             * detector after this press ends so it cannot wake itself. */
            key1_power = KEY1_POWER_IDLE;
            watch_rtc_hibernate(rt_pin_read(VBUS_DET) == PIN_HIGH);
        }
        rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
        bool sent_sos = outbox(now);
#ifdef WATCH_SOS_RESOLVED_UPLINK
        sent_sos = sent_sos || send_resolution(now);
#endif
        rt_mutex_release(&store_mutex);
        if (sample_ready) {
            uint8_t frame[WATCH_FRAME_SIZE];
            watch_frame_imu(frame, seq++, now, &sample);
            if (!sent_sos)
                watch_ble_send(frame);
        }
        rt_uint32_t key_events;
        rt_event_recv(&key_wake, KEY2_EDGE_EVENT, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      rt_tick_from_millisecond(WATCH_POLL_MS), &key_events);
    }
}
