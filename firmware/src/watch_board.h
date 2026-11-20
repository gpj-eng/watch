#ifndef WATCH_BOARD_H
#define WATCH_BOARD_H
#include "protocol.h"

int watch_imu_init(void);
bool watch_imu_read(struct watch_imu *sample);
void watch_motor(bool on);
void watch_audio_init(void);
void watch_audio(bool on);
void watch_ble_init(void);
void watch_ble_poll(void);
bool watch_ble_ready(void);
bool watch_ble_send(uint8_t frame[WATCH_FRAME_SIZE]);
bool watch_ble_ack(uint64_t *id, uint8_t *status);
unsigned watch_ble_pin(void);
void watch_ble_pair(void);
uint32_t watch_now_ms(void);

#endif
