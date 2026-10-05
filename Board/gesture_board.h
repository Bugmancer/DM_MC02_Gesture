#ifndef GESTURE_BOARD_H
#define GESTURE_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_READY_IMU   0x01u
#define BOARD_READY_KEYS  0x02u
#define BOARD_READY_FLASH 0x04u
#define BOARD_STORE_CAPACITY 32768u

typedef struct {
    uint32_t timestamp_ms;
    float accel[3]; /* m/s^2, including gravity. */
    float gyro[3];  /* rad/s, after the last successful stationary calibration. */
} BoardImuSample;

typedef enum {
    BOARD_KEY_NONE = 0,
    BOARD_KEY_UP,
    BOARD_KEY_DOWN,
    BOARD_KEY_LEFT,
    BOARD_KEY_RIGHT,
    BOARD_KEY_OK,
    BOARD_KEY_USER
} BoardKey;

/* Call after CubeMX peripheral initialization; returns the ready-bit mask. */
uint32_t board_init(void);
int board_read_imu(BoardImuSample *sample);
/* Blocking; keep the board still. Failure preserves the previous bias. */
int board_calibrate_gyro(uint32_t duration_ms);
/* Poll from the foreground; emits one event per debounced press. */
BoardKey board_poll_key(uint32_t now_ms);
/* Stable USER key level, updated independently of ADC keys by board_poll_key. */
uint8_t board_user_key_down(void);
void board_rgb(uint8_t red, uint8_t green, uint8_t blue);
/* 1 = success. Load sets length to zero if no valid record can be loaded. */
int board_store_load(void *buffer, uint32_t capacity, uint32_t *length);
/* Blocking; pause acquisition first. Uses only 0x7e0000..0x7fffff. */
int board_store_save(const void *data, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
