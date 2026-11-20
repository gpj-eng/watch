#include "watch_board.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <finsh.h>
#include <flashdb.h>
#include <fal.h>
#include <string.h>
#include <littlevgl2rtt.h>
#include <lvgl.h>
LV_FONT_DECLARE(watch_cjk);

#define KEY2 43
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
static bool store_ok;
static struct watch_alarm alarm;
static struct watch_detector detector;
static volatile bool demo_requested;
static bool imu_ok, manual_requested, cancel_requested;
static uint32_t sensor_seen;
static lv_obj_t *title, *countdown, *caption, *action, *action_label;
static const char *notice;
static uint32_t notice_started;

uint32_t watch_now_ms(void)
{
    /* Keep elapsed arithmetic valid even when the RTOS tick wraps. */
    return (uint32_t)((uint64_t)rt_tick_get() * 1000u / RT_TICK_PER_SECOND);
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
    store_ok = fal_init() >= 0 && fdb_kvdb_init(&database, "watch", "watch_sos", NULL, NULL) == FDB_NO_ERR;
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
    demo_requested = true;
}
MSH_CMD_EXPORT(watch_demo, Run local alarm demo without sending SOS);

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
    if (alarm.demo) {
        show_notice("演示结束", now);
        return;
    }
    rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
    struct persisted next = stored;
    unsigned slot = 0;
    while (slot < MAX_PENDING && next.pending[slot].id)
        ++slot;
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
    if (alarm.phase == WATCH_COUNTDOWN && code == LV_EVENT_SHORT_CLICKED)
        cancel_requested = true;
    if (alarm.phase == WATCH_IDLE && code == LV_EVENT_LONG_PRESSED)
        manual_requested = true;
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
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111919), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    const lv_font_t *font = &watch_cjk;
    title = label(screen, 63, font, 0xf3f8f4);
    countdown = label(screen, 112, &lv_font_montserrat_48, 0xffffff);
    caption = label(screen, 212, font, 0xc7d6d0);
    action = lv_button_create(screen);
    lv_obj_set_size(action, 300, 82);
    lv_obj_align(action, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_set_style_radius(action, 20, 0);
    lv_obj_add_event_cb(action, action_pressed, LV_EVENT_ALL, NULL);
    action_label = label(action, 0, font, 0xffffff);
    lv_obj_set_width(action_label, 270);
    lv_obj_center(action_label);
}

static void render(uint32_t now)
{
    if (!title)
        return;
    const bool active = alarm.phase == WATCH_COUNTDOWN;
    const bool linked = watch_ble_ready();
    unsigned pin = watch_ble_pin();
    if (active) {
        lv_label_set_text(title, alarm.demo ? "演示提醒" : "疑似跌倒");
        lv_label_set_text_fmt(countdown, "%02u", watch_alarm_seconds(&alarm, now));
        lv_label_set_text(caption, alarm.demo ? "演示不会发送短信" : "秒后提交求助");
    } else if (pin < 1000000u) {
        lv_label_set_text(title, "蓝牙配对");
        lv_label_set_text_fmt(countdown, "%06u", pin);
        lv_label_set_text(caption, "请在接收端输入");
    } else {
        lv_label_set_text(title, notice && now - notice_started < 12000u ? notice : "安全手表");
        lv_label_set_text(countdown, "SOS");
        lv_label_set_text(caption, !imu_ok || now - sensor_seen > 500u ? "传感器异常" :
                                      linked ? "接收端已连接" : "接收端未连接");
    }
    lv_obj_set_style_bg_color(action, lv_color_hex(active ? 0x286354 : 0xa73732), 0);
    lv_label_set_text(action_label, active ? "我没事" : "长按求助");
}

int main(void)
{
    load_store();
    rt_pin_mode(KEY2, PIN_MODE_INPUT_PULLDOWN);
    imu_ok = watch_imu_init() == 0;
    watch_audio_init();
    watch_ble_init();
    init_ui();
    rt_kprintf("WATCH_READY imu=%d storage=%d display=%d\n", imu_ok, store_ok, title != NULL);
    uint32_t last_draw = 0, down_since = 0;
    uint16_t seq = 0;
    bool key_raw = false, key_stable = false, handled = false;
    uint32_t key_changed = 0;
    while (true) {
        uint32_t now = watch_now_ms();
        watch_ble_poll();
        bool pressed = rt_pin_read(KEY2) == PIN_HIGH;
        if (pressed != key_raw) {
            key_raw = pressed;
            key_changed = now;
        }
        if (key_stable != key_raw && now - key_changed >= 30u) {
            key_stable = key_raw;
            if (key_stable) {
                down_since = now;
                handled = alarm.phase == WATCH_COUNTDOWN;
                cancel_requested |= handled;
            }
        }
        if (key_stable && !handled && now - down_since >= 1200u) {
            handled = true;
            manual_requested = true;
        }
        if (demo_requested) {
            demo_requested = false;
            watch_alarm_fall(&alarm, now, true);
        }
        if (manual_requested) {
            manual_requested = false;
            watch_alarm_manual(&alarm, now);
        }
        watch_alarm_update(&alarm, now, cancel_requested);
        cancel_requested = false;
        if (alarm.phase == WATCH_SUBMIT || alarm.phase == WATCH_CANCELLED) {
            if (alarm.phase == WATCH_SUBMIT)
                submit(now);
            else {
                show_notice("已取消", now);
                rt_kprintf("WATCH_CANCELLED\n");
            }
            alarm.phase = WATCH_IDLE;
            detector = (struct watch_detector){0};
        }

        struct watch_imu sample;
        bool sample_ready = imu_ok && watch_imu_read(&sample);
        if (sample_ready) {
            sensor_seen = now;
            if (alarm.phase == WATCH_IDLE && watch_detect(&detector, &sample, now)) {
                watch_alarm_fall(&alarm, now, false);
                rt_kprintf("WATCH_FALL candidate\n");
            }
        }
        watch_motor(watch_alarm_vibrate(&alarm, now));
        watch_audio(alarm.phase == WATCH_COUNTDOWN);
        rt_mutex_take(&store_mutex, RT_WAITING_FOREVER);
        bool sent_sos = outbox(now);
        rt_mutex_release(&store_mutex);
        if (sample_ready) {
            uint8_t frame[20];
            watch_frame_imu(frame, seq++, now, &sample);
            if (!sent_sos)
                watch_ble_send(frame);
        }
        if (now - last_draw >= 150u) {
            last_draw = now;
            render(now);
        }
        if (title)
            lv_timer_handler();
        rt_thread_mdelay(4);
    }
}
