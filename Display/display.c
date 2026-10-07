#include "display.h"
#include "main.h"
#include "spi.h"
#include <stdio.h>
#include <string.h>
#include "display_font.h"

#define LCD_WIDTH          280U
#define LCD_ROWS           15U
#define TEXT_HEIGHT        16U
#define TEXT_LENGTH        33U
#define TILE_HEIGHT        4U
#define TILE_BYTES         (LCD_WIDTH * TILE_HEIGHT * 2U)
#define ALL_ROWS           ((1UL << LCD_ROWS) - 1UL)
#define COLOR_BG           0x0841U
#define COLOR_TEXT         0xEF7DU
#define COLOR_MUTED        0xAD55U
#define COLOR_GREEN        0x57EAU
#define COLOR_CYAN         0x3E9FU
#define COLOR_YELLOW       0xFFE0U
#define COLOR_RED          0xFAAAU

typedef struct {
    char text[TEXT_LENGTH + 1U];
    uint16_t foreground;
} display_row_t;

/* DMA1 cannot access DTCM. Link this section into AXI SRAM. */
static uint8_t tile[TILE_BYTES] __attribute__((section(".dma_buffer"), aligned(32)));
static display_view_t current_view;
static display_row_t wanted[LCD_ROWS];
static display_row_t shown[LCD_ROWS];
static display_row_t active;
static uint32_t valid_rows;
static uint32_t last_compose;
static uint32_t transfer_start;
static uint8_t pending_view;
static uint8_t healthy;
static uint8_t dma_active;
static uint8_t active_row;
static uint8_t active_tile;
static uint8_t search_row;

static uint8_t lcd_command(uint8_t command, const uint8_t *data, uint16_t size)
{
    HAL_StatusTypeDef result;
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
    result = HAL_SPI_Transmit(&hspi1, &command, 1U, 1U);
    if (result == HAL_OK && size != 0U) {
        HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
        result = HAL_SPI_Transmit(&hspi1, (uint8_t *)data, size, 1U);
    }
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
    return (uint8_t)(result == HAL_OK);
}

static uint8_t lcd_window(uint16_t y)
{
    static const uint8_t columns[4] = {0U, 20U, 1U, 43U};
    uint16_t end = (uint16_t)(y + TILE_HEIGHT - 1U);
    uint8_t rows[4] = {(uint8_t)(y >> 8), (uint8_t)y,
                       (uint8_t)(end >> 8), (uint8_t)end};
    uint8_t command = 0x2CU;
    if (!lcd_command(0x2AU, columns, sizeof(columns)) ||
        !lcd_command(0x2BU, rows, sizeof(rows))) {
        return 0U;
    }
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
    if (HAL_SPI_Transmit(&hspi1, &command, 1U, 1U) != HAL_OK) {
        HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
        return 0U;
    }
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
    return 1U;
}

static void score_string(char *destination, size_t capacity, float score)
{
    uint32_t milli;
    if (!(score >= 0.0f && score < 1000.0f)) {
        (void)snprintf(destination, capacity, "---");
        return;
    }
    milli = (uint32_t)(score * 1000.0f + 0.5f);
    (void)snprintf(destination, capacity, "%lu.%03lu",
                   (unsigned long)(milli / 1000U), (unsigned long)(milli % 1000U));
}

static uint32_t capped_count(uint32_t count)
{
    return count > 99999999UL ? 99999999UL : count;
}

static void compose_settings(void)
{
    uint8_t slot = current_view.config_slot < 8U ? current_view.config_slot : 0U;
    uint8_t selected_row = current_view.settings_row < 9U ? current_view.settings_row : 0U;
    uint32_t color = current_view.slot_colors[slot] & 0xffffffUL;
    uint8_t i;
    (void)snprintf(wanted[1].text, sizeof(wanted[1].text), "DEVICE SETTINGS");
    wanted[1].foreground = COLOR_CYAN;
    (void)snprintf(wanted[2].text, sizeof(wanted[2].text), "%.*s", (int)TEXT_LENGTH, current_view.message);
    wanted[2].foreground = COLOR_YELLOW;
    (void)snprintf(wanted[3].text, sizeof(wanted[3].text), "  ACTION COUNT       %u / 8", (unsigned)current_view.class_limit);
    (void)snprintf(wanted[4].text, sizeof(wanted[4].text), "  DEMOS PER ACTION   %u / 20", (unsigned)current_view.config_demos);
    (void)snprintf(wanted[5].text, sizeof(wanted[5].text), "  RGB HOLD TIME      %u.%u s",
        (unsigned)current_view.config_rgb_hold_ms / 1000U,
        ((unsigned)current_view.config_rgb_hold_ms % 1000U) / 100U);
    (void)snprintf(wanted[6].text, sizeof(wanted[6].text), "  COLOR FOR ACTION   %u", (unsigned)slot + 1U);
    (void)snprintf(wanted[7].text, sizeof(wanted[7].text), "  RED                %3u", (unsigned)((color >> 16) & 255U));
    (void)snprintf(wanted[8].text, sizeof(wanted[8].text), "  GREEN              %3u", (unsigned)((color >> 8) & 255U));
    (void)snprintf(wanted[9].text, sizeof(wanted[9].text), "  BLUE               %3u", (unsigned)(color & 255U));
    (void)snprintf(wanted[10].text, sizeof(wanted[10].text), "  SAVE AND EXIT");
    (void)snprintf(wanted[11].text, sizeof(wanted[11].text), "  CANCEL");
    for (i = 3U; i <= 11U; ++i) wanted[i].foreground = COLOR_MUTED;
    wanted[3U + selected_row].text[0] = '>';
    wanted[3U + selected_row].foreground = COLOR_YELLOW;
    (void)snprintf(wanted[12].text, sizeof(wanted[12].text), "RGB #%06lX   < > CHANGE", (unsigned long)color);
    (void)snprintf(wanted[13].text, sizeof(wanted[13].text), "%s",
                   selected_row >= 4U && selected_row <= 6U ? "OK +1   < > +/-17" :
                   selected_row == 2U ? "HOLD +/-0.1s   UP/DOWN SELECT" : "UP/DOWN SELECT   OK SELECT");
    if (strlen(current_view.message) > TEXT_LENGTH)
        (void)snprintf(wanted[14].text, sizeof(wanted[14].text), "%s", current_view.message + TEXT_LENGTH);
    wanted[14].foreground = COLOR_YELLOW;
    wanted[12].foreground = wanted[13].foreground = COLOR_MUTED;
}

static void compose_rows(void)
{
    static const char * const state_names[] = {
        "IDLE", "AUTO", "LEARNING", "READY", "SAVING", "UNKNOWN",
        "MATCH", "CONFIRM", "ERROR"
    };
    uint8_t slot = current_view.selected_slot < 8U ? current_view.selected_slot : 0U;
    uint8_t class_limit = current_view.class_limit >= 1U && current_view.class_limit <= 8U ? current_view.class_limit : 8U;
    uint8_t state = (uint8_t)current_view.state;
    char best[12], second[12];
    size_t message_size = strlen(current_view.message);
    uint8_t i;
    memset(wanted, 0, sizeof(wanted));
    for (i = 0U; i < LCD_ROWS; ++i) {
        wanted[i].foreground = COLOR_TEXT;
    }
    wanted[0].foreground = COLOR_CYAN;
    (void)snprintf(wanted[0].text, sizeof(wanted[0].text), "DM-MC02 GESTURE");
    if (current_view.settings_open) {
        compose_settings();
        return;
    }
    if (state > DISPLAY_STATE_ERROR) {
        state = DISPLAY_STATE_ERROR;
    }
    (void)snprintf(wanted[1].text, sizeof(wanted[1].text), "STATE: %s", state_names[state]);
    wanted[1].foreground = state == DISPLAY_STATE_ERROR ? COLOR_RED :
                           state == DISPLAY_STATE_UNKNOWN ? COLOR_YELLOW :
                           state == DISPLAY_STATE_ARMED || state == DISPLAY_STATE_MATCH ?
                           COLOR_GREEN : COLOR_CYAN;
    (void)snprintf(wanted[2].text, sizeof(wanted[2].text), "ACTION: %u/%u    TEMPLATES: %u",
                   (unsigned int)slot + 1U, (unsigned int)class_limit, (unsigned int)current_view.slot_templates[slot]);
    for (i = 0U; i < class_limit; ++i) {
        uint8_t row = (uint8_t)(3U + i / 4U);
        size_t used = strlen(wanted[row].text);
        (void)snprintf(wanted[row].text + used, sizeof(wanted[row].text) - used,
                       "%c%u:%u  ", i == slot ? '>' : ' ', (unsigned int)i + 1U,
                       (unsigned int)current_view.slot_templates[i]);
        wanted[row].foreground = COLOR_MUTED;
    }
    (void)snprintf(wanted[5].text, sizeof(wanted[5].text), "CAPTURE: %u/%u",
                   (unsigned int)current_view.learning_count,
                   (unsigned int)current_view.learning_target);
    score_string(best, sizeof(best), current_view.best_score);
    score_string(second, sizeof(second), current_view.second_score);
    if (current_view.best_slot >= 0 && current_view.best_slot < 8) {
        (void)snprintf(wanted[6].text, sizeof(wanted[6].text), "BEST: %u    SCORE: %s",
                       (unsigned int)current_view.best_slot + 1U, best);
    } else {
        (void)snprintf(wanted[6].text, sizeof(wanted[6].text), "BEST: -    SCORE: %s", best);
    }
    (void)snprintf(wanted[7].text, sizeof(wanted[7].text), "SECOND SCORE: %s", second);
    (void)snprintf(wanted[8].text, sizeof(wanted[8].text), "REJ:%lu DROP:%lu",
                   (unsigned long)capped_count(current_view.rejected_count),
                   (unsigned long)capped_count(current_view.sample_drops));
    wanted[8].foreground = current_view.sample_drops == 0U ? COLOR_MUTED : COLOR_RED;
    (void)snprintf(wanted[9].text, sizeof(wanted[9].text), "PWR:%s IMU:%s FLASH:%s",
                   current_view.power_ok ? "ON" : "OFF",
                   current_view.imu_ok ? "OK" : "ERR", current_view.flash_ok ? "OK" : "ERR");
    wanted[9].foreground = current_view.imu_ok && current_view.flash_ok ? COLOR_GREEN : COLOR_RED;
    memcpy(wanted[10].text, current_view.message,
           message_size > TEXT_LENGTH ? TEXT_LENGTH : message_size);
    if (message_size > TEXT_LENGTH) {
        memcpy(wanted[11].text, current_view.message + TEXT_LENGTH,
               message_size - TEXT_LENGTH);
    }
    wanted[10].foreground = COLOR_YELLOW;
    wanted[11].foreground = COLOR_YELLOW;
    (void)snprintf(wanted[12].text, sizeof(wanted[12].text), "%s",
                   state == DISPLAY_STATE_CONFIRM ? "OK CONFIRM DELETE" :
                   state == DISPLAY_STATE_READY ? "OK RETRY SAVE" :
                   state == DISPLAY_STATE_LEARNING ? "HOLD KEY TO RECORD" :
                   current_view.slot_templates[slot] != 0U ? "OK DELETE ACTION" : "OK LEARN ACTION");
    (void)snprintf(wanted[13].text, sizeof(wanted[13].text), "< > SELECT   UP SETTINGS");
    (void)snprintf(wanted[14].text, sizeof(wanted[14].text), "DOWN CANCEL");
    wanted[12].foreground = COLOR_MUTED;
    wanted[13].foreground = COLOR_MUTED;
    wanted[14].foreground = COLOR_MUTED;
}

static void raster_tile(void)
{
    uint16_t x;
    uint8_t y;
    size_t text_size = strlen(active.text);
    for (y = 0U; y < TILE_HEIGHT; ++y) {
        uint8_t scanline = (uint8_t)(active_tile * TILE_HEIGHT + y);
        for (x = 0U; x < LCD_WIDTH; ++x) {
            uint16_t color = COLOR_BG;
            if (x >= 8U && x < 8U + TEXT_LENGTH * 8U) {
                uint16_t character = (uint16_t)((x - 8U) / 8U);
                if (character < text_size) {
                    uint8_t c = (uint8_t)active.text[character];
                    if (c >= 'a' && c <= 'z') {
                        c = (uint8_t)(c - 'a' + 'A');
                    }
                    if (c < 32U || c > 95U) {
                        c = '?';
                    }
                    if ((display_font[c - 32U][scanline] & (1U << ((x - 8U) % 8U))) != 0U) {
                        color = active.foreground;
                    }
                }
            }
            tile[((uint32_t)y * LCD_WIDTH + x) * 2U] = (uint8_t)(color >> 8);
            tile[((uint32_t)y * LCD_WIDTH + x) * 2U + 1U] = (uint8_t)color;
        }
    }
}

uint8_t display_init(void)
{
    static const uint8_t sequence[] = {
        0x36U, 1U, 0x70U, 0x3AU, 1U, 0x05U,
        0xB2U, 5U, 0x0CU, 0x0CU, 0x00U, 0x33U, 0x33U,
        0xB7U, 1U, 0x35U, 0xBBU, 1U, 0x32U,
        0xC2U, 1U, 0x01U, 0xC3U, 1U, 0x15U, 0xC4U, 1U, 0x20U,
        0xC6U, 1U, 0x0FU, 0xD0U, 2U, 0xA4U, 0xA1U,
        0xE0U, 14U, 0xD0U, 0x08U, 0x0EU, 0x09U, 0x09U, 0x05U, 0x31U,
        0x33U, 0x48U, 0x17U, 0x14U, 0x15U, 0x31U, 0x34U,
        0xE1U, 14U, 0xD0U, 0x08U, 0x0EU, 0x09U, 0x09U, 0x15U, 0x31U,
        0x33U, 0x48U, 0x17U, 0x14U, 0x15U, 0x31U, 0x34U,
        0x21U, 0U, 0x29U, 0U
    };
    size_t position = 0U;
    healthy = 0U;
    dma_active = 0U;
    HAL_GPIO_WritePin(LCD_BLK_GPIO_Port, LCD_BLK_Pin, GPIO_PIN_RESET);
    if ((uintptr_t)tile < 0x24000000UL ||
        (uintptr_t)tile + sizeof(tile) > 0x24050000UL) {
        return 0U;
    }
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LCD_RES_GPIO_Port, LCD_RES_Pin, GPIO_PIN_RESET);
    HAL_Delay(100U);
    HAL_GPIO_WritePin(LCD_RES_GPIO_Port, LCD_RES_Pin, GPIO_PIN_SET);
    HAL_Delay(100U);
    if (!lcd_command(0x11U, NULL, 0U)) {
        return 0U;
    }
    HAL_Delay(120U);
    while (position < sizeof(sequence)) {
        uint8_t command = sequence[position++];
        uint8_t size = sequence[position++];
        if (!lcd_command(command, sequence + position, size)) {
            return 0U;
        }
        position += size;
    }
    memset(&current_view, 0, sizeof(current_view));
    current_view.best_slot = -1;
    current_view.best_score = -1.0f;
    current_view.second_score = -1.0f;
    current_view.learning_target = 3U;
    compose_rows();
    last_compose = HAL_GetTick();
    pending_view = 0U;
    valid_rows = 0U;
    active_row = LCD_ROWS;
    search_row = 0U;
    healthy = 1U;
    return 1U;
}

void display_set_view(const display_view_t *view)
{
    if (view != NULL) {
        current_view = *view;
        current_view.message[sizeof(current_view.message) - 1U] = '\0';
        pending_view = 1U;
    }
}

void display_process(void)
{
    uint32_t now;
    uint8_t checked;
    if (!healthy) {
        return;
    }
    now = HAL_GetTick();
    if (dma_active) {
        /* READY is set by the SPI EOT IRQ, after the last bit has left SPI. */
        if (HAL_SPI_GetState(&hspi1) != HAL_SPI_STATE_READY) {
            if ((uint32_t)(now - transfer_start) > 20U) {
                /* Do not touch CS/DC/buffer if DMA completion was lost. */
                healthy = 0U;
            }
            return;
        }
        HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
        dma_active = 0U;
        if (HAL_SPI_GetError(&hspi1) != HAL_SPI_ERROR_NONE) {
            healthy = 0U;
            return;
        }
        ++active_tile;
        if (active_tile == TEXT_HEIGHT / TILE_HEIGHT) {
            shown[active_row] = active;
            valid_rows |= 1UL << active_row;
            search_row = (uint8_t)((active_row + 1U) % LCD_ROWS);
            active_row = LCD_ROWS;
            if (valid_rows == ALL_ROWS) {
                HAL_GPIO_WritePin(LCD_BLK_GPIO_Port, LCD_BLK_Pin, GPIO_PIN_SET);
            }
        }
    }
    if (pending_view && (uint32_t)(now - last_compose) >= 100U) {
        compose_rows();
        last_compose = now;
        pending_view = 0U;
    }
    if (active_row == LCD_ROWS) {
        for (checked = 0U; checked < LCD_ROWS; ++checked) {
            uint8_t row = (uint8_t)((search_row + checked) % LCD_ROWS);
            if ((valid_rows & (1UL << row)) == 0U ||
                wanted[row].foreground != shown[row].foreground ||
                strcmp(wanted[row].text, shown[row].text) != 0) {
                active_row = row;
                active = wanted[row];
                active_tile = 0U;
                break;
            }
        }
        if (active_row == LCD_ROWS) {
            return;
        }
    }
    raster_tile();
    if (!lcd_window((uint16_t)(active_row * TEXT_HEIGHT + active_tile * TILE_HEIGHT))) {
        healthy = 0U;
        return;
    }
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        SCB_CleanDCache_by_Addr((uint32_t *)tile, (int32_t)sizeof(tile));
    }
    transfer_start = HAL_GetTick();
    if (HAL_SPI_Transmit_DMA(&hspi1, tile, sizeof(tile)) != HAL_OK) {
        HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
        healthy = 0U;
        return;
    }
    dma_active = 1U;
}

uint8_t display_is_ok(void)
{
    return healthy;
}
