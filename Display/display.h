#ifndef GESTURE_DISPLAY_H
#define GESTURE_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_STATE_IDLE = 0,
    DISPLAY_STATE_ARMED,
    DISPLAY_STATE_LEARNING,
    DISPLAY_STATE_READY,
    DISPLAY_STATE_SAVING,
    DISPLAY_STATE_UNKNOWN,
    DISPLAY_STATE_MATCH,
    DISPLAY_STATE_CONFIRM,
    DISPLAY_STATE_ERROR
} display_state_t;

typedef struct {
    display_state_t state;
    uint8_t selected_slot;
    uint8_t slot_templates[8];
    uint8_t learning_count;
    uint8_t learning_target;
    uint8_t power_ok;
    uint8_t imu_ok;
    uint8_t flash_ok;
    int8_t best_slot;
    float best_score;
    float second_score;
    uint32_t rejected_count;
    uint32_t sample_drops;
    char message[40];
} display_view_t;

/* Call after MX_SPI1_Init, before starting sample acquisition. */
uint8_t display_init(void);
/* Main-loop only; copies the view. Slot indices are zero-based. */
void display_set_view(const display_view_t *view);
/* Main-loop only; submits at most one 280 x 4 pixel DMA transfer. */
void display_process(void);
uint8_t display_is_ok(void);

#ifdef __cplusplus
}
#endif

#endif
