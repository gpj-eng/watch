#include "watch_board.h"
#include "watch_time.h"
#include "watch_wrist.h"
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

#define KEY1 34
#define KEY2 43
#define VBUS_DET 44
#define KEY1_DEBOUNCE_MS 30u
#define KEY1_SHORT_MS 500u
#define KEY1_LONG_MS 3000u
#define KEY1_POWER_GUARD_MS 300u
#define NOTICE_MS 12000u
#define KEY2_EDGE_EVENT (1u << 0)
#define KEY2_DEBOUNCE_MS 30u
#define KEY2_MANUAL_HOLD_MS 1200u
#define WATCH_POLL_MS 4u
#define IMU_ACTIVITY_GAP_MS 100u
#define WATCH_PANEL_BRIGHTNESS 50u
#define MAX_PENDING 4
#define STORE_VERSION 0x48530201u

struct persisted {
    uint32_t version;
    uint64_t sequence;
    char contacts[WATCH_MAX_CONTACTS][WATCH_PHONE_SIZE];
    struct watch_sos pending[MAX_PENDING];
};

static struct persisted stored = {.version = STORE_VERSION};
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
enum watch_page { PAGE_NONE, PAGE_DIAL, PAGE_FUNCTION, PAGE_SLEEP, PAGE_ALARM };
static enum watch_page visible_page;
static lv_obj_t *screen, *wallpaper, *title, *reading, *caption, *action, *action_label, *sync_hint;
static lv_obj_t *dial_date, *dial_digits[5];
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

static void submit(uint32_t now)
{
    if (alarm.demo)
        return;
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
                return;
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
            show_notice("等待接收端", now);
            rt_kprintf("WATCH_SOS queued reason=%u contacts=%u\n", item->reason, item->count);
        } else {
            show_notice("闪存存储失败", now);
        }
    }
    rt_mutex_release(&store_mutex);
}

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
        watch_motor(false, alarm.cfg->vib_pwm_duty);
        rt_kprintf("WATCH_SOS_TIMEOUT ms=%u event=EVT_SOS_TIMEOUT\n", (unsigned)now);
        watch_voice_set(WATCH_VOICE_HELP);
        submit(now);
        bench_alarm = false;
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
    uint8_t status;
    for (unsigned n = 0; n < 8 && watch_ble_ack(&id, &status); ++n) {
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
    if (visible_page == PAGE_FUNCTION && alarm.phase == ALARM_STATE_IDLE && code == LV_EVENT_LONG_PRESSED) {
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

static void init_ui(void)
{
    if (littlevgl2rtt_init("lcd") != RT_EOK)
        return;
    lcd = rt_device_find("lcd");
    screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    wallpaper = lv_obj_create(screen);
    lv_obj_remove_style_all(wallpaper);
    lv_obj_set_size(wallpaper, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(wallpaper, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(wallpaper, lv_color_hex(0x102038), 0);
    lv_obj_set_style_bg_grad_color(wallpaper, lv_color_hex(0x2a3f5f), 0);
    lv_obj_set_style_bg_grad_dir(wallpaper, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(wallpaper, LV_OPA_COVER, 0);
    dial_date = label(wallpaper, 57, &watch_dial_date, 0xffffff);
    lv_obj_t *clock_row = lv_obj_create(wallpaper);
    lv_obj_remove_style_all(clock_row);
    lv_obj_remove_flag(clock_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(clock_row, 364, 200);
    lv_obj_align(clock_row, LV_ALIGN_TOP_MID, 0, 105);
    int x = 0;
    for (unsigned i = 0; i < 5; ++i) {
        const int width = i == 2 ? 32 : 83;
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
    sync_hint = lv_obj_create(screen);
    lv_obj_remove_style_all(sync_hint);
    lv_obj_remove_flag(sync_hint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sync_hint, 7, 7);
    lv_obj_align(sync_hint, LV_ALIGN_TOP_RIGHT, -37, 45);
    lv_obj_set_style_radius(sync_hint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sync_hint, lv_color_hex(0xffcb91), 0);
    lv_obj_set_style_bg_opa(sync_hint, LV_OPA_COVER, 0);
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

    if (usb != vbus) {
        vbus = usb;
        power_changed = now;
        consumed |= raw || stable || pressed;
        rt_kprintf("WATCH_POWER vbus=%d battery=%s\n", usb, usb ? "unknown" : "only");
    }
    if (raw != pressed) {
        raw = pressed;
        changed = now;
    }
    if (alarm_active && (stable || raw) && !consumed) {
        consumed = true;
        rt_kprintf("WATCH_KEY1 ignored=alarm\n");
    }
    bool activity = false;
    if (stable != raw && now - changed >= KEY1_DEBOUNCE_MS) {
        stable = raw;
        if (stable) {
            down_since = changed;
            consumed |= now - power_changed < KEY1_POWER_GUARD_MS;
            activity = !consumed;
            if (!consumed && *page == PAGE_SLEEP) {
                *page = PAGE_DIAL;
                consumed = true;
            }
        } else {
            uint32_t held = changed - down_since;
            if (!consumed && held < KEY1_SHORT_MS) {
                *page = *page == PAGE_DIAL ? PAGE_FUNCTION : PAGE_DIAL;
                rt_kprintf("WATCH_KEY1 short ms=%u\n", (unsigned)held);
            }
            consumed = false;
        }
    } else if (!stable && !raw && now - changed >= KEY1_DEBOUNCE_MS) {
        consumed = false;
    }
    if (stable && raw && !consumed && now - down_since >= KEY1_LONG_MS) {
        consumed = true;
        if (*page != PAGE_SLEEP) {
            *page = PAGE_SLEEP;
            rt_kprintf("WATCH_KEY1 long mode=monitor_sleep vbus=%d battery=%s\n",
                       vbus, vbus ? "unknown" : "only");
        }
    }
    return activity;
}

static void render(uint32_t now, enum watch_page next, unsigned pin, const char *power_reason)
{
    if (!title)
        return;
    const bool alarm_page = next == PAGE_ALARM;
    const bool counting = alarm_page && watch_alarm_counting(&alarm);
    if (visible_page != next) {
        static const char *const names[] = {
            [PAGE_DIAL] = "dial", [PAGE_FUNCTION] = "function",
            [PAGE_SLEEP] = "sleep", [PAGE_ALARM] = "alarm",
        };
        if (visible_page == PAGE_SLEEP || next == PAGE_SLEEP) {
            /* Board-level LCD power-off also cuts the touchscreen's shared 3.3 V rail. */
            uint8_t brightness = next == PAGE_SLEEP ? 0 : WATCH_PANEL_BRIGHTNESS;
            if (!lcd || rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness) != RT_EOK) {
                rt_kprintf("WATCH_DISPLAY brightness=%u error\n", (unsigned)brightness);
                return;
            }
            if (next == PAGE_SLEEP) {
                lv_display_enable_invalidation(NULL, false);
                lv_timer_pause(lv_display_get_refr_timer(NULL));
                set_visible(action, false);
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
        set_visible(title, next != PAGE_DIAL);
        set_visible(reading, next != PAGE_DIAL);
        set_visible(caption, next != PAGE_DIAL);
    } else if (next == PAGE_SLEEP) {
        return;
    }

    set_visible(action, next == PAGE_FUNCTION || counting);
    set_visible(sync_hint, next == PAGE_DIAL && !watch_time_is_synced());
    char value[8];
    const bool notifying = notice && now - notice_started < NOTICE_MS;
    if (counting) {
        rt_snprintf(value, sizeof(value), "%02u", watch_alarm_seconds(&alarm, now));
        update_label(title, alarm.demo || bench_alarm ? "演示提醒" : "疑似跌倒");
        update_label(reading, value);
        update_label(caption, alarm.demo ? "演示不会发送短信" : "短按取消");
        update_label(action_label, "我没事");
    } else if (alarm_page) {
        update_label(title, alarm.demo ? "演示提醒" : "求助");
        update_label(reading, "SOS");
        update_label(caption, alarm.demo ? "演示不会发送短信" : notifying ? notice : "");
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
    } else {
        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        struct tm local;
        char date[32], time[6];
        if (watch_time_calendar(now, &local)) {
            rt_snprintf(date, sizeof(date), "%d月%d日 周%s", local.tm_mon + 1,
                        local.tm_mday, weekdays[local.tm_wday]);
            rt_snprintf(time, sizeof(time), "%02d:%02d", local.tm_hour, local.tm_min);
            update_label(dial_date, date);
            for (unsigned i = 0; i < 5; ++i) {
                const char digit[] = {time[i], '\0'};
                update_label(dial_digits[i], digit);
            }
        }
    }
}

int main(void)
{
    load_store();
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
    bool key_raw = false, key_stable = false, handled = false, pressed_in_sos = false;
    uint32_t key_changed = 0;
    enum watch_page page = PAGE_DIAL;
    bool manual_page = false;
    uint32_t manual_since = 0;
    unsigned last_pin = 1000000u;
    struct watch_wrist wrist = {0};
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
        if (watch_alarm_finish_voice(&alarm, voice_event)) {
            watch_voice_set(WATCH_VOICE_OFF);
            detector = (struct watch_detector){0};
            if (alarm.demo) {
                show_notice("演示结束", now);
                rt_kprintf("WATCH_DEMO_END ms=%u\n", (unsigned)now);
            }
            rt_kprintf("WATCH_VOICE_EXIT ms=%u event=%s\n", (unsigned)now,
                       voice_event == EVT_SOS_VOICE_STOPPED ? "EVT_SOS_VOICE_STOPPED" :
                       voice_event == EVT_SOS_VOICE_DONE ? "EVT_SOS_VOICE_DONE" : "EVT_SOS_VOICE_FAILED");
        }

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
                handled = watch_alarm_counting(&alarm);
                pressed_in_sos = alarm.phase == ALARM_STATE_SOS_ACTIVE;
                cancel_requested |= handled;
            }
        }
        if (key_raw && key_stable && !handled) {
            if (pressed_in_sos && alarm.phase == ALARM_STATE_SOS_ACTIVE &&
                now - down_since >= alarm.cfg->sos_voice_stop_hold_ms) {
                handled = true;
                rt_kprintf("WATCH_VOICE_STOP_REQUEST ms=%u source=KEY2 hold_ms=%u\n",
                           (unsigned)now, (unsigned)(now - down_since));
                watch_voice_set(WATCH_VOICE_STOP_HELP);
            } else if (!pressed_in_sos && alarm.phase == ALARM_STATE_IDLE &&
                       now - down_since >= KEY2_MANUAL_HOLD_MS) {
                handled = true;
                manual_requested = true;
            }
        }
        if (demo_requested) {
            uint8_t mode = demo_requested;
            demo_requested = 0;
            if (alarm.phase == ALARM_STATE_IDLE) {
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
            if (alarm.phase == ALARM_STATE_IDLE && watch_detect(&detector, &sample, now)) {
                start_countdown(now, false);
                rt_kprintf("WATCH_FALL candidate\n");
            }
        }
        if (key_stable && watch_alarm_counting(&alarm))
            handled = true;
        watch_motor(watch_alarm_vibrate(&alarm, now), alarm.cfg->vib_pwm_duty);
        rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
        bool sent_sos = outbox(now);
        rt_mutex_release(&store_mutex);
        if (sample_ready) {
            uint8_t frame[20];
            watch_frame_imu(frame, seq++, now, &sample);
            if (!sent_sos)
                watch_ble_send(frame);
        }
        uint32_t epoch;
        bool time_changed = watch_ble_time(&epoch);
        if (time_changed)
            watch_time_sync(epoch, now);
        watch_time_poll(now);

        if (manual_page && now - manual_since >= NOTICE_MS)
            manual_page = false;
        const bool alarm_active = alarm.phase != ALARM_STATE_IDLE || manual_page;
        if (!alarm_active && visible_page == PAGE_ALARM) {
            page = PAGE_DIAL;
            last_activity = now;
        }
        const enum watch_page from = page;
        const bool key1_activity = update_key1(&page, alarm_active || visible_page == PAGE_ALARM, now);
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
        } else if (from == PAGE_SLEEP && sample_ready && watch_wrist_update(&wrist, &sample, now)) {
            page = PAGE_DIAL;
            power_reason = "wrist";
            last_activity = now;
        } else if (from != PAGE_SLEEP && page == PAGE_SLEEP) {
            power_reason = "key1";
        }

        if (page != PAGE_SLEEP) {
            wrist = (struct watch_wrist){0};
            if (key1_activity || key2_activity || (sample_ready && (!recent_sample || !watch_wrist_still(&sample))))
                last_activity = now;
        }
        unsigned pin = watch_ble_pin();
        if (pin != last_pin && pin < 1000000u && !alarm_active && page != PAGE_SLEEP) {
            page = PAGE_FUNCTION;
            last_activity = now;
        }
        last_pin = pin;
        if (title && page != PAGE_SLEEP && !alarm_active && sample_ready && now - last_activity >= WATCH_SCREEN_TIMEOUT_MS &&
                lv_display_get_inactive_time(NULL) >= WATCH_SCREEN_TIMEOUT_MS) {
            page = PAGE_SLEEP;
            power_reason = "idle";
        }
        enum watch_page next = alarm_active ? PAGE_ALARM : page;
        if (time_changed || next != visible_page || now - last_draw >= 150u) {
            last_draw = now;
            render(now, next, pin, power_reason);
        }
        if (title)
            lv_timer_handler();
        rt_uint32_t key_events;
        rt_event_recv(&key_wake, KEY2_EDGE_EVENT, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      rt_tick_from_millisecond(WATCH_POLL_MS), &key_events);
    }
}
