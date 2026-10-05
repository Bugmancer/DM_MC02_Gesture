#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "../display.c"

SCB_Type mock_scb;
SPI_HandleTypeDef hspi1;
static HAL_SPI_StateTypeDef spi_state;
static uint32_t tick;
static unsigned int transfers;
static unsigned int gpio_writes;
static uint8_t fail_dma;
static GPIO_PinState chip_select;
static GPIO_PinState data_command;
static GPIO_PinState backlight;
static uint8_t sent[TILE_BYTES];

void HAL_GPIO_WritePin(uint32_t port, uint16_t pin, GPIO_PinState state)
{
    (void)pin;
    ++gpio_writes;
    if (port == LCD_CS_GPIO_Port || port == LCD_DC_GPIO_Port) {
        assert(spi_state == HAL_SPI_STATE_READY);
    }
    if (port == LCD_CS_GPIO_Port) { chip_select = state; }
    if (port == LCD_DC_GPIO_Port) { data_command = state; }
    if (port == LCD_BLK_GPIO_Port) { backlight = state; }
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi, uint8_t *data,
                                    uint16_t size, uint32_t timeout)
{
    (void)spi;
    (void)data;
    (void)timeout;
    assert(size > 0U && size <= 14U);
    assert(spi_state == HAL_SPI_STATE_READY);
    assert(chip_select == GPIO_PIN_RESET);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *spi, uint8_t *data, uint16_t size)
{
    (void)spi;
    assert(size == 2240U);
    assert(spi_state == HAL_SPI_STATE_READY);
    assert(chip_select == GPIO_PIN_RESET);
    assert(data_command == GPIO_PIN_SET);
    if (fail_dma) { return HAL_ERROR; }
    memcpy(sent, data, size);
    spi_state = HAL_SPI_STATE_BUSY_TX;
    ++transfers;
    return HAL_OK;
}

HAL_SPI_StateTypeDef HAL_SPI_GetState(SPI_HandleTypeDef *spi)
{
    (void)spi;
    return spi_state;
}

uint32_t HAL_SPI_GetError(SPI_HandleTypeDef *spi) { (void)spi; return 0U; }
uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t delay) { tick += delay; }
void SCB_CleanDCache_by_Addr(uint32_t *address, int32_t size)
{
    assert(((uintptr_t)address & 31U) == 0U);
    assert(size == 2240);
}

/* Hardware reset and the AXI address assertion are verified on the target. */
static void reset_refresh(void)
{
    memset(&current_view, 0, sizeof(current_view));
    current_view.best_slot = -1;
    compose_rows();
    memset(shown, 0, sizeof(shown));
    healthy = 1U;
    dma_active = 0U;
    valid_rows = 0U;
    active_row = LCD_ROWS;
    search_row = 0U;
    pending_view = 0U;
    last_compose = 0U;
    transfers = 0U;
    gpio_writes = 0U;
    fail_dma = 0U;
    tick = 0U;
    spi_state = HAL_SPI_STATE_READY;
    chip_select = GPIO_PIN_SET;
    data_command = GPIO_PIN_SET;
    backlight = GPIO_PIN_RESET;
    mock_scb.CCR = SCB_CCR_DC_Msk;
}

static void finish_transfer(void)
{
    assert(memcmp(sent, tile, sizeof(sent)) == 0);
    spi_state = HAL_SPI_STATE_READY;
    ++tick;
    display_process();
}

static void test_initial_frame_and_unchanged_view(void)
{
    unsigned int i;
    reset_refresh();
    display_process();
    for (i = 0U; i < 59U; ++i) {
        assert(backlight == GPIO_PIN_RESET);
        finish_transfer();
    }
    assert(transfers == 60U);
    finish_transfer();
    assert(backlight == GPIO_PIN_SET);
    display_set_view(&current_view);
    tick += 100U;
    display_process();
    assert(transfers == 60U);
    assert(display_is_ok());
}

static void test_updates_cannot_touch_active_dma(void)
{
    display_view_t next = {0};
    unsigned int writes;
    reset_refresh();
    display_process();
    writes = gpio_writes;
    next.state = DISPLAY_STATE_LEARNING;
    next.selected_slot = 7U;
    next.learning_count = 2U;
    (void)snprintf(next.message, sizeof(next.message), "NEW SAMPLE");
    display_set_view(&next);
    tick += 5U;
    display_process();
    assert(gpio_writes == writes);
    assert(transfers == 1U);
    assert(memcmp(sent, tile, sizeof(sent)) == 0);
    finish_transfer();
    assert(transfers == 2U);
    assert(display_is_ok());
}

static void test_timeout_stops_without_touching_spi(void)
{
    unsigned int writes;
    reset_refresh();
    display_process();
    writes = gpio_writes;
    tick += 21U;
    display_process();
    assert(!display_is_ok());
    assert(gpio_writes == writes);
    assert(memcmp(sent, tile, sizeof(sent)) == 0);
    display_process();
    assert(transfers == 1U);
}

static void test_start_failure(void)
{
    reset_refresh();
    fail_dma = 1U;
    display_process();
    assert(!display_is_ok());
    assert(chip_select == GPIO_PIN_SET);
    assert(transfers == 0U);
}

static void test_unterminated_and_short_messages(void)
{
    display_view_t next = {0};
    reset_refresh();
    memset(next.message, 'A', sizeof(next.message));
    display_set_view(&next);
    tick = 100U;
    display_process();
    assert(strlen(wanted[10].text) == 33U);
    assert(strlen(wanted[11].text) == 6U);
    spi_state = HAL_SPI_STATE_READY;
    memset(next.message, 'Z', sizeof(next.message));
    memcpy(next.message, "OK", 3U);
    display_set_view(&next);
    tick = 200U;
    display_process();
    assert(strcmp(wanted[10].text, "OK") == 0);
    assert(wanted[11].text[0] == '\0');
}

static void test_device_settings_and_active_slots_fit_panel(void)
{
    unsigned int row, selection;
    reset_refresh();
    current_view.class_limit = 3U;
    current_view.config_demos = 2U;
    current_view.config_rgb_hold_ms = 30000U;
    current_view.config_slot = 2U;
    current_view.slot_colors[2] = 0x12abffUL;
    current_view.state = DISPLAY_STATE_ARMED;
    compose_rows();
    assert(strstr(wanted[2].text, "1/3") != NULL);
    assert(strstr(wanted[3].text, "4:") == NULL);
    assert(wanted[4].text[0] == '\0');
    assert(strstr(wanted[13].text, "SETTINGS") != NULL);
    current_view.settings_open = 1U;
    (void)snprintf(current_view.message, sizeof(current_view.message), "Settings save failed. OK retries");
    for (selection = 0U; selection < 9U; ++selection) {
        current_view.settings_row = (uint8_t)selection;
        compose_rows();
        assert(wanted[selection + 3U].text[0] == '>');
        for (row = 0U; row < LCD_ROWS; ++row) assert(strlen(wanted[row].text) <= TEXT_LENGTH);
        assert(strstr(wanted[12].text, "12ABFF") != NULL);
        assert(strstr(wanted[2].text, "save failed") != NULL);
        assert(strstr(wanted[5].text, "30.0 s") != NULL);
        assert(strstr(wanted[7].text, "18") != NULL);
        assert(strstr(wanted[8].text, "171") != NULL);
        assert(strstr(wanted[9].text, "255") != NULL);
    }
    current_view.settings_row = 255U;
    current_view.config_slot = 255U;
    compose_rows();
    assert(wanted[3].text[0] == '>');
}

int main(void)
{
    test_initial_frame_and_unchanged_view();
    test_updates_cannot_touch_active_dma();
    test_timeout_stops_without_touching_spi();
    test_start_failure();
    test_unterminated_and_short_messages();
    test_device_settings_and_active_slots_fit_panel();
    puts("Display transport and settings tests passed (6 cases).");
    return 0;
}
