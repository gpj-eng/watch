#include "alarm.h"
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_alarm(void)
{
    struct watch_alarm alarm = {.cfg = &alarm_cfg_default};
    /* Begin near the uint32 timestamp rollover. */
    uint32_t start = UINT32_MAX - 8100u;
    watch_alarm_fall(&alarm, start, false);
    assert(watch_alarm_seconds(&alarm, start) == 30);
    assert(watch_alarm_vibrate(&alarm, start));
    assert(watch_alarm_vibrate(&alarm, start + 599u));
    assert(!watch_alarm_vibrate(&alarm, start + 600u));
    assert(!watch_alarm_vibrate(&alarm, start + 1499u));
    assert(watch_alarm_vibrate(&alarm, start + 1500u));
    assert(watch_alarm_update(&alarm, start + 19999u, false) == EVT_ALARM_NONE);
    assert(alarm.phase == ALARM_STATE_COUNTDOWN_NORMAL);
    assert(watch_alarm_vibrate(&alarm, start + 19999u));
    assert(watch_alarm_update(&alarm, start + 20000u, false) == EVT_ALARM_URGENT);
    assert(alarm.phase == ALARM_STATE_COUNTDOWN_URGENT);
    assert(watch_alarm_vibrate(&alarm, start + 20000u));
    assert(watch_alarm_vibrate(&alarm, start + 20499u));
    assert(!watch_alarm_vibrate(&alarm, start + 20500u));
    assert(!watch_alarm_vibrate(&alarm, start + 20899u));
    assert(watch_alarm_vibrate(&alarm, start + 20900u));
    assert(watch_alarm_update(&alarm, start + 29999u, true) == EVT_ALARM_CANCEL);
    assert(alarm.phase == ALARM_STATE_IDLE && !watch_alarm_vibrate(&alarm, start + 29999u));
    assert(watch_alarm_update(&alarm, start + 30000u, false) == EVT_ALARM_NONE);

    alarm_cfg_t shorter = alarm_cfg_default;
    shorter.cancel_timeout_ms = 12000u;
    alarm.cfg = &shorter;
    watch_alarm_fall(&alarm, 0, false);
    assert(watch_alarm_seconds(&alarm, 0) == 12);
    assert(watch_alarm_update(&alarm, 7999u, false) == EVT_ALARM_NONE);
    assert(watch_alarm_update(&alarm, 8000u, false) == EVT_ALARM_URGENT);
    assert(watch_alarm_seconds(&alarm, 8000u) == 4);
    assert(watch_alarm_vibrate(&alarm, 8499u));
    assert(!watch_alarm_vibrate(&alarm, 8500u));
    assert(watch_alarm_vibrate(&alarm, 11999u));
    assert(!watch_alarm_vibrate(&alarm, 12000u));
    assert(watch_alarm_update(&alarm, 12000u, true) == EVT_SOS_TIMEOUT);
    assert(alarm.phase == ALARM_STATE_SOS_ACTIVE && alarm.reason == WATCH_FALL);
    assert(watch_alarm_update(&alarm, 12001u, false) == EVT_ALARM_NONE);
    assert(watch_alarm_manual(&alarm) == EVT_ALARM_NONE);
    watch_alarm_fall(&alarm, 12001u, false);
    assert(alarm.phase == ALARM_STATE_SOS_ACTIVE);
    assert(watch_alarm_vibrate(&alarm, 12000u));
    assert(watch_alarm_vibrate(&alarm, 12299u));
    assert(!watch_alarm_vibrate(&alarm, 12300u));
    assert(!watch_alarm_vibrate(&alarm, 12999u));
    assert(watch_alarm_vibrate(&alarm, 13000u));
    assert(watch_alarm_hold(&alarm, true, false, 15500u) == EVT_ALARM_NONE);
    assert(!alarm.holding);
    assert(watch_alarm_hold(&alarm, true, true, 16000u) == EVT_ALARM_NONE);
    assert(watch_alarm_hold(&alarm, true, false, 17000u) == EVT_ALARM_NONE && alarm.holding);
    assert(watch_alarm_hold(&alarm, false, false, 17000u) == EVT_ALARM_NONE);
    assert(!alarm.holding && alarm.phase == ALARM_STATE_SOS_ACTIVE);
    uint32_t press = UINT32_MAX - 1000u;
    assert(watch_alarm_hold(&alarm, true, true, press) == EVT_ALARM_NONE);
    assert(watch_alarm_hold(&alarm, true, false, press + 2999u) == EVT_ALARM_NONE);
    assert(watch_alarm_hold(&alarm, true, false, press + 3000u) == EVT_SOS_RESOLVED);
    assert(!alarm.holding && !watch_alarm_vibrate(&alarm, press + 3000u));
    assert(watch_alarm_hold(&alarm, true, false, press + 3001u) == EVT_ALARM_NONE);
    assert(watch_alarm_manual(&alarm) == EVT_SOS_MANUAL);
    assert(alarm.phase == ALARM_STATE_IDLE && alarm.reason == WATCH_MANUAL_SOS);

    watch_alarm_fall(&alarm, 0, true);
    assert(watch_alarm_update(&alarm, 12000u, false) == EVT_SOS_TIMEOUT);
    assert(alarm.demo && watch_alarm_hold(&alarm, true, true, 13000u) == EVT_ALARM_NONE);
    assert(watch_alarm_hold(&alarm, true, false, 16000u) == EVT_SOS_RESOLVED);
    assert(alarm.phase == ALARM_STATE_IDLE);
}

int main(void)
{
    test_alarm();
    struct watch_detector detector = {0};
    const struct watch_imu still = {.az = 1000}, impact = {.ax = 3400, .gx = 2500}, settled = {.ay = 1000};
    for (unsigned i = 0; i < 20; ++i)
        assert(!watch_detect(&detector, &still, i * 19));
    assert(!watch_detect(&detector, &impact, 380));
    for (unsigned i = 0; i < 155; ++i)
        assert(!watch_detect(&detector, &settled, 399 + i * 19));
    assert(watch_detect(&detector, &settled, 399 + 155 * 19));

    const struct watch_sos sos = {
        .id = 0xfe12345600000001ull, .reason = WATCH_FALL, .count = 2,
        .phones = {"+15550100123", "+15550100124"},
    };
    const uint8_t expected[] = {
        0x02, 0x01, 0x01, 0, 0, 0, 0x56, 0x34, 0x12, 0xfe,
        0, 2, 0x15, 0x55, 0x01, 0x00, 0x12, 0x3f, 0xff, 0xff,
    };
    uint8_t frame[20] = {0};
    assert(watch_frame_sos(frame, &sos, 0));
    assert(!memcmp(frame, expected, sizeof(frame)));
    frame[0] = 0x81;
    frame[1] = 2;
    memset(&frame[10], 0, 10);
    uint64_t id = 0;
    uint8_t type = 0, status = 0;
    assert(watch_frame_ack(frame, sizeof(frame), &type, &id, &status));
    assert(type == WATCH_SOS_ACK_TYPE && id == sos.id && status == 2);
    frame[0] = WATCH_RESOLVED_ACK_TYPE;
    assert(watch_frame_ack(frame, sizeof(frame), &type, &id, &status));
    assert(type == WATCH_RESOLVED_ACK_TYPE && id == sos.id && status == 2);
    frame[10] = 1;
    assert(!watch_frame_ack(frame, sizeof(frame), &type, &id, &status));
    const uint8_t device[] = {0xf0, 0xab, 0x32, 0x12, 0x34, 0x56};
    const uint8_t resolved_frame[] = {
        0x03, 0x01, 0x01, 0, 0, 0, 0x56, 0x34, 0x12, 0xfe,
        0xf0, 0xab, 0x32, 0x12, 0x34, 0x56, 0x12, 0x34, 0x56, 0x78,
    };
    assert(watch_frame_resolved(frame, sos.id, device, 0x78563412u));
    assert(!memcmp(frame, resolved_frame, sizeof(resolved_frame)));
    assert(!watch_frame_resolved(frame, 0, device, 0));

    puts("PASS alarm timings, hold and release, synthetic fall, and SOS/resolved frames");
    return 0;
}
