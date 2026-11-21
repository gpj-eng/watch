#ifndef WATCH_BOARD_H
#define WATCH_BOARD_H
#include "protocol.h"

int watch_imu_init(void);
bool watch_imu_read(struct watch_imu *sample);
void watch_motor(bool on, uint8_t duty);

enum watch_voice_mode { WATCH_VOICE_OFF, WATCH_VOICE_TONE, WATCH_VOICE_HELP };
void watch_voice_init(const alarm_cfg_t *cfg);
void watch_voice_set(enum watch_voice_mode mode);
enum watch_alarm_event watch_voice_take_event(void);
void watch_ble_init(void);
void watch_ble_poll(void);
bool watch_ble_ready(void);
bool watch_ble_send(uint8_t frame[WATCH_FRAME_SIZE]);
bool watch_ble_ack(uint8_t *type, uint64_t *id, uint8_t *status);
bool watch_ble_device_id(uint8_t id[WATCH_DEVICE_ID_SIZE]);
bool watch_ble_time(uint32_t *local_epoch);
unsigned watch_ble_pin(void);
void watch_ble_pair(void);
uint32_t watch_now_ms(void);

#endif
