#include "alarm.h"
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct watch_alarm alarm = {0};
    /* Begin near the uint32 timestamp rollover. */
    uint32_t start = UINT32_MAX - 8100u;
    watch_alarm_fall(&alarm, start, false);
    assert(watch_alarm_seconds(&alarm, start) == 30);
    assert(watch_alarm_vibrate(&alarm, start + 9999u));
    assert(!watch_alarm_vibrate(&alarm, start + 10000u));
    watch_alarm_update(&alarm, start + 29999u, false);
    assert(alarm.phase == WATCH_COUNTDOWN);
    watch_alarm_update(&alarm, start + 29999u, true);
    assert(alarm.phase == WATCH_CANCELLED);
    watch_alarm_fall(&alarm, 0, false);
    watch_alarm_update(&alarm, 30000, false);
    assert(alarm.phase == WATCH_SUBMIT && alarm.reason == WATCH_FALL);
    watch_alarm_manual(&alarm, 0);
    assert(alarm.phase == WATCH_SUBMIT && alarm.reason == WATCH_MANUAL_SOS);

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
    uint8_t status = 0;
    assert(watch_frame_ack(frame, sizeof(frame), &id, &status));
    assert(id == sos.id && status == 2);

    puts("PASS alarm timings, cancellation, synthetic fall and SOS/ACK frames");
    return 0;
}
